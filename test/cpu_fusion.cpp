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

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <list>
#include <memory>

#include "gtest/gtest.h"
#include "ngraph/autodiff/adjoints.hpp"
#include "ngraph/file_util.hpp"
#include "ngraph/graph_util.hpp"
#include "ngraph/log.hpp"
#include "ngraph/ngraph.hpp"
#include "ngraph/op/batch_norm.hpp"
#include "ngraph/op/get_output_element.hpp"
#include "ngraph/op/max_pool.hpp"
#include "ngraph/op/parameter.hpp"
#include "ngraph/op/relu.hpp"
#include "ngraph/op/sum.hpp"
#include "ngraph/pass/graph_rewrite.hpp"
#include "ngraph/pass/manager.hpp"
#include "ngraph/pass/reshape_elimination.hpp"
#include "ngraph/pass/visualize_tree.hpp"
#include "ngraph/pattern/matcher.hpp"
#include "ngraph/pattern/op/label.hpp"
#include "ngraph/pattern/op/skip.hpp"
#include "ngraph/runtime/cpu/cpu_layout_descriptor.hpp"
#include "ngraph/runtime/cpu/op/batch_norm_relu.hpp"
#include "ngraph/runtime/cpu/op/conv_bias.hpp"
#include "ngraph/runtime/cpu/op/conv_relu.hpp"
#include "ngraph/runtime/cpu/op/convert_layout.hpp"
#include "ngraph/runtime/cpu/op/lstm.hpp"
#include "ngraph/runtime/cpu/op/matmul_bias.hpp"
#include "ngraph/runtime/cpu/op/rnn.hpp"
#include "ngraph/runtime/cpu/op/sigmoid.hpp"
#include "ngraph/runtime/cpu/pass/cpu_concat_inputs.hpp"
#include "ngraph/runtime/cpu/pass/cpu_fusion.hpp"
#include "ngraph/runtime/cpu/pass/cpu_post_layout_optimizations.hpp"
#include "ngraph/runtime/cpu/pass/cpu_rnn_fusion.hpp"
#include "ngraph/runtime/cpu/pass/cpu_rnn_mat_fusion.hpp"
#include "ngraph/runtime/cpu/pass/cpu_workspace_insertion.hpp"
#include "ngraph/serializer.hpp"
#include "ngraph/util.hpp"
#include "nlohmann/json.hpp"
#include "util/all_close.hpp"
#include "util/autodiff/backprop_function.hpp"
#include "util/autodiff/numeric_compare.hpp"
#include "util/matcher.hpp"
#include "util/random.hpp"
#include "util/test_tools.hpp"

#include "util/random.hpp"

using namespace ngraph;
using namespace std;

TEST(cpu_fusion, gemm_pattern)
{
    Shape shape_w{2, 4};
    Shape shape_x{4, 1};
    Shape shape_b{1};
    auto A = make_shared<op::Parameter>(element::f32, shape_w);
    auto B = make_shared<op::Parameter>(element::f32, shape_x);
    auto C = make_shared<op::Parameter>(element::f32, shape_b);

    auto dot = make_shared<op::Dot>(A, B);
    auto broadcast = make_shared<op::Broadcast>(C, dot->get_shape(), AxisSet{0});
    auto add = dot + broadcast;

    auto W = std::make_shared<pattern::op::Label>(A);
    auto x = std::make_shared<pattern::op::Label>(B);

    auto reshape_pred = [](std::shared_ptr<Node> n) {
        return static_cast<bool>(std::dynamic_pointer_cast<op::Reshape>(n));
    };

    auto skip_w = std::make_shared<pattern::op::Skip>(W, reshape_pred);
    auto skip_x = std::make_shared<pattern::op::Skip>(x, reshape_pred);

    auto pdot = make_shared<op::Dot>(skip_w, skip_x);
    auto b = std::make_shared<pattern::op::Label>(C);
    auto pbroadcast = make_shared<op::Broadcast>(b, dot->get_shape(), AxisSet{0});
    auto padd = pdot + pbroadcast;

    TestMatcher n(nullptr);
    ASSERT_TRUE(n.match(padd, add));
    ASSERT_EQ(n.get_pattern_map()[W], A);
    ASSERT_EQ(n.get_pattern_map()[x], B);
    ASSERT_EQ(n.get_pattern_map()[b], C);

    auto reshape_w = make_shared<op::Reshape>(A, AxisVector{1, 0}, W->get_shape());
    auto reshape_x = make_shared<op::Reshape>(B, AxisVector{1, 0}, x->get_shape());
    auto re_dot = make_shared<op::Dot>(reshape_w, reshape_x);
    auto re_add = re_dot + broadcast;
    ASSERT_TRUE(n.match(padd, re_add));
    ASSERT_EQ(n.get_pattern_map()[W], A);
    ASSERT_EQ(n.get_pattern_map()[x], B);
    ASSERT_EQ(n.get_pattern_map()[b], C);

    auto cg = make_shared<op::MatmulBias>(
        W, x, C, W->get_shape(), x->get_shape(), false, false, AxisSet{0});
}

TEST(cpu_fusion, gemm_cpu_broadcast_row)
{
    Shape shapeA{3, 2};
    Shape shapeB{2, 3};
    Shape shapeC{2, 2};
    auto A = make_shared<op::Parameter>(element::f32, shapeA);
    auto B = make_shared<op::Parameter>(element::f32, shapeB);

    auto bias = op::Constant::create<float>(element::f32, Shape{2}, std::vector<float>{2.0f, 3.0f});

    auto cg = make_shared<op::MatmulBias>(
        A, B, bias, A->get_shape(), B->get_shape(), true, true, AxisSet{0});

    auto f = make_shared<Function>(cg, op::ParameterVector{A, B});

    auto backend = runtime::Backend::create("CPU");

    shared_ptr<runtime::TensorView> a = backend->create_tensor(element::f32, shapeA);
    shared_ptr<runtime::TensorView> b = backend->create_tensor(element::f32, shapeB);
    shared_ptr<runtime::TensorView> result = backend->create_tensor(element::f32, shapeC);

    vector<float> dataA{1.0f, 4.0f, 1.0f, 4.0f, 1.0f, 4.0f};
    vector<float> dataB{3.0f, 3.0f, 3.0f, 9.0f, 9.0f, 9.0f};
    copy_data(a, dataA);
    copy_data(b, dataB);

    backend->call(f, {result}, {a, b});
    vector<float> expected{11, 30, 38, 111};
    EXPECT_EQ(read_vector<float>(result), expected);
}

TEST(cpu_fusion, gemm_cpu_broadcast_column)
{
    Shape shapeA{3, 2};
    Shape shapeB{2, 3};
    Shape shapeC{2, 2};
    auto A = make_shared<op::Parameter>(element::f32, shapeA);
    auto B = make_shared<op::Parameter>(element::f32, shapeB);

    auto bias = op::Constant::create<float>(element::f32, Shape{2}, std::vector<float>{2.0f, 3.0f});

    auto cg = make_shared<op::MatmulBias>(
        A, B, bias, A->get_shape(), B->get_shape(), true, true, AxisSet{1});

    auto f = make_shared<Function>(cg, op::ParameterVector{A, B});

    auto backend = runtime::Backend::create("CPU");

    shared_ptr<runtime::TensorView> a = backend->create_tensor(element::f32, shapeA);
    shared_ptr<runtime::TensorView> b = backend->create_tensor(element::f32, shapeB);
    shared_ptr<runtime::TensorView> result = backend->create_tensor(element::f32, shapeC);

    vector<float> dataA{1.0f, 4.0f, 1.0f, 4.0f, 1.0f, 4.0f};
    vector<float> dataB{3.0f, 3.0f, 3.0f, 9.0f, 9.0f, 9.0f};
    copy_data(a, dataA);
    copy_data(b, dataB);

    backend->call(f, {result}, {a, b});
    vector<float> expected{11, 29, 39, 111};
    EXPECT_EQ(read_vector<float>(result), expected);
}

TEST(cpu_fusion, gemm_cpu_broadcast_matrix)
{
    Shape shapeA{3, 2};
    Shape shapeB{2, 3};
    Shape shapeC{2, 2};
    auto A = make_shared<op::Parameter>(element::f32, shapeA);
    auto B = make_shared<op::Parameter>(element::f32, shapeB);

    auto reshape_w = make_shared<op::Reshape>(A, AxisVector{1, 0}, Shape{2, 3});
    auto reshape_x = make_shared<op::Reshape>(B, AxisVector{1, 0}, Shape{3, 2});

    auto one = op::Constant::create<float>(element::f32, Shape{}, std::vector<float>{1.0f});

    auto broadcast = make_shared<op::Broadcast>(one, shapeC, AxisSet{0, 1});
    auto cg = make_shared<op::MatmulBias>(
        A, B, one, A->get_shape(), B->get_shape(), true, true, AxisSet{0, 1});

    auto f = make_shared<Function>(cg, op::ParameterVector{A, B});

    auto backend = runtime::Backend::create("CPU");

    shared_ptr<runtime::TensorView> a = backend->create_tensor(element::f32, shapeA);
    shared_ptr<runtime::TensorView> b = backend->create_tensor(element::f32, shapeB);
    shared_ptr<runtime::TensorView> result = backend->create_tensor(element::f32, shapeC);

    vector<float> dataA{1.0f, 4.0f, 1.0f, 4.0f, 1.0f, 4.0f};
    vector<float> dataB{3.0f, 3.0f, 3.0f, 9.0f, 9.0f, 9.0f};
    copy_data(a, dataA);
    copy_data(b, dataB);

    backend->call(f, {result}, {a, b});
    vector<float> expected{10, 28, 37, 109};
    ASSERT_TRUE(read_vector<float>(result) == expected);
}

TEST(cpu_fusion, gemm_cpu_no_bias)
{
    auto shapeA = Shape{3, 2};
    auto shapeB = Shape{2, 3};
    auto shapeC = Shape{2, 2};
    auto A = make_shared<op::Parameter>(element::f32, shapeA);
    auto B = make_shared<op::Parameter>(element::f32, shapeB);

    auto reshape_w = make_shared<op::Reshape>(A, AxisVector{1, 0}, Shape{2, 3});
    auto reshape_x = make_shared<op::Reshape>(B, AxisVector{1, 0}, Shape{3, 2});

    auto cg =
        make_shared<op::MatmulBias>(A, B, nullptr, A->get_shape(), B->get_shape(), true, true);

    auto f = make_shared<Function>(cg, op::ParameterVector{A, B});

    auto backend = runtime::Backend::create("CPU");

    shared_ptr<runtime::TensorView> a = backend->create_tensor(element::f32, shapeA);
    shared_ptr<runtime::TensorView> b = backend->create_tensor(element::f32, shapeB);
    shared_ptr<runtime::TensorView> result = backend->create_tensor(element::f32, shapeC);

    vector<float> dataA{1.0f, 4.0f, 1.0f, 4.0f, 1.0f, 4.0f};
    vector<float> dataB{3.0f, 3.0f, 3.0f, 9.0f, 9.0f, 9.0f};
    copy_data(a, dataA);
    copy_data(b, dataB);

    backend->call(f, {result}, {a, b});
    vector<float> expected{9, 27, 36, 108};
    ASSERT_TRUE(read_vector<float>(result) == expected);
}

