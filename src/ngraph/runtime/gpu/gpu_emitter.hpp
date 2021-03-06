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

#pragma once

#include <string>
#include <vector>

#include "ngraph/codegen/code_writer.hpp"
#include "ngraph/node.hpp"
#include "ngraph/runtime/gpu/gpu_external_function.hpp"
#include "ngraph/runtime/gpu/gpu_tensor_view_wrapper.hpp"

#define EMITTER_DECL(op_name)                                                                      \
    emit<op_name>(GPU_ExternalFunction * external_function,                                        \
                  codegen::CodeWriter & writer,                                                    \
                  const ngraph::Node* node,                                                        \
                  const std::vector<GPU_TensorViewWrapper>& args,                                  \
                  const std::vector<GPU_TensorViewWrapper>& out)
namespace ngraph
{
    namespace runtime
    {
        namespace gpu
        {
            class GPU_Emitter
            {
            public:
                template <typename OP>
                static void emit(GPU_ExternalFunction* external_function,
                                 codegen::CodeWriter& writer,
                                 const ngraph::Node* node,
                                 const std::vector<GPU_TensorViewWrapper>& args,
                                 const std::vector<GPU_TensorViewWrapper>& out)
                {
                    throw std::runtime_error("Unimplemented op in GPU emitter for " +
                                             node->get_name());
                }

                static void nop(GPU_ExternalFunction* external_function,
                                codegen::CodeWriter& writer,
                                const ngraph::Node* node,
                                const std::vector<GPU_TensorViewWrapper>& args,
                                const std::vector<GPU_TensorViewWrapper>& out)
                {
                }

                template <typename T>
                static void emit_elementwise(GPU_ExternalFunction* external_function,
                                             codegen::CodeWriter& writer,
                                             const ngraph::Node* node,
                                             const std::vector<GPU_TensorViewWrapper>& args,
                                             const std::vector<GPU_TensorViewWrapper>& out)
                {
                    if (out[0].get_size() == 0)
                    {
                        return;
                    }
                    else if (out.size() > 1)
                    {
                        throw std::runtime_error(
                            "Multi-output elementwise ops are not currently supported.");
                    }
                    auto& cuda_emitter =
                        external_function->get_primitive_emitter()->get_cuda_emitter();

                    writer.block_begin();
                    {
                        std::vector<std::string> dtypes;
                        for (auto& arg : args)
                        {
                            dtypes.push_back(arg.get_type());
                        }
                        dtypes.push_back(out[0].get_type());
                        auto ew_index = cuda_emitter->build_elementwise<T>(
                            external_function->ctx().get(), dtypes, out[0].get_shape());
                        writer << "gpu::invoke_primitive(ctx, " << ew_index << ", ";
                        writer << "std::vector<void*>{" << args.front().get_name();
                        for (size_t i = 1; i < args.size(); i++)
                        {
                            writer << ", " << args[i].get_name();
                        }
                        writer << "}.data(), ";
                        writer << "std::vector<void*>{" << out[0].get_name() << "}.data()";
                        writer << ");\n";
                    }
                    writer.block_end();
                }
            };
            Shape get_padded_shape(const Shape& input_shape,
                                   const Shape& padding_below,
                                   const Shape& padding_above,
                                   const Shape& padding_interior);
        }
    }
}
