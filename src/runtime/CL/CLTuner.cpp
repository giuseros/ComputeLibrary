/*
 * Copyright (c) 2017-2018 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "arm_compute/runtime/CL/CLTuner.h"

#include "arm_compute/core/CL/ICLKernel.h"
#include "arm_compute/core/Error.h"
#include "arm_compute/runtime/CL/CLScheduler.h"

#include <cerrno>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

using namespace arm_compute;

CLTuner::CLTuner(bool tune_new_kernels)
    : real_clEnqueueNDRangeKernel(nullptr), _lws_table(), _queue(), _queue_profiler(), _kernel_event(), _tune_new_kernels(tune_new_kernels)
{
}

bool CLTuner::kernel_event_is_set() const
{
    return _kernel_event() != nullptr;
}
void CLTuner::set_cl_kernel_event(cl_event kernel_event)
{
    _kernel_event = kernel_event;
}

void CLTuner::set_tune_new_kernels(bool tune_new_kernels)
{
    _tune_new_kernels = tune_new_kernels;
}
bool CLTuner::tune_new_kernels() const
{
    return _tune_new_kernels;
}

void CLTuner::tune_kernel_static(ICLKernel &kernel)
{
    ARM_COMPUTE_UNUSED(kernel);
}

void CLTuner::tune_kernel_dynamic(ICLKernel &kernel)
{
    // Get the configuration ID from the kernel
    const std::string &config_id = kernel.config_id();

    // Check if we need to find the Optimal LWS. If config_id is equal to default_config_id, the kernel does not require to be tuned
    if(config_id != arm_compute::default_config_id)
    {
        auto p = _lws_table.find(config_id);

        if(p == _lws_table.end())
        {
            if(_tune_new_kernels)
            {
                // Find the optimal LWS for the kernel
                cl::NDRange opt_lws = find_optimal_lws(kernel);

                // Insert the optimal LWS in the table
                add_lws_to_table(config_id, opt_lws);

                // Set Local-Workgroup-Size
                kernel.set_lws_hint(opt_lws);
            }
        }
        else
        {
            // Set Local-Workgroup-Size
            kernel.set_lws_hint(p->second);
        }
    }
}

void CLTuner::add_lws_to_table(const std::string &kernel_id, cl::NDRange optimal_lws)
{
    _lws_table.emplace(kernel_id, optimal_lws);
}

cl::NDRange CLTuner::find_optimal_lws(ICLKernel &kernel)
{
    if(real_clEnqueueNDRangeKernel == nullptr)
    {
        real_clEnqueueNDRangeKernel = CLSymbols::get().clEnqueueNDRangeKernel_ptr;

        // Get the default queue
        _queue = CLScheduler::get().queue();

        // Check if we can use the OpenCL timer with the default queue
        cl_command_queue_properties props = _queue.getInfo<CL_QUEUE_PROPERTIES>();

        if((props & CL_QUEUE_PROFILING_ENABLE) == 0)
        {
            // Set the queue for profiling
            _queue_profiler = cl::CommandQueue(CLScheduler::get().context(), props | CL_QUEUE_PROFILING_ENABLE);
        }
        else
        {
            _queue_profiler = _queue;
        }
    }
    // Start intercepting enqueues:
    auto interceptor = [this](cl_command_queue command_queue, cl_kernel kernel, cl_uint work_dim, const size_t *gwo, const size_t *gws, const size_t *lws, cl_uint num_events_in_wait_list,
                              const cl_event * event_wait_list, cl_event * event)
    {
        ARM_COMPUTE_ERROR_ON_MSG(event != nullptr, "Not supported");
        ARM_COMPUTE_UNUSED(event);
        if(this->kernel_event_is_set())
        {
            // If the event is already set it means the kernel enqueue is sliced: given that we only time the first slice we can save time by skipping the other enqueues.
            return CL_SUCCESS;
        }
        cl_event tmp;
        cl_int   retval = this->real_clEnqueueNDRangeKernel(command_queue, kernel, work_dim, gwo, gws, lws, num_events_in_wait_list, event_wait_list, &tmp);

        // Set OpenCL event
        this->set_cl_kernel_event(tmp);

        return retval;
    };
    CLSymbols::get().clEnqueueNDRangeKernel_ptr = interceptor;

    cl_ulong min_exec_time = std::numeric_limits<cl_ulong>::max();

    cl::NDRange opt_lws = cl::NullRange;

    const int x_step = std::max(1, kernel.window().x().step());
    const int y_step = std::max(1, kernel.window().y().step());
    const int z_step = std::max(1, kernel.window().z().step());
    const int x_end  = kernel.window().x().end() - kernel.window().x().start() / x_step > 1 ? 16 : 1;
    const int y_end  = kernel.window().y().end() - kernel.window().y().start() / y_step > 1 ? 16 : 1;
    const int z_end  = kernel.window().z().end() - kernel.window().z().start() / z_step > 1 ? 8 : 1;

    // First run using the default LWS
    {
        cl::NDRange lws_test = cl::NullRange;

        kernel.set_lws_hint(lws_test);

        // Run the kernel
        kernel.run(kernel.window(), _queue_profiler);

        _queue_profiler.finish();

        const cl_ulong start = _kernel_event.getProfilingInfo<CL_PROFILING_COMMAND_START>();
        const cl_ulong end   = _kernel_event.getProfilingInfo<CL_PROFILING_COMMAND_END>();
        const cl_ulong diff  = end - start;
        _kernel_event        = nullptr;

        min_exec_time = diff;
    }

    for(int z = 1; z <= z_end; ++z)
    {
        for(int y = 1; y <= y_end; ++y)
        {
            for(int x = 1; x <= x_end; ++x)
            {
                cl::NDRange lws_test = cl::NDRange(x, y, z);

                const bool invalid_lws = (x * y * z > static_cast<int>(kernel.get_max_workgroup_size())) || (x == 1 && y == 1 && z == 1);

                if(invalid_lws)
                {
                    continue;
                }

                //Set the Local-Workgroup-Size
                kernel.set_lws_hint(lws_test);

                // Run the kernel
                kernel.run(kernel.window(), _queue_profiler);

                _queue_profiler.finish();

                const cl_ulong start = _kernel_event.getProfilingInfo<CL_PROFILING_COMMAND_START>();
                const cl_ulong end   = _kernel_event.getProfilingInfo<CL_PROFILING_COMMAND_END>();
                const cl_ulong diff  = end - start;
                _kernel_event        = nullptr;

                // Check the execution time
                if(diff < min_exec_time)
                {
                    min_exec_time = diff;
                    opt_lws       = cl::NDRange(x, y, z);
                }
            }
        }
    }

    // Restore real function
    CLSymbols::get().clEnqueueNDRangeKernel_ptr = real_clEnqueueNDRangeKernel;

    return opt_lws;
}

void CLTuner::import_lws_table(const std::unordered_map<std::string, cl::NDRange> &lws_table)
{
    _lws_table.clear();
    _lws_table = lws_table;
}

const std::unordered_map<std::string, cl::NDRange> &CLTuner::lws_table() const
{
    return _lws_table;
}

void CLTuner::load_from_file(const std::string &filename)
{
    std::ifstream fs;
    fs.exceptions(std::ifstream::badbit);
    fs.open(filename, std::ios::in);
    if(!fs.is_open())
    {
        ARM_COMPUTE_ERROR("Failed to open '%s' (%s [%d])", filename.c_str(), strerror(errno), errno);
    }
    std::string line;
    while(!std::getline(fs, line).fail())
    {
        std::istringstream ss(line);
        std::string        token;
        if(std::getline(ss, token, ';').fail())
        {
            ARM_COMPUTE_ERROR("Malformed row '%s' in %s (Should be of the form 'kernel_id;lws[0];lws[1];lws[2]')", ss.str().c_str(), filename.c_str());
        }
        std::string kernel_id = token;
        cl::NDRange lws(1, 1, 1);
        for(int i = 0; i < 3; i++)
        {
            if(std::getline(ss, token, ';').fail())
            {
                ARM_COMPUTE_ERROR("Malformed row '%s' in %s (Should be of the form 'kernel_id;lws[0];lws[1];lws[2]')", ss.str().c_str(), filename.c_str());
            }
            lws.get()[i] = support::cpp11::stoi(token);
        }

        // If all dimensions are 0: reset to NullRange (i.e nullptr)
        if(lws[0] == 0 && lws[1] == 0 && lws[2] == 0)
        {
            lws = cl::NullRange;
        }
        add_lws_to_table(kernel_id, lws);
    }
    fs.close();
}

void CLTuner::save_to_file(const std::string &filename) const
{
    std::ofstream fs;
    fs.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    fs.open(filename, std::ios::out);
    for(auto kernel_data : _lws_table)
    {
        fs << kernel_data.first << ";" << kernel_data.second[0] << ";" << kernel_data.second[1] << ";" << kernel_data.second[2] << std::endl;
    }
    fs.close();
}