TEST(cpu_fusion, cpu_fusion_pass_basic)
{
    Shape shape{};
    Shape shape_w{2, 4};
    Shape shape_x{4, 1};
    Shape shape_b{1};
    auto A = make_shared<op::Parameter>(element::f32, shape_w);
    auto B = make_shared<op::Parameter>(element::f32, shape_x);
    auto C = make_shared<op::Parameter>(element::f32, shape_b);

    auto dot = make_shared<op::Dot>(A, B);
    auto broadcast = make_shared<op::Broadcast>(C, dot->get_shape(), AxisSet{0});
    auto add = dot + broadcast;
    auto graph = make_shared<op::Abs>(add);
    pass::Manager pass_manager;
    pass_manager.register_pass<runtime::cpu::pass::CPUFusion>(
        runtime::cpu::pass::CPUFusion::REGULAR_FUSIONS);
    auto func = make_shared<Function>(graph, op::ParameterVector{A, B, C});
    pass_manager.run_passes(func);
    ASSERT_NE(std::dynamic_pointer_cast<op::MatmulBias>(graph->get_argument(0)), nullptr);
}

TEST(cpu_fusion, commutative_matmul_bias)
{
    Shape shape{};
    Shape shape_w{2, 4};
    Shape shape_x{4, 1};
    Shape shape_b{1};
    auto A = make_shared<op::Parameter>(element::f32, shape_w);
    auto B = make_shared<op::Parameter>(element::f32, shape_x);
    auto C = make_shared<op::Parameter>(element::f32, shape_b);

    auto dot = make_shared<op::Dot>(A, B);
    auto broadcast = make_shared<op::Broadcast>(C, dot->get_shape(), AxisSet{0});
    auto add = broadcast + dot;
    auto graph = make_shared<op::Abs>(add);
    pass::Manager pass_manager;
    pass_manager.register_pass<runtime::cpu::pass::CPUFusion>(
        runtime::cpu::pass::CPUFusion::REGULAR_FUSIONS);
    auto func = make_shared<Function>(graph, op::ParameterVector{A, B, C});
    pass_manager.run_passes(func);
    ASSERT_NE(std::dynamic_pointer_cast<op::MatmulBias>(graph->get_argument(0)), nullptr);
}

TEST(cpu_fusion, cpu_fusion_pass_matmul_bias)
{
    Shape shape_w{2, 4};
    Shape shape_x{4, 1};
    Shape shape_b{1};
    auto W = make_shared<op::Parameter>(element::f32, shape_w);
    auto x = make_shared<op::Parameter>(element::f32, shape_x);
    auto b = make_shared<op::Parameter>(element::f32, shape_b);

    auto mmb = std::make_shared<op::MatmulBias>(
        W, x, nullptr, W->get_shape(), x->get_shape(), false, false);
    auto broadcast = std::make_shared<op::Broadcast>(b, mmb->get_shape(), AxisSet{0});
    auto add = mmb + broadcast;

    auto graph = make_shared<op::Abs>(add);
    pass::Manager pass_manager;
    pass_manager.register_pass<runtime::cpu::pass::CPUFusion>(
        runtime::cpu::pass::CPUFusion::REGULAR_FUSIONS);
    auto func = make_shared<Function>(graph, op::ParameterVector{W, x, b});
    pass_manager.run_passes(func);
    auto gmm = graph->get_argument(0);
    ASSERT_TRUE(std::dynamic_pointer_cast<op::MatmulBias>(gmm));
    ASSERT_EQ(gmm->get_argument(2), b);
}

TEST(cpu_fusion, cpu_fusion_pass_matmul_no_bias)
{
    Shape shape_w{4, 2};
    Shape shape_x{1, 4};
    auto W = make_shared<op::Parameter>(element::f32, shape_w);
    auto x = make_shared<op::Parameter>(element::f32, shape_x);

    auto reshape_w = std::make_shared<op::Reshape>(W, AxisVector{1, 0}, Shape{2, 4});
    auto reshape_x = std::make_shared<op::Reshape>(x, AxisVector{1, 0}, Shape{4, 1});
    auto re_dot = make_shared<op::Dot>(reshape_w, reshape_x);
    auto graph = make_shared<op::Abs>(re_dot);

    pass::Manager pass_manager;
    pass_manager.register_pass<runtime::cpu::pass::CPUFusion>(
        runtime::cpu::pass::CPUFusion::REGULAR_FUSIONS);
    auto func = make_shared<Function>(graph, op::ParameterVector{W, x});
    pass_manager.run_passes(func);
    size_t mmb = count_ops_of_type<op::MatmulBias>(func);
    ASSERT_EQ(mmb, 1);
}

TEST(cpu_fusion, gemm_mlp)
{
    const string json_path = file_util::path_join(SERIALIZED_ZOO, "mxnet/mnist_mlp_forward.json");
    const string json_string = file_util::read_file_to_string(json_path);
    stringstream ss(json_string);
    shared_ptr<Function> func = ngraph::deserialize(ss);
    pass::Manager pass_manager;
    pass_manager.register_pass<runtime::cpu::pass::CPUFusion>(
        runtime::cpu::pass::CPUFusion::REGULAR_FUSIONS);
    pass_manager.run_passes(func);
    auto mmbs = count_ops_of_type<op::MatmulBias>(func);
    ASSERT_EQ(mmbs, 3);
}

TEST(cpu_fusion, fuse_fprop_bn)
{
    pass::Manager pass_manager;
    pass_manager.register_pass<pass::VisualizeTree>("bn_fprop_before_fusion.png");
    pass_manager.register_pass<ngraph::pass::ReshapeElimination>();
    pass_manager.register_pass<runtime::cpu::pass::CPUFusion>(
        runtime::cpu::pass::CPUFusion::REGULAR_FUSIONS);
    pass_manager.register_pass<pass::VisualizeTree>("bn_fprop_after_fusion.png");
    const string json_path = file_util::path_join(SERIALIZED_ZOO, "mxnet/bn_fprop_b2c3h2w2.json");
    const string json_string = file_util::read_file_to_string(json_path);
    stringstream ss(json_string);
    shared_ptr<Function> func = ngraph::deserialize(ss);
    pass_manager.run_passes(func);
    size_t ccg = count_ops_of_type<op::BatchNorm>(func);
    ASSERT_EQ(ccg, 1);
}

TEST(cpu_fusion, zero_padded_reshaped_conv)
{
    auto X = make_shared<op::Parameter>(element::f32, Shape{1, 2, 2, 1});
    auto F = make_shared<op::Parameter>(element::f32, Shape{1, 1, 1, 1});

    auto pad_value = op::Constant::create<float>(element::f32, Shape{}, std::vector<float>{0.0f});

    auto pad =
        make_shared<op::Pad>(X, pad_value, Shape{0, 1, 0, 0}, Shape{0, 0, 1, 0}, Shape{0, 0, 0, 0});

    auto reshape = make_shared<op::Reshape>(pad, AxisVector{0, 3, 1, 2}, Shape{1, 1, 3, 3});

    auto conv = make_shared<op::Convolution>(reshape,
                                             F,
                                             Strides{1, 1},
                                             Strides{1, 1},
                                             CoordinateDiff{0, 0},
                                             CoordinateDiff{0, 0},
                                             Strides{1, 1});

    auto func = make_shared<Function>(conv, op::ParameterVector{X, F});

    ASSERT_EQ(count_ops_of_type<op::Pad>(func), 1);

    auto backend = runtime::Backend::create("CPU");
    backend->compile(func);

    ASSERT_EQ(count_ops_of_type<op::Pad>(func), 0);
}

TEST(cpu_fusion, zero_padded_conv)
{
    auto X = make_shared<op::Parameter>(element::f32, Shape{1, 1, 2, 2});
    auto F = make_shared<op::Parameter>(element::f32, Shape{1, 1, 1, 1});

    auto pad_value = op::Constant::create<float>(element::f32, Shape{}, std::vector<float>{0.0f});

    auto pad =
        make_shared<op::Pad>(X, pad_value, Shape{0, 0, 0, 1}, Shape{0, 0, 1, 0}, Shape{0, 0, 0, 0});

    auto conv = make_shared<op::Convolution>(pad,
                                             F,
                                             Strides{1, 1},
                                             Strides{1, 1},
                                             CoordinateDiff{0, 0},
                                             CoordinateDiff{0, 0},
                                             Strides{1, 1});

    auto func = make_shared<Function>(conv, op::ParameterVector{X, F});

    ASSERT_EQ(count_ops_of_type<op::Pad>(func), 1);

    auto backend = runtime::Backend::create("CPU");
    backend->compile(func);

    ASSERT_EQ(count_ops_of_type<op::Pad>(func), 0);
}

TEST(cpu_fusion, non_zero_padded_conv)
{
    auto X = make_shared<op::Parameter>(element::f32, Shape{1, 1, 2, 2});
    auto F = make_shared<op::Parameter>(element::f32, Shape{1, 1, 1, 1});

    auto pad_value = op::Constant::create<float>(element::f32, Shape{}, std::vector<float>{1.0f});

    auto pad =
        make_shared<op::Pad>(X, pad_value, Shape{0, 0, 0, 1}, Shape{0, 0, 1, 0}, Shape{0, 0, 0, 0});

    auto conv = make_shared<op::Convolution>(pad,
                                             F,
                                             Strides{1, 1},
                                             Strides{1, 1},
                                             CoordinateDiff{0, 0},
                                             CoordinateDiff{0, 0},
                                             Strides{1, 1});

    auto func = make_shared<Function>(conv, op::ParameterVector{X, F});

    ASSERT_EQ(count_ops_of_type<op::Pad>(func), 1);

    auto backend = runtime::Backend::create("CPU");
    backend->compile(func);

    ASSERT_EQ(count_ops_of_type<op::Pad>(func), 1);
}

TEST(cpu_fusion, zero_padded_conv_backprop_filters)
{
    auto X = make_shared<op::Parameter>(element::f32, Shape{1, 1, 2, 2});
    auto F = make_shared<op::Parameter>(element::f32, Shape{1, 1, 2, 2});

    auto pad_value = op::Constant::create<float>(element::f32, Shape{}, std::vector<float>{0.0f});

    auto pad =
        make_shared<op::Pad>(X, pad_value, Shape{0, 0, 0, 1}, Shape{0, 0, 1, 0}, Shape{0, 0, 0, 0});

    auto conv = make_shared<op::ConvolutionBackpropFilters>(pad,
                                                            Shape{1, 1, 2, 2},
                                                            F,
                                                            Strides{1, 1},
                                                            Strides{1, 1},
                                                            CoordinateDiff{0, 0},
                                                            CoordinateDiff{0, 0},
                                                            Strides{1, 1});

    auto func = make_shared<Function>(conv, op::ParameterVector{X, F});

    ASSERT_EQ(count_ops_of_type<op::Pad>(func), 1);

    auto backend = runtime::Backend::create("CPU");
    backend->compile(func);

    ASSERT_EQ(count_ops_of_type<op::Pad>(func), 0);
}

TEST(cpu_fusion, fuse_conv_bias)
{
    pass::Manager pass_manager;
    pass_manager.register_pass<ngraph::pass::ReshapeElimination>();
    pass_manager.register_pass<runtime::cpu::pass::CPUFusion>(
        runtime::cpu::pass::CPUFusion::DIFFERENTIABLE_FUSIONS);
    const string json_path = file_util::path_join(SERIALIZED_ZOO, "conv_bias.json");
    const string json_string = file_util::read_file_to_string(json_path);
    stringstream ss(json_string);
    shared_ptr<Function> func = ngraph::deserialize(ss);
    pass_manager.run_passes(func);
    size_t cb = count_ops_of_type<op::ConvolutionBias>(func);
    ASSERT_GT(cb, 0);
}

