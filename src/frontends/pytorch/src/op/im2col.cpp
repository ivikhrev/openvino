// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "openvino/frontend/pytorch/node_context.hpp"
#include "openvino/opsets/opset10.hpp"
#include "utils.hpp"

namespace ov {
namespace frontend {
namespace pytorch {
namespace op {

namespace {
std::shared_ptr<Node> get_im2col_indices_along_dim(NodeContext& context,
                                                   ov::Output<Node> input_d,
                                                   int64_t kernel_size_d,
                                                   int64_t dilation_d,
                                                   int64_t padding_d,
                                                   int64_t stride_d) {
    auto zero = context.mark_node(opset10::Constant::create(element::i64, Shape{}, {0}));
    auto minus_one = context.mark_node(opset10::Constant::create(element::i64, Shape{}, {-1}));
    auto kernel_size = context.mark_node(opset10::Constant::create(element::i64, Shape{}, {kernel_size_d}));
    auto padding_2 = context.mark_node(opset10::Constant::create(element::i64, Shape{}, {padding_d * 2}));
    auto stride = context.mark_node(opset10::Constant::create(element::i64, Shape{}, {stride_d}));
    auto input_d_squeezed = context.mark_node(std::make_shared<opset10::Squeeze>(input_d, zero));
    auto blocks_d = context.mark_node(std::make_shared<opset10::Add>(input_d_squeezed, padding_2));
    auto subtrahend =
        context.mark_node(opset10::Constant::create(element::i64, Shape{}, {dilation_d * (kernel_size_d - 1)}));
    blocks_d = context.mark_node(std::make_shared<opset10::Subtract>(blocks_d, subtrahend));
    auto blocks_d_indices = context.mark_node(std::make_shared<opset10::Range>(zero, blocks_d, stride, element::i64));
    blocks_d_indices = context.mark_node(std::make_shared<opset10::Unsqueeze>(blocks_d_indices, zero));
    std::vector<int64_t> rng;
    for (int64_t i = 0; i < kernel_size_d * dilation_d; i += dilation_d) {
        rng.push_back(i);
    }

    auto kernel_grid = context.mark_node(opset10::Constant::create(element::i64, Shape{rng.size()}, rng));
    auto kernel_mask = context.mark_node(std::make_shared<opset10::Unsqueeze>(kernel_grid, minus_one));
    return context.mark_node(std::make_shared<opset10::Add>(blocks_d_indices, kernel_mask));
}
}  // namespace

OutputVector translate_im2col(NodeContext& context) {
    auto input = context.get_input(0);
    auto kernel_size = context.const_input<std::vector<int64_t>>(1);
    FRONT_END_OP_CONVERSION_CHECK(kernel_size.size() == 2, "kernel size should contains 2 elements");
    auto dilation = context.const_input<std::vector<int64_t>>(2);
    FRONT_END_OP_CONVERSION_CHECK(kernel_size.size() == 2, "dilation should contains 2 elements");
    auto padding = context.const_input<std::vector<int64_t>>(3);
    FRONT_END_OP_CONVERSION_CHECK(kernel_size.size() == 2, "padding should contains 2 elements");
    auto stride = context.const_input<std::vector<int64_t>>(4);
    FRONT_END_OP_CONVERSION_CHECK(kernel_size.size() == 2, "stride should contains 2 elements");
    auto zero = context.mark_node(opset10::Constant::create(element::i64, Shape{}, {0}));
    auto input_shape = context.mark_node(std::make_shared<opset10::ShapeOf>(input));
    auto zero_f = context.mark_node(opset10::Constant::create(element::f32, Shape{}, {0}));
    auto minus_one = context.mark_node(opset10::Constant::create(element::i64, Shape{1}, {-1}));
    auto two = context.mark_node(opset10::Constant::create(element::i64, Shape{}, {2}));
    auto four = context.mark_node(opset10::Constant::create(element::i64, Shape{}, {4}));
    auto input_shape_split = context.mark_node(std::make_shared<opset10::Split>(input_shape, zero, 4));
    auto input_b = input_shape_split->output(0);
    auto input_c = input_shape_split->output(1);
    auto input_h = input_shape_split->output(2);
    auto input_w = input_shape_split->output(3);
    auto stride_h = stride[0];
    auto stride_w = stride[1];
    auto padding_h = padding[0];
    auto padding_w = padding[1];
    auto dilation_h = dilation[0];
    auto dilation_w = dilation[1];
    auto kernel_h = kernel_size[0];
    auto kernel_w = kernel_size[1];
    auto blocks_row_indices = get_im2col_indices_along_dim(context, input_h, kernel_h, dilation_h, padding_h, stride_h);
    auto blocks_col_indices = get_im2col_indices_along_dim(context, input_w, kernel_w, dilation_w, padding_w, stride_w);
    auto kernel_window = context.mark_node(opset10::Constant::create(element::i64, Shape{}, {kernel_h * kernel_w}));
    auto input_c_squeezed = context.mark_node(std::make_shared<opset10::Squeeze>(input_c, zero));
    auto channel_unfolded = context.mark_node(std::make_shared<opset10::Multiply>(input_c_squeezed, kernel_window));
    auto channel_unfolded_unsqueezed = context.mark_node(std::make_shared<opset10::Unsqueeze>(channel_unfolded, zero));
    auto output_shape = context.mark_node(
        std::make_shared<opset10::Concat>(OutputVector{input_b, channel_unfolded_unsqueezed, minus_one}, 0));
    auto pads = context.mark_node(
        opset10::Constant::create(element::i64, Shape{4}, std::vector<int64_t>{0, 0, padding_h, padding_w}));
    auto padded_input =
        context.mark_node(std::make_shared<opset10::Pad>(input, pads, pads, zero_f, ov::op::PadMode::CONSTANT));
    auto output = context.mark_node(std::make_shared<opset10::Gather>(padded_input, blocks_row_indices, two));
    output = context.mark_node(std::make_shared<opset10::Gather>(output, blocks_col_indices, four));
    auto permutation_dims =
        context.mark_node(opset10::Constant::create(element::i64, Shape{6}, std::vector<int64_t>{0, 1, 2, 4, 3, 5}));
    output = context.mark_node(std::make_shared<opset10::Transpose>(output, permutation_dims));
    return {context.mark_node(std::make_shared<opset10::Reshape>(output, output_shape, false))};
};

}  // namespace op
}  // namespace pytorch
}  // namespace frontend
}  // namespace ov