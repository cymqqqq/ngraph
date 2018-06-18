/*******************************************************************************
* Copyright 2017-2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

extern "C"
{
    #include "ngraph/ngraph.h"
}

#include <mutex>

#include "ngraph/runtime/backend.hpp"

static std::mutex s_init_mutex;
static int s_init_count = 0;

extern "C" void ngraph_initialize()
{
    std::lock_guard<std::mutex> guard(s_init_mutex);
    if (s_init_count++ == 0)
    {
        // All initializers go here
        ngraph::runtime::Backend::initialize();
    }
}

extern "C" void ngraph_finalize()
{
    std::lock_guard<std::mutex> guard(s_init_mutex);
    if (s_init_count <= 0)
    {
        throw std::runtime_error("ngraph_finalize must be called exactly once for every ngraph_initialize call");
    }
    if (--s_init_count == 0)
    {
        // All finalizers go here
        ngraph::runtime::Backend::finalize();
    }
}