struct ConvolutionBiasTestData
{
    size_t n{0};
    size_t c{0};
    size_t filter{0};
    size_t kernel_size{0};
    size_t w{0};
    size_t h{0};
    shared_ptr<runtime::TensorView> data_val;
    shared_ptr<runtime::TensorView> weights_val;
    shared_ptr<runtime::TensorView> bias_val;
    shared_ptr<runtime::TensorView> result_val;
    shared_ptr<runtime::TensorView> delta_val;
    shared_ptr<runtime::TensorView> d_data_val;
    shared_ptr<runtime::TensorView> d_weights_val;
    shared_ptr<runtime::TensorView> d_bias_val;
    vector<float> expected_result_val;
    vector<float> expected_d_data_val;
    vector<float> expected_d_weights_val;
    vector<float> expected_d_bias_val;

    Shape data_shape;
    Shape weights_shape;
    Shape bias_shape;
    Shape result_shape;
    shared_ptr<op::Parameter> data;
    shared_ptr<op::Parameter> weights;
    shared_ptr<op::Parameter> bias;
    shared_ptr<op::Parameter> delta;

    void n1c1h3w3(shared_ptr<runtime::Backend> backend)
    {
        n = 1;
        c = 1;
        filter = 1;
        kernel_size = 3;
        w = 3;
        h = w;

        data_shape = Shape{n, c, h, w};
        data = make_shared<op::Parameter>(element::f32, data_shape);
        weights_shape = Shape{filter, c, kernel_size, kernel_size};
        weights = make_shared<op::Parameter>(element::f32, weights_shape);
        bias_shape = Shape{filter};
        bias = make_shared<op::Parameter>(element::f32, bias_shape);
        result_shape = Shape{n, filter, 1, 1};

        data_val = backend->create_tensor(element::f32, data_shape);
        copy_data(data_val,
                  vector<float>{-0.67765152f,
                                0.10073948f,
                                0.57595438f,
                                -0.3469252f,
                                -0.22134334f,
                                -1.80471897f,
                                -0.80642909f,
                                1.22033095f,
                                2.23235631f});
        weights_val = backend->create_tensor(element::f32, weights_shape);
        copy_data(weights_val,
                  vector<float>{0.20070229f,
                                -0.54968649f,
                                -0.19819015f,
                                -0.38577855f,
                                1.37109005f,
                                -0.23789984f,
                                0.14867957f,
                                -0.49851316f,
                                -0.84815776f});
        bias_val = backend->create_tensor(element::f32, bias_shape);
        copy_data(bias_val, vector<float>{0.07811152f});

        result_val = backend->create_tensor(element::f32, result_shape);
        copy_data(result_val, vector<float>{0});

        delta = make_shared<op::Parameter>(element::f32, result_shape);
        delta_val = backend->create_tensor(element::f32, result_shape);
        copy_data(delta_val, vector<float>{-2.58936238f});

        d_data_val = backend->create_tensor(element::f32, data_shape);
        copy_data(d_data_val, vector<float>{0, 0, 0, 0, 0, 0, 0, 0, 0});

        d_weights_val = backend->create_tensor(element::f32, weights_shape);
        copy_data(d_weights_val, vector<float>{0, 0, 0, 0, 0, 0, 0, 0, 0});

        d_bias_val = backend->create_tensor(element::f32, bias_shape);
        copy_data(d_bias_val, vector<float>{0});

        expected_result_val = vector<float>{-2.58936238f};
        expected_d_data_val = vector<float>{-0.51969099f,
                                            1.42333758f,
                                            0.5131861f,
                                            0.99892044f,
                                            -3.5502491f,
                                            0.61600888f,
                                            -0.3849853f,
                                            1.29083121f,
                                            2.19618773f};
        expected_d_weights_val = vector<float>{1.7546854f,
                                               -0.26085103f,
                                               -1.49135458f,
                                               0.89831507f,
                                               0.57313812f,
                                               4.67307138f,
                                               2.08813715f,
                                               -3.15987897f,
                                               -5.7803793f};
        expected_d_bias_val = vector<float>{-2.58936238f};
    }
};

TEST(cpu_fusion, conv_bias_fprop_n1c1h3w3)
{
    auto backend = runtime::Backend::create("CPU");

    ConvolutionBiasTestData conv_test;
    conv_test.n1c1h3w3(backend);

    auto convolution = make_shared<op::Convolution>(conv_test.data, conv_test.weights);
    auto convolution_bias = make_shared<op::ConvolutionBias>(convolution, conv_test.bias);

    auto f = make_shared<Function>(
        convolution_bias, op::ParameterVector{conv_test.data, conv_test.weights, conv_test.bias});

    backend->call(
        f, {conv_test.result_val}, {conv_test.data_val, conv_test.weights_val, conv_test.bias_val});
    auto result_vec = read_vector<float>(conv_test.result_val);

    EXPECT_TRUE(
        test::all_close(conv_test.expected_result_val, read_vector<float>(conv_test.result_val)));
}

TEST(cpu_fusion, conv_bias_bprop_n1c1h3w3)
{
    auto backend = runtime::Backend::create("CPU");

    ConvolutionBiasTestData conv_test;
    conv_test.n1c1h3w3(backend);

    auto convolution = make_shared<op::Convolution>(conv_test.data, conv_test.weights);
    auto convolution_bias = make_shared<op::ConvolutionBias>(convolution, conv_test.bias);

    auto f = make_shared<Function>(
        convolution_bias, op::ParameterVector{conv_test.data, conv_test.weights, conv_test.bias});

    ngraph::autodiff::Adjoints adjoints(NodeVector{convolution_bias}, NodeVector{conv_test.delta});

    auto d_data = adjoints.backprop_node(conv_test.data);
    auto d_weights = adjoints.backprop_node(conv_test.weights);
    auto d_bias = adjoints.backprop_node(conv_test.bias);

    auto df = make_shared<Function>(
        NodeVector{d_data, d_weights, d_bias},
        op::ParameterVector{conv_test.data, conv_test.weights, conv_test.bias, conv_test.delta});

    backend->call(
        df,
        {conv_test.d_data_val, conv_test.d_weights_val, conv_test.d_bias_val},
        {conv_test.data_val, conv_test.weights_val, conv_test.bias_val, conv_test.delta_val});

    EXPECT_TRUE(
        test::all_close(conv_test.expected_d_data_val, read_vector<float>(conv_test.d_data_val)));
    EXPECT_TRUE(test::all_close(conv_test.expected_d_weights_val,
                                read_vector<float>(conv_test.d_weights_val)));
    EXPECT_TRUE(
        test::all_close(conv_test.expected_d_bias_val, read_vector<float>(conv_test.d_bias_val)));
}

TEST(cpu_fusion, sigmoid_fprop_fusion)
{
    pass::Manager pass_manager;
    pass_manager.register_pass<runtime::cpu::pass::CPUFusion>(
        runtime::cpu::pass::CPUFusion::REGULAR_FUSIONS);
    const string json_path = file_util::path_join(SERIALIZED_ZOO, "mxnet/Graph_fprop_sigmoid.json");
    const string json_string = file_util::read_file_to_string(json_path);
    stringstream ss(json_string);
    shared_ptr<Function> func = ngraph::deserialize(ss);
    pass_manager.run_passes(func);
    size_t ccg = count_ops_of_type<op::Sigmoid>(func);
    ASSERT_EQ(ccg, 1);
}

TEST(cpu_fusion, sigmoid_n1c1h2w2)
{
    auto input = make_shared<op::Parameter>(element::f32, Shape{1, 1, 2, 2});
    auto sigmoid_node = make_shared<op::Sigmoid>(input);
    auto func = make_shared<Function>(sigmoid_node, op::ParameterVector{input});

    auto backend = runtime::Backend::create("CPU");

    shared_ptr<runtime::TensorView> a = backend->create_tensor(element::f32, input->get_shape());
    shared_ptr<runtime::TensorView> result =
        backend->create_tensor(element::f32, input->get_shape());

    vector<float> dataA{1.0f, 4.0f, 1.0f, 4.0f};
    copy_data(a, dataA);

    backend->call(func, {result}, {a});
    vector<float> expected{0.73105858f, 0.98201379f, 0.73105858f, 0.98201379f};
    ASSERT_TRUE(read_vector<float>(result) == expected);
}

TEST(cpu_fusion, sigmoid_n1c1h4)
{
    auto input = make_shared<op::Parameter>(element::f32, Shape{1, 1, 4});
    auto sigmoid_node = make_shared<op::Sigmoid>(input);
    auto func = make_shared<Function>(sigmoid_node, op::ParameterVector{input});

    auto backend = runtime::Backend::create("CPU");

    shared_ptr<runtime::TensorView> a = backend->create_tensor(element::f32, input->get_shape());
    shared_ptr<runtime::TensorView> result =
        backend->create_tensor(element::f32, input->get_shape());

    vector<float> dataA{1.0f, 4.0f, 1.0f, 4.0f};
    copy_data(a, dataA);

    backend->call(func, {result}, {a});
    vector<float> expected{0.73105858f, 0.98201379f, 0.73105858f, 0.98201379f};
    ASSERT_TRUE(read_vector<float>(result) == expected);
}

TEST(cpu_fusion, sigmoid_bprop_fusion)
{
    const string json_path = file_util::path_join(SERIALIZED_ZOO, "mxnet/Graph_fprop_sigmoid.json");
    const string json_string = file_util::read_file_to_string(json_path);
    stringstream ss(json_string);
    shared_ptr<Function> func = ngraph::deserialize(ss);
    auto df = autodiff::backprop_function(func);
    auto backend = runtime::Backend::create("CPU");
    backend->compile(df);
    size_t ccg = count_ops_of_type<op::SigmoidBackprop>(df);
    ASSERT_EQ(ccg, 1);
}

TEST(cpu_fusion, sigmoid_bprop_n1c1h4)
{
    auto input = make_shared<op::Parameter>(element::f32, Shape{1, 1, 4});
    auto delta = make_shared<op::Parameter>(element::f32, Shape{1, 1, 4});
    auto sigmoid_node = make_shared<op::SigmoidBackprop>(input, delta);
    auto func = make_shared<Function>(sigmoid_node, op::ParameterVector{input, delta});
    auto backend = runtime::Backend::create("CPU");

    shared_ptr<runtime::TensorView> a = backend->create_tensor(element::f32, input->get_shape());
    shared_ptr<runtime::TensorView> b = backend->create_tensor(element::f32, delta->get_shape());
    shared_ptr<runtime::TensorView> result =
        backend->create_tensor(element::f32, input->get_shape());

    vector<float> dataA{1.0f, 4.0f, 1.0f, 4.0f};
    vector<float> dataB{1.0f, 1.0f, 1.0f, 1.0f};

    copy_data(a, dataA);
    copy_data(b, dataB);
    backend->call(func, {result}, {a, b});

    vector<float> expected{0.196612f, 0.0176627f, 0.196612f, 0.0176627f};
    EXPECT_TRUE(test::all_close(expected, read_vector<float>(result)));
}

TEST(cpu_fusion, batchnorm_fprop_relu_b1c2h2w2)
{
    auto input_shape = Shape{1, 2, 2, 2};
    auto input = make_shared<op::Parameter>(element::f32, input_shape);
    auto mean_shape = Shape{2};
    auto var_shape = Shape{2};
    auto gamma_shape = Shape{2};
    auto gamma = make_shared<op::Parameter>(element::f32, gamma_shape);
    auto beta_shape = Shape{2};
    auto beta = make_shared<op::Parameter>(element::f32, beta_shape);
    double eps = 0.001;
    auto shape_r = Shape{1, 2, 2, 2};
    auto bn = make_shared<op::BatchNorm>(eps, gamma, beta, input);

    auto output_rt = std::make_shared<op::GetOutputElement>(bn, 0);
    // Note, op::Splice is used to break Relu(BatchNorm) fusion
    // otherwise we will be comparing two BatchNormRelus
    // Unfortunately, we can't use INTERPRETER for
    // verifying the results as it doesn't implement
    // BatchNorm op.
    auto slice =
        std::make_shared<op::Slice>(output_rt, Coordinate{0, 0, 0, 0}, Coordinate{1, 2, 2, 2});
    auto output_relu = std::make_shared<op::Relu>(slice);
    auto mean_rt = std::make_shared<op::GetOutputElement>(bn, 1);
    auto variance_rt = std::make_shared<op::GetOutputElement>(bn, 2);

    auto bn_relu = make_shared<op::BatchNormRelu>(eps, gamma, beta, input);
    auto output_rt_bnr = std::make_shared<op::GetOutputElement>(bn_relu, 0);
    auto mean_rt_bnr = std::make_shared<op::GetOutputElement>(bn_relu, 1);
    auto variance_rt_bnr = std::make_shared<op::GetOutputElement>(bn_relu, 2);

    auto f = make_shared<Function>(
        NodeVector{output_relu, mean_rt, variance_rt, output_rt_bnr, mean_rt_bnr, variance_rt_bnr},
        op::ParameterVector{input, gamma, beta});
    auto backend = runtime::Backend::create("CPU");

    // Create some tensors for input/output
    auto input_t = backend->create_tensor(element::f32, Shape{1, 2, 2, 2});

    copy_data(input_t,
              vector<float>{0.54881352f,
                            0.71518934f,
                            0.60276335f,
                            0.54488319f,
                            0.42365479f,
                            0.64589411f,
                            0.4375872f,
                            0.89177299f});
    auto gamma_t = backend->create_tensor(element::f32, gamma_shape);
    copy_data(gamma_t, vector<float>{1.0f, 1.0f});
    auto beta_t = backend->create_tensor(element::f32, beta_shape);
    copy_data(beta_t, vector<float>{0.0f, 0.0f});
    auto bn_output = backend->create_tensor(element::f32, shape_r);
    auto result_mean = backend->create_tensor(element::f32, mean_shape);
    auto result_variance = backend->create_tensor(element::f32, var_shape);

    auto bn_output_bnr = backend->create_tensor(element::f32, shape_r);
    auto result_mean_bnr = backend->create_tensor(element::f32, mean_shape);
    auto result_variance_bnr = backend->create_tensor(element::f32, var_shape);

    backend->call(f,
                  {bn_output,
                   result_mean,
                   result_variance,
                   bn_output_bnr,
                   result_mean_bnr,
                   result_variance_bnr},
                  {input_t, gamma_t, beta_t});

    EXPECT_TRUE(test::all_close(read_vector<float>(bn_output), read_vector<float>(bn_output_bnr)));
    EXPECT_TRUE(
        test::all_close(read_vector<float>(result_mean), read_vector<float>(result_mean_bnr)));
    EXPECT_TRUE(test::all_close(read_vector<float>(result_variance),
                                read_vector<float>(result_variance_bnr)));
}

TEST(cpu_fusion, fuse_conv_relu)
{
    auto A = std::make_shared<op::Parameter>(element::f32, Shape{2, 1, 2, 2});
    auto weights = std::make_shared<op::Parameter>(element::f32, Shape{1, 1, 2, 2});
    auto convolution = std::make_shared<op::Convolution>(A, weights, Strides{1, 1}, Strides{1, 1});
    auto relu = std::make_shared<op::Relu>(convolution);
    auto abs_node =
        std::make_shared<op::Abs>(std::make_shared<op::Abs>(std::make_shared<op::Abs>(relu)));
    auto func = make_shared<Function>(abs_node, op::ParameterVector{A, weights});

    pass::Manager pass_manager;
    pass_manager.register_pass<runtime::cpu::pass::CPUFusion>(
        runtime::cpu::pass::CPUFusion::REGULAR_FUSIONS);
    pass_manager.run_passes(func);
    size_t cb = count_ops_of_type<op::ConvolutionRelu>(func);
    ASSERT_GT(cb, 0);
}

template <typename T>
static std::vector<std::vector<T>>
    execute(std::shared_ptr<Function> f, std::vector<std::vector<T>> args, std::string cbackend)
{
    auto backend = runtime::Backend::create(cbackend);

    auto parms = f->get_parameters();

    if (parms.size() != args.size())
    {
        throw ngraph_error("number of parameters and arguments don't match");
    }

    std::vector<std::shared_ptr<ngraph::runtime::TensorView>> arg_tensors(args.size());
    for (size_t i = 0; i < args.size(); i++)
    {
        auto t = backend->create_tensor(parms.at(i)->get_element_type(), parms.at(i)->get_shape());
        copy_data(t, args.at(i));
        arg_tensors.at(i) = t;
    }

    auto results = f->get_results();
    std::vector<std::shared_ptr<ngraph::runtime::TensorView>> result_tensors(results.size());

    for (size_t i = 0; i < results.size(); i++)
    {
        result_tensors.at(i) =
            backend->create_tensor(results.at(i)->get_element_type(), results.at(i)->get_shape());
    }

    backend->call(f, result_tensors, arg_tensors);

    std::vector<std::vector<T>> result_vectors;
    for (auto rt : result_tensors)
    {
        result_vectors.push_back(read_vector<T>(rt));
    }
    return result_vectors;
}

TEST(cpu_fusion, conv_relu_n2c1h2w2_2)
{
    Shape shape_a{2, 1, 6, 6};
    Shape shape_weights{1, 1, 2, 2};

    auto make_int_function = [shape_a, shape_weights]() {
        auto A = std::make_shared<op::Parameter>(element::f32, shape_a);
        auto weights = std::make_shared<op::Parameter>(element::f32, shape_weights);
        auto conv = std::make_shared<op::Convolution>(A, weights, Strides{2, 2}, Strides{1, 1});
        auto relu = std::make_shared<op::Relu>(conv);
        auto f = make_shared<Function>(NodeVector{relu}, op::ParameterVector{A, weights});
        return f;
    };

    auto int_f = make_int_function();

    auto make_cpu_function = [shape_a, shape_weights]() {
        auto A = std::make_shared<op::Parameter>(element::f32, shape_a);
        auto weights = std::make_shared<op::Parameter>(element::f32, shape_weights);
        auto conv = std::make_shared<op::Convolution>(A, weights, Strides{2, 2}, Strides{1, 1});
        auto conv_relu = std::make_shared<op::ConvolutionRelu>(conv);
        auto f = make_shared<Function>(NodeVector{conv_relu}, op::ParameterVector{A, weights});
        return f;
    };

    auto cpu_f = make_cpu_function();

    vector<vector<float>> args{
        {1.25f,  2.25f, 5.25f, 6.25f,  -1.25f, -1.25f, 3.25f, -4.25f, 7.25f,  8.25f,  -1.25f,
         -1.25f, 1.25f, 2.25f, -3.25f, 2.25f,  4.25f,  4.25f, 1.25f,  2.25f,  -4.25f, 2.25f,
         4.25f,  4.25f, 0.f,   0.f,    -1.f,   0.f,    2.f,   2.f,    0.f,    0.f,    0.f,
         0.f,    2.f,   2.f,   1.25f,  2.25f,  5.25f,  6.25f, 1.25f,  1.25f,  3.25f,  4.25f,
         -7.25f, 8.25f, 1.25f, -1.25f, -1.25f, 2.25f,  3.25f, 2.25f,  -4.25f, -4.25f, -1.25f,
         -2.25f, 4.25f, 2.25f, 4.25f,  4.25f,  0.f,    0.f,   1.f,    0.f,    -2.f,   2.f,
         0.f,    0.f,   0.f,   0.f,    -2.f,   -2.f},
        {2., 2., 2., 2.}};

    auto int_results = execute(int_f, args, "INTERPRETER");
    auto cpu_results = execute(cpu_f, args, "CPU");
    EXPECT_TRUE(test::all_close(cpu_results.at(0), int_results.at(0)));
}

TEST(cpu_fusion, conv_bias_relu_n2c1h2w2_2)
{
    Shape shape_a{2, 1, 6, 6};
    Shape shape_weights{1, 1, 2, 2};
    Shape shape_bias{1};

    auto make_int_function = [shape_a, shape_weights, shape_bias]() {
        auto A = std::make_shared<op::Parameter>(element::f32, shape_a);
        auto weights = std::make_shared<op::Parameter>(element::f32, shape_weights);
        auto conv = std::make_shared<op::Convolution>(A, weights, Strides{2, 2}, Strides{1, 1});
        auto bias = std::make_shared<op::Parameter>(element::f32, shape_bias);
        auto conv_bias =
            conv + std::make_shared<op::Broadcast>(bias, conv->get_shape(), AxisSet{0, 2, 3});
        auto relu = std::make_shared<op::Relu>(conv_bias);
        auto f = make_shared<Function>(NodeVector{relu}, op::ParameterVector{A, weights, bias});
        return f;
    };

    auto int_f = make_int_function();

    auto make_cpu_function = [shape_a, shape_weights, shape_bias]() {
        auto A = std::make_shared<op::Parameter>(element::f32, shape_a);
        auto weights = std::make_shared<op::Parameter>(element::f32, shape_weights);
        auto bias = std::make_shared<op::Parameter>(element::f32, shape_bias);
        auto conv = std::make_shared<op::Convolution>(A, weights, Strides{2, 2}, Strides{1, 1});
        auto conv_bias_relu = std::make_shared<op::ConvolutionBiasRelu>(
            std::make_shared<op::ConvolutionBias>(conv, bias));
        auto f = make_shared<Function>(NodeVector{conv_bias_relu},
                                       op::ParameterVector{A, weights, bias});
        return f;
    };

    auto cpu_f = make_cpu_function();

    vector<vector<float>> args{
        {1.25f,  2.25f, 5.25f, 6.25f,  -1.25f, -1.25f, 3.25f, -4.25f, 7.25f,  8.25f,  -1.25f,
         -1.25f, 1.25f, 2.25f, -3.25f, 2.25f,  4.25f,  4.25f, 1.25f,  2.25f,  -4.25f, 2.25f,
         4.25f,  4.25f, 0.f,   0.f,    -1.f,   0.f,    2.f,   2.f,    0.f,    0.f,    0.f,
         0.f,    2.f,   2.f,   1.25f,  2.25f,  5.25f,  6.25f, 1.25f,  1.25f,  3.25f,  4.25f,
         -7.25f, 8.25f, 1.25f, -1.25f, -1.25f, 2.25f,  3.25f, 2.25f,  -4.25f, -4.25f, -1.25f,
         -2.25f, 4.25f, 2.25f, 4.25f,  4.25f,  0.f,    0.f,   1.f,    0.f,    -2.f,   2.f,
         0.f,    0.f,   0.f,   0.f,    -2.f,   -2.f},
        {2., 2., 2., 2.},
        {0.1f}};

    auto int_results = execute(int_f, args, "INTERPRETER");
    auto cpu_results = execute(cpu_f, args, "CPU");
    EXPECT_TRUE(test::all_close(cpu_results.at(0), int_results.at(0)));
}

std::vector<shared_ptr<runtime::TensorView>>
    rnn_matrix_fusion_eval(const size_t time_steps,
                           const Shape& data_shape,
                           const Shape& weights_shape,
                           const Shape& bias_shape,
                           const vector<float>& data_val,
                           const vector<float>& weights_val,
                           const vector<float>& bias_val,
                           const bool enable_pass)
{
    auto data = make_shared<op::Parameter>(element::f32, data_shape);
    auto weights = make_shared<op::Parameter>(element::f32, weights_shape);
    auto bias = make_shared<op::Parameter>(element::f32, bias_shape);

    // results from each time step
    NodeVector results;
    for (size_t t = 0; t < time_steps; ++t)
    {
        auto data_slice = make_shared<op::Slice>(
            data, Coordinate{0, t, 0}, Coordinate{data_shape[0], t + 1, data_shape[2]});
        auto data_reshape = make_shared<op::Reshape>(
            data_slice, AxisVector{0, 1, 2}, Shape{data_shape[0], data_shape[2]});
        auto weights_reshape = make_shared<op::Reshape>(
            weights, AxisVector{1, 0}, Shape{weights_shape[1], weights_shape[0]});
        auto dot = make_shared<op::Dot>(data_reshape, weights_reshape);
        auto bias_broadcast = make_shared<op::Broadcast>(bias, dot->get_shape(), AxisSet{0});
        auto add = make_shared<op::Add>(dot, bias_broadcast);
        results.push_back(add);
    }
    auto func = make_shared<Function>(results, op::ParameterVector{data, weights, bias});
    if (enable_pass)
    {
        pass::Manager pass_manager;
        pass_manager.register_pass<runtime::cpu::pass::CPURnnMatFusion>();
        pass_manager.register_pass<runtime::cpu::pass::CPUFusion>(
            runtime::cpu::pass::CPUFusion::REGULAR_FUSIONS);
        pass_manager.run_passes(func);
        // check all of our dot/add are converted to a single MatmulBias op.
        size_t count = count_ops_of_type<op::MatmulBias>(func);
        EXPECT_EQ(count, 1);
    }

    auto backend = runtime::Backend::create("CPU");

    shared_ptr<runtime::TensorView> data_tensor =
        backend->create_tensor(element::f32, data->get_shape());
    shared_ptr<runtime::TensorView> weights_tensor =
        backend->create_tensor(element::f32, weights->get_shape());
    shared_ptr<runtime::TensorView> bias_tensor =
        backend->create_tensor(element::f32, bias->get_shape());

    std::vector<shared_ptr<runtime::TensorView>> result_tensors;
    for (auto r : results)
    {
        result_tensors.push_back(backend->create_tensor(element::f32, r->get_shape()));
    }

    copy_data(data_tensor, data_val);
    copy_data(weights_tensor, weights_val);
    copy_data(bias_tensor, bias_val);
    backend->call(func, result_tensors, {data_tensor, weights_tensor, bias_tensor});
    return result_tensors;
}

TEST(cpu_fusion, rnn_matrix_fusion_eval_pass)
{
    const size_t time_steps = 4;
    Shape data_shape{3, time_steps, 5};
    Shape weights_shape{6, data_shape[2]};
    Shape bias_shape{6};

    test::Uniform<float> rng{0, 1, 0};
    vector<float> data_val(shape_size(data_shape));
    vector<float> weights_val(shape_size(weights_shape));
    vector<float> bias_val(shape_size(bias_shape));
    rng.initialize(data_val);
    rng.initialize(weights_val);
    rng.initialize(bias_val);

    std::vector<shared_ptr<runtime::TensorView>> result_expected = rnn_matrix_fusion_eval(
        time_steps, data_shape, weights_shape, bias_shape, data_val, weights_val, bias_val, false);
    std::vector<shared_ptr<runtime::TensorView>> result_fused = rnn_matrix_fusion_eval(
        time_steps, data_shape, weights_shape, bias_shape, data_val, weights_val, bias_val, true);
    for (size_t i = 0; i < result_expected.size(); ++i)
    {
        EXPECT_TRUE(test::all_close<float>(result_expected[i], result_fused[i]));
    }
}

TEST(cpu_fusion, rnn_fusion_from_json_model)
{
    pass::Manager pass_manager;
    pass_manager.register_pass<runtime::cpu::pass::CPURnnMatFusion>();
    pass_manager.register_pass<runtime::cpu::pass::CPUFusion>(
        runtime::cpu::pass::CPUFusion::REGULAR_FUSIONS);
    const string json_path =
        file_util::path_join(SERIALIZED_ZOO, "mxnet/rnn-10-step-fusion-test.json");
    const string json_string = file_util::read_file_to_string(json_path);
    stringstream ss(json_string);
    shared_ptr<Function> func = ngraph::deserialize(ss);
    pass_manager.run_passes(func);
    const size_t NUM_STEPS = 10;
    auto mmb_predicate = [](std::shared_ptr<Node> node) {
        auto users = node->get_users();
        return users.size() == NUM_STEPS &&
               std::all_of(begin(users), end(users), [](std::shared_ptr<Node> n) {
                   return std::dynamic_pointer_cast<op::Slice>(n) != nullptr;
               });
    };

    auto mmbs = get_ops_of_type<op::MatmulBias>(func);
    ASSERT_TRUE(std::any_of(begin(mmbs), end(mmbs), mmb_predicate));
}

TEST(cpu_fusion, weight_fusion)
{
    auto param = std::make_shared<op::Parameter>(element::f32, Shape{64});
    auto reshape_conv =
        std::make_shared<ngraph::op::Reshape>(param, AxisVector{0}, Shape{16, 4, 1, 1});
    auto data_conv = std::make_shared<op::Parameter>(element::f32, Shape{16, 4, 7, 7});
    auto tvt = reshape_conv->get_outputs().at(0).get_tensor_view().get();
    auto lt_desc = std::make_shared<runtime::cpu::LayoutDescriptor>(*tvt, AxisVector{0, 1, 2, 3});
    auto cvt_lt_conv = std::make_shared<runtime::cpu::op::ConvertLayout>(reshape_conv, lt_desc);
    auto conv = std::make_shared<ngraph::op::Convolution>(
        data_conv, cvt_lt_conv, Strides{1, 1}, Strides{1, 1});

    auto reshape_conv_bprop =
        std::make_shared<op::Reshape>(param, AxisVector{0}, Shape{16, 4, 1, 1});
    auto dummy_arg_conv_bprop = std::make_shared<op::Parameter>(element::f32, Shape{1, 16, 7, 7});
    auto tvt_bprop = reshape_conv_bprop->get_outputs().at(0).get_tensor_view().get();
    auto lt_desc_bprop =
        std::make_shared<runtime::cpu::LayoutDescriptor>(*tvt_bprop, AxisVector{0, 1, 2, 3});
    auto cvt_lt_conv_bprop =
        std::make_shared<runtime::cpu::op::ConvertLayout>(reshape_conv_bprop, lt_desc_bprop);
    auto conv_bprop = std::make_shared<op::ConvolutionBackpropData>(Shape{1, 4, 7, 7},
                                                                    cvt_lt_conv_bprop,
                                                                    dummy_arg_conv_bprop,
                                                                    Strides{1, 1},
                                                                    Strides{1, 1},
                                                                    CoordinateDiff{0, 0},
                                                                    CoordinateDiff{0, 0},
                                                                    Strides{1, 1});

    auto conv_relu = std::make_shared<op::Relu>(conv);
    auto conv_bprop_abs = std::make_shared<op::Abs>(conv_bprop);

    auto f = make_shared<Function>(NodeVector{conv_relu, conv_bprop_abs},
                                   op::ParameterVector{param, data_conv, dummy_arg_conv_bprop});

    pass::Manager pass_manager;
    pass_manager.register_pass<runtime::cpu::pass::CPUPostLayoutOptimizations>();
    pass_manager.run_passes(f);

    auto new_conv_bprop_data = conv_bprop_abs->get_argument(0);
    auto new_convert_layout = new_conv_bprop_data->get_argument(0);

    ASSERT_EQ(std::dynamic_pointer_cast<runtime::cpu::op::ConvertLayout>(
                  new_convert_layout->get_argument(0)),
              cvt_lt_conv);
}

TEST(cpu_fusion, max_pool_with_indices)
{
    Shape shape_a{10, 3, 28, 28};
    auto input = std::make_shared<op::Parameter>(element::f32, shape_a);
    Shape window_shape{2, 2};
    auto max_pool = std::make_shared<op::MaxPool>(input, window_shape);
    auto C = std::make_shared<op::Parameter>(element::f32, max_pool->get_shape());

    ngraph::autodiff::Adjoints adjoints(NodeVector{max_pool}, NodeVector{C});

    auto dinput = adjoints.backprop_node(input);

    auto df = std::make_shared<Function>(NodeVector{dinput}, op::ParameterVector{input, C});

    auto f = std::make_shared<Function>(NodeVector{max_pool}, op::ParameterVector{input});

    {
        pass::Manager pass_manager;
        pass_manager.register_pass<pass::VisualizeTree>("max_pool_fprop_before.pdf");
        pass_manager.run_passes(f);
    }

    {
        pass::Manager pass_manager;
        pass_manager.register_pass<pass::VisualizeTree>("max_pool_bprop_before.pdf");
        pass_manager.register_pass<runtime::cpu::pass::CPUWorkspaceInsertion>();
        pass_manager.register_pass<pass::VisualizeTree>("max_pool_bprop_after.pdf");
        pass_manager.run_passes(df);
    }

    {
        pass::Manager pass_manager;
        pass_manager.register_pass<pass::VisualizeTree>("max_pool_fprop_after.pdf");
        pass_manager.run_passes(f);
    }

    auto maxpool_goe_output =
        std::dynamic_pointer_cast<op::GetOutputElement>(f->get_results().at(0)->get_argument(0));
    ASSERT_TRUE(maxpool_goe_output);
    ASSERT_EQ(maxpool_goe_output->get_n(), 0);
    auto maxpool_with_indices = df->get_results().at(0)->get_argument(0);
    auto maxpool_goe_indices =
        std::dynamic_pointer_cast<op::GetOutputElement>(maxpool_with_indices->get_argument(2));
    ASSERT_TRUE(maxpool_goe_indices);
    ASSERT_EQ(maxpool_goe_indices->get_n(), 1);
}

TEST(cpu_fusion, backwards_maxpool_with_indices_n4_c1_hw4_2x2_max)
{
    Shape shape_a{1, 4, 4, 4};
    Shape maxpool_shape{1, 4, 3, 3};
    auto A = std::make_shared<op::Parameter>(element::f32, shape_a);
    Shape window_shape{2, 2};
    auto window_movement_strides = Strides{1, 1};
    auto maxpool = std::make_shared<op::MaxPool>(A, window_shape, window_movement_strides);
    auto f = std::make_shared<Function>(maxpool, op::ParameterVector{A});

    auto backend = runtime::Backend::create("CPU");
    shared_ptr<runtime::TensorView> ep = backend->create_tensor(element::f32, maxpool_shape);
    vector<float> dataEp(shape_size(maxpool_shape), 4);

    shared_ptr<runtime::TensorView> input = backend->create_tensor(element::f32, shape_a);
    shared_ptr<runtime::TensorView> output = backend->create_tensor(element::f32, shape_a);

    vector<float> dataInput{11.f, 31.f, 40.f, 47.f, 13.f, 61.f, 48.f, 59.f, 17.f, 39.f, 64.f,
                            62.f, 45.f, 55.f, 36.f, 19.f, 65.f, 33.f, 49.f, 30.f, 56.f, 41.f,
                            53.f, 58.f, 22.f, 35.f, 52.f, 50.f, 63.f, 54.f, 12.f, 26.f, 44.f,
                            21.f, 69.f, 24.f, 46.f, 25.f, 51.f, 29.f, 72.f, 15.f, 73.f, 10.f,
                            16.f, 37.f, 70.f, 32.f, 28.f, 66.f, 57.f, 27.f, 60.f, 42.f, 43.f,
                            71.f, 18.f, 38.f, 67.f, 68.f, 14.f, 20.f, 34.f, 23.f};

    vector<float> expected{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 12.0f, 0.0f, 4.0f, 0.0f, 0.0f,  16.0f,
                           0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 4.0f,  0.0f, 0.0f, 0.0f, 4.0f,  0.0f,
                           8.0f, 8.0f, 0.0f, 0.0f, 4.0f, 0.0f,  4.0f, 4.0f, 0.0f, 0.0f,  0.0f,
                           0.0f, 8.0f, 0.0f, 4.0f, 0.0f, 0.0f,  0.0f, 8.0f, 0.0f, 16.0f, 0.0f,
                           0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 8.0f,  0.0f, 0.0f, 4.0f, 0.0f,  0.0f,
                           8.0f, 0.0f, 4.0f, 8.0f, 4.0f, 0.0f,  0.0f, 0.0f, 0.0f};

    copy_data(ep, dataEp);
    copy_data(input, dataInput);

    auto C = std::make_shared<op::Parameter>(element::f32, maxpool_shape);
    auto df = autodiff::backprop_function(f);

    {
        pass::Manager pass_manager;
        pass_manager.register_pass<pass::VisualizeTree>("max_pool_bprop_before2.pdf");
        pass_manager.register_pass<runtime::cpu::pass::CPUWorkspaceInsertion>();
        pass_manager.register_pass<pass::VisualizeTree>("max_pool_bprop_after2.pdf");
        pass_manager.run_passes(df);
    }

    backend->call(df, {output}, {input, ep});
    ASSERT_TRUE(read_vector<float>(output) == expected);
}

TEST(cpu_fusion, batch_norm_folding)
{
    Shape shape_input{1, 8, 3, 3};
    Shape shape_weights{2, 8, 1, 1};
    Shape shape_norm{2};

    auto make_function = [shape_input, shape_weights, shape_norm]() {
        auto input = std::make_shared<op::Parameter>(element::f32, shape_input);
        auto weights = std::make_shared<op::Parameter>(element::f32, shape_weights);
        double eps = 0.001;
        auto gamma = std::make_shared<op::Parameter>(element::f32, shape_norm);
        auto beta = std::make_shared<op::Parameter>(element::f32, shape_norm);
        auto mean = std::make_shared<op::Parameter>(element::f32, shape_norm);
        auto var = std::make_shared<op::Parameter>(element::f32, shape_norm);
        auto conv = std::make_shared<op::Convolution>(input, weights, Strides{1, 1}, Strides{1, 1});
        auto bn = std::make_shared<op::BatchNorm>(eps, gamma, beta, conv, mean, var);
        auto f = make_shared<Function>(NodeVector{bn},
                                       op::ParameterVector{input, weights, gamma, beta, mean, var});
        return f;
    };

    auto int_f = make_function();
    auto cpu_f = make_function();

    vector<vector<float>> args{
        {1.25f,  2.25f, 5.25f, 6.25f,  -1.25f, -1.25f, 3.25f, -4.25f, 7.25f,  8.25f,  -1.25f,
         -1.25f, 1.25f, 2.25f, -3.25f, 2.25f,  4.25f,  4.25f, 1.25f,  2.25f,  -4.25f, 2.25f,
         4.25f,  4.25f, 0.f,   0.f,    -1.f,   0.f,    2.f,   2.f,    0.f,    0.f,    0.f,
         0.f,    2.f,   2.f,   1.25f,  2.25f,  5.25f,  6.25f, 1.25f,  1.25f,  3.25f,  4.25f,
         -7.25f, 8.25f, 1.25f, -1.25f, -1.25f, 2.25f,  3.25f, 2.25f,  -4.25f, -4.25f, -1.25f,
         -2.25f, 4.25f, 2.25f, 4.25f,  4.25f,  0.f,    0.f,   1.f,    0.f,    -2.f,   2.f,
         0.f,    0.f,   0.f,   0.f,    -2.f,   -2.f},
        {1.25f,
         2.25f,
         5.25f,
         6.25f,
         -1.25f,
         -1.25f,
         3.25f,
         -4.25f,
         7.25f,
         8.25f,
         -1.25f,
         0.f,
         0.f,
         0.f,
         0.f,
         -2.f},
        {-0.9384f, 0.01875f},
        {11.0f, 1.3f},
        {0.12f, 0.31f},
        {0.01f, 0.11f},
    };

    auto int_results = execute(int_f, args, "INTERPRETER");
    auto cpu_results = execute(cpu_f, args, "CPU");
    EXPECT_TRUE(test::all_close(cpu_results.at(0), int_results.at(0)));
}

TEST(cpu_fusion, rnn_fprop_1_lstm_cell)
{
    auto src_layer = make_shared<op::Parameter>(element::f32, Shape{10, 100});
    auto src_iter = make_shared<op::Parameter>(element::f32, Shape{20, 100});
    auto weights_layer = make_shared<op::Parameter>(element::f32, Shape{400, 100});
    auto weights_iter = make_shared<op::Parameter>(element::f32, Shape{400, 100});
    auto biases = make_shared<op::Parameter>(element::f32, Shape{400});
    const int number_of_timesteps = 1;
    const int number_of_gates_per_cell = 4;
    const int src_seq_length = 1;
    const int src_layer_feature_size = 100;
    const int feature_size = 100;
    const int num_rnn_cell_states = 2;
    const int rnn_direction = 1;
    const int num_of_rnn_fused_layer = 1;
    auto rnn_node = make_shared<op::Rnn>(src_layer,
                                         src_iter,
                                         weights_layer,
                                         weights_iter,
                                         biases,
                                         number_of_timesteps,
                                         number_of_gates_per_cell,
                                         src_seq_length,
                                         src_layer_feature_size,
                                         feature_size,
                                         num_rnn_cell_states,
                                         rnn_direction,
                                         num_of_rnn_fused_layer);
    auto rnn_ht_output = make_shared<op::GetOutputElement>(rnn_node, 0);
    auto rnn_ct_output = make_shared<op::GetOutputElement>(rnn_node, 1);

    auto func = make_shared<Function>(
        NodeVector{rnn_ht_output, rnn_ct_output},
        op::ParameterVector{src_layer, src_iter, weights_layer, weights_iter, biases});
    auto backend = runtime::Backend::create("CPU");

    shared_ptr<runtime::TensorView> src_layer_t =
        backend->create_tensor(element::f32, src_layer->get_shape());
    shared_ptr<runtime::TensorView> src_iter_t =
        backend->create_tensor(element::f32, src_iter->get_shape());
    shared_ptr<runtime::TensorView> weights_layer_t =
        backend->create_tensor(element::f32, weights_layer->get_shape());
    shared_ptr<runtime::TensorView> weights_iter_t =
        backend->create_tensor(element::f32, weights_iter->get_shape());
    shared_ptr<runtime::TensorView> biases_t =
        backend->create_tensor(element::f32, biases->get_shape());
    shared_ptr<runtime::TensorView> result_ht = backend->create_tensor(element::f32, {10, 100});
    shared_ptr<runtime::TensorView> result_ct =
        backend->create_tensor(element::f32, Shape{20, 100});

    copy_data(src_layer_t, vector<float>(1000, 1));
    copy_data(src_iter_t, vector<float>(2000, 1));
    copy_data(weights_layer_t, vector<float>(400 * 100, 1));
    copy_data(weights_iter_t, vector<float>(400 * 100, 1));
    copy_data(biases_t, vector<float>(400, 1));

    backend->call(func,
                  {result_ht, result_ct},
                  {src_layer_t, src_iter_t, weights_layer_t, weights_iter_t, biases_t});
    vector<float> expected_ht(10 * 100, 0.964028f);
    vector<float> expected_ct;
    for (size_t i = 0; i < 20 * 100; i++)
    {
        if (i < 1000)
        {
            expected_ct.push_back(0.964028f);
        }
        else
        {
            expected_ct.push_back(2.0f);
        }
    }

    EXPECT_TRUE(test::all_close(expected_ht, read_vector<float>(result_ht)));
    EXPECT_TRUE(test::all_close(expected_ct, read_vector<float>(result_ct)));
}

TEST(cpu_fusion, fuse_lstm_cells)
{
    pass::Manager pass_manager;
    pass_manager.register_pass<runtime::cpu::pass::LSTMFusion>();
    pass_manager.register_pass<runtime::cpu::pass::ConcatInputs>();
    const string json_path =
        file_util::path_join(SERIALIZED_ZOO, "mxnet/2rnn_layer_3lstm_cell.json");
    const string json_string = file_util::read_file_to_string(json_path);
    stringstream ss(json_string);
    shared_ptr<Function> func = ngraph::deserialize(ss);
    pass_manager.run_passes(func);
    auto lstm_ops = get_ops_of_type<op::Lstm>(func);
    EXPECT_EQ(lstm_ops.size(), 6);
}

TEST(cpu_fusion, fuse_2_layer_rnn)
{
    pass::Manager pass_manager;
    pass_manager.register_pass<runtime::cpu::pass::LSTMFusion>();
    pass_manager.register_pass<runtime::cpu::pass::RNNFusion>();
    const string json_path =
        file_util::path_join(SERIALIZED_ZOO, "mxnet/2rnn_layer_3lstm_cell.json");
    const string json_string = file_util::read_file_to_string(json_path);
    stringstream ss(json_string);
    shared_ptr<Function> func = ngraph::deserialize(ss);
    pass_manager.run_passes(func);
    size_t count = count_ops_of_type<op::Rnn>(func);
    auto rnn_ops = get_ops_of_type<op::Rnn>(func);
    EXPECT_EQ(rnn_ops.size(), count);
    for (auto& node : rnn_ops)
    {
        EXPECT_EQ(node->get_num_timesteps(), node->get_src_sequence_length());
        EXPECT_EQ(node->get_num_cell_states(), node->get_argument(1)->get_arguments().size());
    }
}

TEST(cpu_fusion, fuse_1_layer_rnn)
{
    pass::Manager pass_manager;
    pass_manager.register_pass<runtime::cpu::pass::LSTMFusion>();
    pass_manager.register_pass<runtime::cpu::pass::RNNFusion>();
    const string json_path =
        file_util::path_join(SERIALIZED_ZOO, "mxnet/1rnn_layer_3lstm_cell.json");
    const string json_string = file_util::read_file_to_string(json_path);
    stringstream ss(json_string);
    shared_ptr<Function> func = ngraph::deserialize(ss);
    pass_manager.run_passes(func);
    size_t count = count_ops_of_type<op::Rnn>(func);
    auto rnn_ops = get_ops_of_type<op::Rnn>(func);
    EXPECT_EQ(rnn_ops.size(), 1);
    EXPECT_EQ(rnn_ops.size(), count);
    for (auto& node : rnn_ops)
    {
        EXPECT_EQ(node->get_num_timesteps(), node->get_src_sequence_length());
        EXPECT_EQ(node->get_num_cell_states(), node->get_argument(1)->get_arguments().size());
    }
}

static std::shared_ptr<Function> make_function(const std::string& file_name)
{
    const string json_path = file_util::path_join(SERIALIZED_ZOO, file_name);
    const string json_string = file_util::read_file_to_string(json_path);
    stringstream ss(json_string);
    shared_ptr<Function> func = ngraph::deserialize(ss);
    return func;
}

TEST(cpu_fusion, rnn_fusion_inter_vs_cpu_1lstm_cell)
{
    const std::string file_name("mxnet/1_lstm_cell_forward.json");
    auto cpu_f = make_function(file_name);
    auto int_f = make_function(file_name);
    test::Uniform<float> rng(0.0f, 1.0f);
    vector<vector<float>> args;

    for (shared_ptr<op::Parameter> param : int_f->get_parameters())
    {
        vector<float> tensor_val(shape_size(param->get_shape()));
        rng.initialize(tensor_val);
        args.push_back(tensor_val);
    }
    auto int_results = execute(int_f, args, "INTERPRETER");
    auto cpu_results = execute(cpu_f, args, "CPU");
    for (size_t i = 0; i < cpu_results.size(); i++)
    {
        EXPECT_TRUE(test::all_close(cpu_results.at(i), int_results.at(i), 1.0e-4f, 1.0e-4f));
    }
}

TEST(cpu_fusion, rnn_fusion_inter_vs_cpu_1rnn_layer_3lstm_cell)
{
    const std::string file_name("mxnet/1rnn_layer_3lstm_cell.json");
    auto cpu_f = make_function(file_name);
    auto int_f = make_function(file_name);
    test::Uniform<float> rng(0.0f, 1.0f);
    vector<vector<float>> args;

    for (shared_ptr<op::Parameter> param : int_f->get_parameters())
    {
        vector<float> tensor_val(shape_size(param->get_shape()));
        rng.initialize(tensor_val);
        args.push_back(tensor_val);
    }
    auto int_results = execute(int_f, args, "INTERPRETER");
    auto cpu_results = execute(cpu_f, args, "CPU");
    for (size_t i = 0; i < cpu_results.size(); i++)
    {
        EXPECT_TRUE(test::all_close(cpu_results.at(i), int_results.at(i), 1.0e-4f, 1.0e-4f));
    }
}

TEST(cpu_fusion, rnn_fusion_inter_vs_cpu_2rnn_layer_3lstm_cell)
{
    const std::string file_name("mxnet/2rnn_layer_3lstm_cell.json");
    auto cpu_f = make_function(file_name);
    auto int_f = make_function(file_name);
    test::Uniform<float> rng(0.0f, 1.0f);
    vector<vector<float>> args;

    for (shared_ptr<op::Parameter> param : int_f->get_parameters())
    {
        vector<float> tensor_val(shape_size(param->get_shape()));
        rng.initialize(tensor_val);
        args.push_back(tensor_val);
    }
    auto int_results = execute(int_f, args, "INTERPRETER");
    auto cpu_results = execute(cpu_f, args, "CPU");
    for (size_t i = 0; i < cpu_results.size(); i++)
    {
        EXPECT_TRUE(test::all_close(cpu_results.at(i), int_results.at(i), 1.0e-4f, 1.0e-4f));
    }
}

std::shared_ptr<Node> get_leader (std::shared_ptr<Node> oldn, const std::unordered_map<std::shared_ptr<Node>, std::shared_ptr<Node>>& groups)
{
    auto it = oldn;
    while (it != groups.at(it))
    {
        it = groups.at(it);
    }
    return it;
}

void log_group2(std::shared_ptr<Node> leader, const std::unordered_map<std::shared_ptr<Node>, NodeVector>& groups)
{
    NGRAPH_DEBUG << "Group leader : " << leader->get_name() << std::endl;

    std::vector<std::string> sgroup;
    for (auto m : groups.at(leader))
    {
        sgroup.push_back(m->get_name());   
    }
    NGRAPH_DEBUG << "Group members : " << vector_to_string(sgroup) << std::endl;
}

void log_group(std::shared_ptr<Node> leader, const std::unordered_map<std::shared_ptr<Node>, std::shared_ptr<Node>>& groups)
{
    NGRAPH_DEBUG << "Group leader : " << leader->get_name() << std::endl;

    std::vector<std::string> sgroup;
    for (auto e : groups)
    {
        if (e.second == leader)
        {
            sgroup.push_back(e.first->get_name());
        }
        
    }
    NGRAPH_DEBUG << "Group members : " << vector_to_string(sgroup) << std::endl;
}

void update_leader(std::shared_ptr<Node> oldn, std::shared_ptr<Node> newn, std::unordered_map<std::shared_ptr<Node>, std::shared_ptr<Node>>& groups)
{


	for (auto it = groups.begin(); it != groups.end(); ++it)
	{
        if (it->second == oldn)
        {
            it->second = newn;
        }
	}
}



void update_leader2(std::shared_ptr<Node> oldn, std::shared_ptr<Node> newn, std::unordered_map<std::shared_ptr<Node>, std::shared_ptr<Node>>& groups)
{


	for (auto it = groups.begin(); it != groups.end(); ++it)
	{
        if (it->second == oldn)
        {
            it->second = newn;
        }
	}
}

static bool is_fusable(std::shared_ptr<Node> n)
{
	return (std::dynamic_pointer_cast<op::util::BinaryElementwiseArithmetic> (n) ||
		std::dynamic_pointer_cast<op::util::UnaryElementwiseArithmetic>(n));
}

/*
TEST(cpu_fusion, graph_partition)
{
	
    Shape shape{};
	auto a = make_shared<op::Parameter>(element::i32, shape);
    auto b = make_shared<op::Parameter>(element::i32, shape);
    auto c = make_shared<op::Parameter>(element::i32, shape);
	auto add_ab = a + b;
	auto add_abs = std::make_shared<op::Abs>(add_ab);
    auto abs_neg = std::make_shared<op::Negative>(add_abs);
    auto sub_c_neg = c - abs_neg;

	auto f = std::make_shared<Function>(ngraph::NodeVector{ sub_c_neg }, op::ParameterVector{ a, b, c });
	

    std::unordered_map<std::shared_ptr<Node>, NodeVector> groups;
	for (auto n : f->get_ordered_ops())
	{
		if (is_fusable(n))
		{
			std::shared_ptr<Node> smallest_group_leader;
			NodeVector group_leaders;
			for (auto arg : n->get_arguments())
			{
				//an argument is fusable and a part of some group
                NGRAPH_DEBUG << "Considering " << arg->get_name();
				if (groups.count(arg) != 0)
				{
					if (!smallest_group_leader)
					{
						smallest_group_leader = arg;
					}
					else
					{
						//out of two groups pick one; ties are broken with the "<" operator
						smallest_group_leader = arg < smallest_group_leader ? arg : smallest_group_leader;
					}
					group_leaders.push_back(arg);
				}
			}

			//create a new group
			if (!smallest_group_leader)
			{
				groups.insert(std::make_pair(n, NodeVector{ n }));
                log_group(n, groups.at(n));
			}
			else
			{
				//note, groups being merged are completely disjoint
				//we can join these groups only because n is connected to both
				//which means both groups have the exact same shapes and thus
				//can be run in the same loop
				//also, note, that we maintain the topological order:
				//we add "n" the latest.
				//it doesn't matter in which order we add other argument groups
				//since they are disjoint and also topologically sorted internally
				auto& group_of_smallest_group_leader = groups.at(smallest_group_leader);
				for (auto gl : group_leaders)
				{
					if (gl != smallest_group_leader)
					{
						group_of_smallest_group_leader.insert(group_of_smallest_group_leader.end(),
							groups.at(gl).begin(), groups.at(gl).end());
					}
				}
				group_of_smallest_group_leader.push_back(n);
                log_group(smallest_group_leader, group_of_smallest_group_leader);
			}
		}
	}

	static const size_t MIN_NODES_TO_FUSE = 3;
	for (auto it = groups.begin(); it != groups.end();)
	{
		if (it->second.size() < MIN_NODES_TO_FUSE)
		{
			it = groups.erase(it);
		}
		else
		{
			it++;
		}	
	}
}
*/

/*
TEST(cpu_fusion, graph_partition2)
{
	
    Shape shape{};
	auto a = make_shared<op::Parameter>(element::i32, shape);
    auto b = make_shared<op::Parameter>(element::i32, shape);
    auto c = make_shared<op::Parameter>(element::i32, shape);
	auto add_ab = a + b;
	auto add_abs = std::make_shared<op::Abs>(add_ab);
    auto abs_neg = std::make_shared<op::Negative>(add_abs);
    auto sub_c_neg = c - abs_neg;


    auto d = make_shared<op::Parameter>(element::i32, shape);
    auto d_abs = std::make_shared<op::Abs>(d);
    auto goe_ab = make_shared<op::GetOutputElement>(add_ab, 0);
    auto add_d = d_abs + goe_ab;
    auto neg_d = std::make_shared<op::Negative>(add_d);

    auto mul_cd = neg_d * sub_c_neg;
	auto f = std::make_shared<Function>(ngraph::NodeVector{ mul_cd }, op::ParameterVector{ a, b, c, d });
	

    std::unordered_map<std::shared_ptr<Node>, std::shared_ptr<Node>> groups;
	for (auto n : f->get_ordered_ops())
	{
		if (is_fusable(n))
		{
			std::shared_ptr<Node> smallest_group_leader;
			NodeVector group_leaders;
			for (auto arg : n->get_arguments())
			{
				//an argument is fusable and a part of some group
                NGRAPH_DEBUG << "Considering " << arg->get_name();
				if (groups.count(arg) != 0)
				{
					if (!smallest_group_leader)
					{
						smallest_group_leader = groups.at(arg);
					}
					else
					{
						//out of two groups pick one; ties are broken with the "<" operator
						smallest_group_leader = groups.at(arg) < smallest_group_leader ? groups.at(arg) : smallest_group_leader;
					}
					group_leaders.push_back(groups.at(arg));
				}
			}

			//create a new group
			if (!smallest_group_leader)
			{
				groups.insert(std::make_pair(n, n));
                log_group(n, groups);
			}
			else
			{
				//note, groups being merged are completely disjoint
				//we can join these groups only because n is connected to both
				//which means both groups have the exact same shapes and thus
				//can be run in the same loop
				//also, note, that we maintain the topological order:
				//we add "n" the latest.
				//it doesn't matter in which order we add other argument groups
				//since they are disjoint and also topologically sorted internally
				for (auto gl : group_leaders)
				{
					if (gl != smallest_group_leader)
					{
                        update_leader(gl, smallest_group_leader, groups);
					}
				}
                groups.insert(std::make_pair(n, smallest_group_leader));
                log_group(smallest_group_leader, groups);
			}
		}
	}

    //unordered_set<std::shared_ptr<Node>> seen;
    std::unordered_map<std::shared_ptr<Node>, NodeVector> graphs;

    for (auto e : groups)
    {
        if (e.first == e.second)
        {
            std::cout << "Adding group " << e.first->get_name() << std::endl;
            graphs.insert(std::make_pair(e.first, NodeVector{e.first}));
        }
    }

    for (auto n : f->get_ordered_ops())
    {
        if (groups.count(n) != 0)
        {
            
            auto head = get_leader(n, groups);
            std::cout << "Looking at " << n->get_name() << " , leader is " << head->get_name() << std::endl;
            if (head != n)
            {
                graphs.at(head).push_back(n);
            }
        }
    }

    std::cout << "After merge : \n";
	static const size_t MIN_NODES_TO_FUSE = 3;
	for (auto it = graphs.begin(); it != graphs.end();)
	{
        log_group2(it->first, graphs);
		if (it->second.size() < MIN_NODES_TO_FUSE)
		{
			it = graphs.erase(it);
		}
		else
		{
			it++;
		}	
	}
}
*/



TEST(cpu_fusion, graph_partition3)
{
	
    Shape shape{};
	auto a = make_shared<op::Parameter>(element::i32, shape);
    auto b = make_shared<op::Parameter>(element::i32, shape);
    auto c = make_shared<op::Parameter>(element::i32, shape);
	auto add_ab = a + b;
	auto add_abs = std::make_shared<op::Abs>(add_ab);
    auto abs_neg = std::make_shared<op::Negative>(add_abs);
    auto sub_c_neg = c - abs_neg;


    auto d = make_shared<op::Parameter>(element::i32, shape);
    auto d_abs = std::make_shared<op::Abs>(d);
    auto goe_ab = make_shared<op::GetOutputElement>(add_ab, 0);
    auto add_d = d_abs + goe_ab;
    auto neg_d = std::make_shared<op::Negative>(add_d);

    auto mul_cd = neg_d * sub_c_neg;
	auto f = std::make_shared<Function>(ngraph::NodeVector{ mul_cd }, op::ParameterVector{ a, b, c, d });
	

    std::unordered_map<std::shared_ptr<Node>, NodeVector> graphs;
    std::unordered_map<std::shared_ptr<Node>, std::shared_ptr<Node>> heads;
	for (auto n : f->get_ordered_ops())
	{
		if (is_fusable(n))
		{
			std::shared_ptr<Node> arg_with_smallest_head;
			NodeVector args_with_heads;
			for (auto arg : n->get_arguments())
			{
				//an argument is fusable and a part of some group
                NGRAPH_DEBUG << "Considering " << arg->get_name();
				if (heads.count(arg) != 0)
				{
					if (!arg_with_smallest_head)
					{
						arg_with_smallest_head = arg;
					}
					else
					{
						//out of two graphs pick one; ties are broken with the "<" operator
						arg_with_smallest_head = heads.at(arg) < heads.at(arg_with_smallest_head) ? arg : arg_with_smallest_head;
					}
					args_with_heads.push_back(arg);
				}
			}

			//create a new group
			if (!arg_with_smallest_head)
			{
                heads.insert(std::make_pair(n, n));
				graphs.insert(std::make_pair(n, NodeVector{ n }));
                log_group2(n, graphs);
			}
			else
			{
				//note, graphs being merged are completely disjoint
				//we can join these graphs only because n is connected to both
				//which means both graphs have the exact same shapes and thus
				//can be run in the same loop
				//also, note, that we maintain the topological order:
				//we add "n" the latest.
				//it doesn't matter in which order we add other argument graphs
				//since they are disjoint and also topologically sorted internally
                auto smallest_head = heads.at(arg_with_smallest_head);
				auto& graph_with_smallest_head = graphs.at(smallest_head);

				for (auto awh : args_with_heads)
				{
					if (awh != arg_with_smallest_head)
					{
                        //merge groups
						graph_with_smallest_head.insert(graph_with_smallest_head.end(),
							graphs.at(heads.at(awh)).begin(), graphs.at(heads.at(awh)).end());
                        //set smallest head for a graph we are merging into smallest_head
                        for (auto g : graphs.at(heads.at(awh)))
                        {
                            heads.at(g) = smallest_head;
                        }
					}
				}
				graph_with_smallest_head.push_back(n);
                heads.insert(std::make_pair(n, smallest_head));
                log_group2(smallest_head, graphs);
			}
		}
	}

	static const size_t MIN_NODES_TO_FUSE = 3;
	for (auto it = graphs.begin(); it != graphs.end();)
	{
		if (it->second.size() < MIN_NODES_TO_FUSE)
		{
			it = graphs.erase(it);
		}
		else
		{
			it++;
		}	
	}
}

class LoopKernelCollector
{
public:
	LoopKernelCollector(std::shared_ptr<Function> f, size_t MIN_NODES_TO_FUSE)
	{
		for (auto n : f->get_ordered_ops())
		{
			if (is_fusable(n))
			{
				collect_fusable_args(n);

				//create a new group
				if (!m_arg_with_smallest_head)
				{
					m_heads.insert(std::make_pair(n, n));
					m_graphs.insert(std::make_pair(n, NodeVector{ n }));
					log_group(n);
				}
				else
				{
					//note, graphs being merged are completely disjoint
					//we can join these graphs only because n is connected to both
					//which means both graphs have the exact same shapes and thus
					//can be run in the same loop
					//also, note, that we maintain the topological order:
					//we add "n" the latest.
					//it doesn't matter in which order we add other argument graphs
					//since they are disjoint and also topologically sorted internally
					auto smallest_head = m_heads.at(m_arg_with_smallest_head);

					for (auto awh : m_args_with_heads)
					{
						if (awh != m_arg_with_smallest_head)
						{
							merge(m_heads.at(awh), smallest_head);
						}
					}

					//add n to the group with the smallest head
					m_graphs.at(smallest_head).push_back(n);
					m_heads.insert(std::make_pair(n, smallest_head));
					log_group(smallest_head);

                    //clean current iteration state
                    m_arg_with_smallest_head = nullptr;
                    m_args_with_heads.clear();
				}
			}
		}

		prune_graphs(MIN_NODES_TO_FUSE);
	}

    const std::unordered_map<std::shared_ptr<Node>, NodeVector>& get_graphs() const { return m_graphs; }

private:
	void prune_graphs(size_t MIN_NODES_TO_FUSE)
	{
		for (auto it = m_graphs.begin(); it != m_graphs.end();)
		{
			if (it->second.size() < MIN_NODES_TO_FUSE)
			{
				it = m_graphs.erase(it);
			}
			else
			{
				it++;
			}
		}
	}

	void merge(std::shared_ptr<Node> src, std::shared_ptr<Node> dst)
	{
		auto& dst_graph = m_graphs.at(dst);
		auto& src_graph = m_graphs.at(src);

		//merge groups
		dst_graph.insert(dst_graph.end(),
			src_graph.begin(), src_graph.end());
		//set smallest head for a graph we are merging into smallest_head
		for (auto g : src_graph)
		{
			m_heads.at(g) = dst;
		}

        m_graphs.erase(src);
	}

	void log_group(std::shared_ptr<Node> head) const
	{
		NGRAPH_DEBUG << "Group leader : " << head->get_name() << std::endl;

		std::vector<std::string> sgroup;
		for (auto m : m_graphs.at(head))
		{
			sgroup.push_back(m->get_name());
		}
		NGRAPH_DEBUG << "Group members : " << vector_to_string(sgroup) << std::endl;
	}

	void collect_fusable_args(std::shared_ptr<Node> n)
	{
		for (auto arg : n->get_arguments())
		{
			//an argument is fusable and a part of some group
			NGRAPH_DEBUG << "Considering " << arg->get_name();
			if (m_heads.count(arg) != 0)
			{
				if (!m_arg_with_smallest_head)
				{
					m_arg_with_smallest_head = arg;
				}
				else
				{
					//out of two graphs pick one; ties are broken with the "<" operator
					m_arg_with_smallest_head = m_heads.at(arg) < m_heads.at(m_arg_with_smallest_head) ? arg : m_arg_with_smallest_head;
				}
				m_args_with_heads.push_back(arg);
			}
		}
	}

	NodeVector m_args_with_heads;
	std::shared_ptr<Node> m_arg_with_smallest_head;
	std::unordered_map<std::shared_ptr<Node>, NodeVector> m_graphs;
	std::unordered_map<std::shared_ptr<Node>, std::shared_ptr<Node>> m_heads;
};

TEST(cpu_fusion, graph_partition4)
{
	
    Shape shape{};
	auto a = make_shared<op::Parameter>(element::i32, shape);
    auto b = make_shared<op::Parameter>(element::i32, shape);
    auto c = make_shared<op::Parameter>(element::i32, shape);
	auto add_ab = a + b;
	auto add_abs = std::make_shared<op::Abs>(add_ab);
    auto abs_neg = std::make_shared<op::Negative>(add_abs);
    auto sub_c_neg = c - abs_neg;


    auto d = make_shared<op::Parameter>(element::i32, shape);
    auto d_abs = std::make_shared<op::Abs>(d);
    auto goe_ab = make_shared<op::GetOutputElement>(add_ab, 0);
    auto add_d = d_abs + goe_ab;
    auto neg_d = std::make_shared<op::Negative>(add_d);

    auto mul_cd = neg_d * sub_c_neg;
	auto f = std::make_shared<Function>(ngraph::NodeVector{ mul_cd }, op::ParameterVector{ a, b, c, d });

    const size_t MIN_NODES_TO_FUSE = 3;
    LoopKernelCollector lkc(f, MIN_NODES_TO_FUSE);
    const auto& graphs = lkc.get_graphs();

    NGRAPH_DEBUG << " PRUNED GROUPS : ";
    for (auto e : graphs)
    {
        log_group2(e.first, graphs);
    }
    ASSERT_EQ(graphs.size(), 1);
}

TEST(cpu_fusion, graph_partition5)
{
	
    Shape shape{};
	auto a = make_shared<op::Parameter>(element::i32, shape);
    auto b = make_shared<op::Parameter>(element::i32, shape);
    auto c = make_shared<op::Parameter>(element::i32, shape);
	auto add_ab = a + b;
	auto add_abs = std::make_shared<op::Abs>(add_ab);
    auto abs_neg = std::make_shared<op::Negative>(add_abs);
    auto sub_c_neg = c - abs_neg;


    auto d = make_shared<op::Parameter>(element::i32, shape);
    auto d_abs = std::make_shared<op::Abs>(d);
    auto goe_ab = make_shared<op::GetOutputElement>(add_ab, 0);
    auto add_d = d_abs + goe_ab;
    auto neg_d = std::make_shared<op::Negative>(add_d);

	auto f = std::make_shared<Function>(ngraph::NodeVector{ neg_d, sub_c_neg }, op::ParameterVector{ a, b, c, d });

    const size_t MIN_NODES_TO_FUSE = 3;
    LoopKernelCollector lkc(f, MIN_NODES_TO_FUSE);
    const auto& graphs = lkc.get_graphs();
    ASSERT_EQ(graphs.size(), 2);

    pass::Manager pass_manager;
    pass_manager.register_pass<pass::VisualizeTree>("graph.pdf");
    pass_manager.run_passes(f);
}