// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kernels/funcs/conv_util.h"
#include "kernels/funcs/npu_funcs.h"
#include "kernels/funcs/npu_op_runner.h"

namespace custom_kernel {

template <typename T, typename Context>
void FillKernel(const Context& dev_ctx,
                const phi::DenseTensor& x,
                const phi::Scalar& val);

template <typename T, typename Context>
void TransposeKernel(const Context& dev_ctx,
                     const phi::DenseTensor& x,
                     const std::vector<int>& axis,
                     phi::DenseTensor* out);

template <typename T, typename Context>
void AclopDepthwiseConv2dKernel(const Context& dev_ctx,
                                const phi::DenseTensor& input,
                                const phi::DenseTensor& transformed_filter,
                                const std::vector<int>& stride,
                                const std::vector<int>& padding,
                                const std::vector<int>& dilation,
                                const std::string& data_format,
                                phi::DenseTensor* out,
                                const bool& channel_last) {
  auto stream = dev_ctx.stream();

  std::vector<int> strides(4, 1);
  std::vector<int> dilations(4, 1);

  phi::DenseTensor input_tensor(input), output_tensor(*out);

  if (channel_last) {
    phi::DenseTensorMeta input_meta = {
        input.dtype(), input.dims(), phi::DataLayout::kNHWC};
    phi::DenseTensorMeta output_meta = {
        out->dtype(), out->dims(), phi::DataLayout::kNHWC};
    input_tensor.set_meta(input_meta);
    output_tensor.set_meta(output_meta);
    strides[1] = stride[0];
    strides[2] = stride[1];
    dilations[1] = dilation[0];
    dilations[2] = dilation[1];
  } else {
    strides[2] = stride[0];
    strides[3] = stride[1];
    dilations[2] = dilation[0];
    dilations[3] = dilation[1];
  }

  const auto& runner = NpuOpRunner("DepthwiseConv2D",
                                   {input_tensor, transformed_filter},
                                   {output_tensor},
                                   {{"strides", strides},
                                    {"dilations", dilations},
                                    {"pads", padding},
                                    {"data_format", data_format}});
  runner.Run(stream);
}

template <typename T, typename Context>
void DepthwiseConv2dKernel(const Context& dev_ctx,
                           const phi::DenseTensor& input,
                           const phi::DenseTensor& filter,
                           const std::vector<int>& stride,
                           const std::vector<int>& paddings_in,
                           const std::string& padding_algorithm,
                           int groups,
                           const std::vector<int>& dilations_in,
                           const std::string& data_format,
                           phi::DenseTensor* out) {
  dev_ctx.template Alloc<T>(out);

  std::vector<int> padding = paddings_in;
  std::vector<int> dilation = dilations_in;

  const bool channel_last = data_format == "NHWC";
  auto in_dims = input.dims();
  auto filter_dims = filter.dims();
  phi::DDim in_data_dims;
  phi::DDim filter_data_dims;

  if (channel_last) {
    in_data_dims = phi::slice_ddim(in_dims, 1, in_dims.size() - 1);
  } else {
    in_data_dims = phi::slice_ddim(in_dims, 2, in_dims.size());
  }
  filter_data_dims = phi::slice_ddim(filter_dims, 2, in_dims.size());

  std::vector<int> ksize = phi::vectorize<int>(filter_data_dims);
  custom_kernel::UpdatePaddingAndDilation(
      &padding, &dilation, padding_algorithm, in_data_dims, stride, ksize);

  // Transform filter (n, 1, h, w) --> (1, n, h, w)
  phi::DenseTensor transformed_filter;
  phi::DenseTensorMeta meta = {
      filter.dtype(),
      {filter.dims()[1], filter.dims()[0], filter.dims()[2], filter.dims()[3]}};

  transformed_filter.set_meta(meta);
  dev_ctx.template Alloc<T>(&transformed_filter);
  std::vector<int> perm = {1, 0, 2, 3};
  custom_kernel::TransposeKernel<T, Context>(
      dev_ctx, filter, perm, &transformed_filter);

  DO_COMPATIBILITY(
      aclnnConvDepthwise2d,
      (custom_kernel::AclopDepthwiseConv2dKernel<T, Context>(dev_ctx,
                                                             input,
                                                             transformed_filter,
                                                             stride,
                                                             padding,
                                                             dilation,
                                                             data_format,
                                                             out,
                                                             channel_last)));

  phi::DenseTensor input_tensor(input), output_tensor(*out);

  if (channel_last) {
    phi::DenseTensorMeta input_meta = {
        input.dtype(), input.dims(), phi::DataLayout::kNHWC};
    phi::DenseTensorMeta output_meta = {
        out->dtype(), out->dims(), phi::DataLayout::kNHWC};
    input_tensor.set_meta(input_meta);
    output_tensor.set_meta(output_meta);
  }

  phi::DenseTensor bias;
  phi::DenseTensorMeta bias_meta = {input_tensor.dtype(), phi::make_ddim({1})};
  bias.set_meta(bias_meta);

  dev_ctx.template Alloc<T>(&bias);
  custom_kernel::FillKernel<T, Context>(dev_ctx, bias, 0.f);

  const int8_t cubeMathType = 0;

  EXEC_NPU_CMD(aclnnConvDepthwise2d,
               dev_ctx,
               input_tensor,
               transformed_filter,
               ksize,
               bias,
               stride,
               padding,
               dilation,
               output_tensor,
               cubeMathType);
}

template <typename T, typename Context>
void AclopDepthwiseConv2dGradKernel(const Context& dev_ctx,
                                    const phi::DenseTensor& input_tensor,
                                    const phi::DenseTensor& output_grad_tensor,
                                    const phi::DenseTensor& transformed_filter,
                                    const std::vector<int>& stride,
                                    const std::vector<int>& dilation,
                                    const std::vector<int>& padding,
                                    const std::string& data_format,
                                    const bool& channel_last,
                                    phi::DenseTensor* input_grad,
                                    phi::DenseTensor* filter_grad) {
  auto stream = dev_ctx.stream();

  // construct NPU attr
  std::vector<int> strides(4, 1);
  std::vector<int> dilations(4, 1);
  if (channel_last) {
    strides[1] = stride[0];
    strides[2] = stride[1];
    dilations[1] = dilation[0];
    dilations[2] = dilation[1];
  } else {
    strides[2] = stride[0];
    strides[3] = stride[1];
    dilations[2] = dilation[0];
    dilations[3] = dilation[1];
  }

  if (filter_grad) {
    dev_ctx.template Alloc<T>(filter_grad);
    // DepthwiseConv2DBackpropFilterD only support fp32 output, so we need cast
    // the output when the out dtype is fp16.
    phi::DenseTensor filter_grad_tmp;
    if (filter_grad->dtype() == phi::DataType::FLOAT16) {
      filter_grad_tmp.Resize(filter_grad->dims());
      dev_ctx.template Alloc<float>(&filter_grad_tmp);
    } else {
      filter_grad_tmp = *filter_grad;
    }
    NpuOpRunner runner;
    runner.SetType("DepthwiseConv2DBackpropFilterD")
        .AddInput(input_tensor)
        .AddInput(output_grad_tensor)
        .AddOutput(filter_grad_tmp)
        .AddAttr("filter_size", phi::vectorize(transformed_filter.dims()))
        .AddAttr("strides", strides)
        .AddAttr("dilations", dilations)
        .AddAttr("pads", padding)
        .AddAttr("data_format", data_format)
        .Run(stream);
    dev_ctx.Wait();
    if (filter_grad->dtype() == phi::DataType::FLOAT16) {
      const auto& cast_runner = NpuOpRunner("Cast",
                                            {filter_grad_tmp},
                                            {*filter_grad},
                                            {{"dst_type", ACL_FLOAT16}});
      cast_runner.Run(stream);
    }
  }

  if (input_grad) {
    dev_ctx.template Alloc<T>(input_grad);

    phi::DenseTensor input_grad_tensor(*input_grad);
    if (channel_last) {
      phi::DenseTensorMeta input_grad_meta = {
          input_grad->dtype(), input_grad->dims(), phi::DataLayout::kNHWC};
      input_grad_tensor.set_meta(input_grad_meta);
    }
    NpuOpRunner runner;
    runner.SetType("DepthwiseConv2DBackpropInputD")
        .AddInput(transformed_filter)
        .AddInput(output_grad_tensor)
        .AddOutput(input_grad_tensor)
        .AddAttr("input_size", phi::vectorize(input_tensor.dims()))
        .AddAttr("strides", strides)
        .AddAttr("dilations", dilations)
        .AddAttr("pads", padding)
        .AddAttr("data_format", data_format)
        .Run(stream);
  }
}

template <typename T, typename Context>
void DepthwiseConv2dGradKernel(const Context& dev_ctx,
                               const phi::DenseTensor& input,
                               const phi::DenseTensor& filter,
                               const phi::DenseTensor& out_grad,
                               const std::vector<int>& stride,
                               const std::vector<int>& paddings_in,
                               const std::string& padding_algorithm,
                               int groups,
                               const std::vector<int>& dilations_in,
                               const std::string& data_format,
                               phi::DenseTensor* input_grad,
                               phi::DenseTensor* filter_grad) {
  std::vector<int> padding = paddings_in;
  std::vector<int> dilation = dilations_in;
  const bool channel_last = data_format == "NHWC";
  // update padding and dilation
  auto in_dims = input.dims();
  auto filter_dims = filter.dims();
  phi::DDim in_data_dims;
  phi::DDim filter_data_dims;

  if (channel_last) {
    in_data_dims = phi::slice_ddim(in_dims, 1, in_dims.size() - 1);
  } else {
    in_data_dims = phi::slice_ddim(in_dims, 2, in_dims.size());
  }
  filter_data_dims = phi::slice_ddim(filter_dims, 2, in_dims.size());

  std::vector<int> ksize = phi::vectorize<int>(filter_data_dims);
  custom_kernel::UpdatePaddingAndDilation(
      &padding, &dilation, padding_algorithm, in_data_dims, stride, ksize);

  auto stream = dev_ctx.stream();

  // Transform filter (n, 1, h, w) --> (1, n, h, w)
  phi::DenseTensor transformed_filter;
  phi::DenseTensorMeta meta = {
      filter.dtype(),
      {filter.dims()[1], filter.dims()[0], filter.dims()[2], filter.dims()[3]}};

  transformed_filter.set_meta(meta);
  dev_ctx.template Alloc<T>(&transformed_filter);
  std::vector<int> perm = {1, 0, 2, 3};
  custom_kernel::TransposeKernel<T, Context>(
      dev_ctx, filter, perm, &transformed_filter);

  // construct NPU attr
  phi::DenseTensor input_tensor(input), output_grad_tensor(out_grad);
  if (channel_last) {
    phi::DenseTensorMeta input_meta = {
        input.dtype(), input.dims(), phi::DataLayout::kNHWC};
    phi::DenseTensorMeta output_grad_meta = {
        out_grad.dtype(), out_grad.dims(), phi::DataLayout::kNHWC};
    input_tensor.set_meta(input_meta);
    output_grad_tensor.set_meta(output_grad_meta);
  }

  DO_COMPATIBILITY(aclnnConvolutionBackward,
                   (custom_kernel::AclopDepthwiseConv2dGradKernel<T, Context>(
                       dev_ctx,
                       input_tensor,
                       output_grad_tensor,
                       transformed_filter,
                       stride,
                       dilation,
                       padding,
                       data_format,
                       channel_last,
                       input_grad,
                       filter_grad)));

  std::vector<int64_t> biasSizes =
      phi::vectorize<int64_t>(phi::slice_ddim(transformed_filter.dims(), 0, 1));
  const bool transposed = false;
  std::vector<int64_t> outputPaddding = {0};
  bool outputMask[3] = {input_grad != nullptr, filter_grad != nullptr, false};
  const int8_t cubeMathType = 0;

  phi::DenseTensor bias_grad;
  phi::DenseTensorMeta bias_grad_meta = {input_tensor.dtype(),
                                         phi::make_ddim({1})};
  bias_grad.set_meta(bias_grad_meta);

  dev_ctx.template Alloc<T>(&bias_grad);
  custom_kernel::FillKernel<T, Context>(dev_ctx, bias_grad, 0.f);

  dev_ctx.template Alloc<T>(filter_grad);
  dev_ctx.template Alloc<T>(input_grad);

  EXEC_NPU_CMD(aclnnConvolutionBackward,
               dev_ctx,
               output_grad_tensor,
               input_tensor,
               transformed_filter,
               biasSizes,
               stride,
               padding,
               dilation,
               transposed,
               outputPaddding,
               groups,
               outputMask,
               cubeMathType,
               input_grad,
               filter_grad,
               bias_grad);
}

template <typename T, typename Context>
void Conv3dKernel(const Context& dev_ctx,
                  const phi::DenseTensor& input,
                  const phi::DenseTensor& filter,
                  const std::vector<int>& strides,
                  const std::vector<int>& padding,
                  const std::string& padding_algorithm,
                  int groups,
                  const std::vector<int>& dilation,
                  const std::string& data_format,
                  phi::DenseTensor* out) {
  auto paddings = padding;
  auto dilations = dilation;

  PADDLE_ENFORCE_EQ(data_format,
                    "NCDHW",
                    phi::errors::Unimplemented(
                        "the data_format must be NCDHW in "
                        "the npu kernel of conv3d, but got data_format "
                        "= [%s]",
                        data_format));

  PADDLE_ENFORCE_EQ(
      groups,
      1,
      phi::errors::Unimplemented("the groups must be 1 in "
                                 "the npu kernel of conv3d, but got groups "
                                 "= [%d]",
                                 groups));

  dev_ctx.template Alloc<T>(out);

  phi::DenseTensor input_tensor(input);
  phi::DenseTensor filter_tensor(filter);
  phi::DenseTensor output_tensor(*out);

  phi::DenseTensorMeta input_meta = {
      input_tensor.dtype(), input_tensor.dims(), phi::DataLayout::kNCDHW};
  input_tensor.set_meta(input_meta);

  phi::DenseTensorMeta filter_meta = {
      filter_tensor.dtype(), filter_tensor.dims(), phi::DataLayout::kNCDHW};
  filter_tensor.set_meta(filter_meta);

  phi::DenseTensorMeta output_meta = {
      output_tensor.dtype(), output_tensor.dims(), phi::DataLayout::kNCDHW};
  output_tensor.set_meta(output_meta);

  // update padding and dilation
  auto in_dims = input.dims();
  auto filter_dims = filter.dims();
  phi::DDim in_data_dims;
  phi::DDim filter_data_dims;

  in_data_dims = phi::slice_ddim(in_dims, 2, in_dims.size());
  filter_data_dims = phi::slice_ddim(filter_dims, 2, in_dims.size());

  std::vector<int> ksize = phi::vectorize<int>(filter_data_dims);
  custom_kernel::UpdatePaddingAndDilation(
      &paddings, &dilations, padding_algorithm, in_data_dims, strides, ksize);

  std::vector<int> strides_vec(5, 1);
  std::vector<int> dilations_vec(5, 1);

  strides_vec[2] = strides[0];
  strides_vec[3] = strides[1];
  strides_vec[4] = strides[2];
  dilations_vec[2] = dilations[0];
  dilations_vec[3] = dilations[1];
  dilations_vec[4] = dilations[2];

  auto stream = dev_ctx.stream();
  const auto& runner = NpuOpRunner("Conv3D",
                                   {input_tensor, filter_tensor},
                                   {output_tensor},
                                   {{"strides", strides_vec},
                                    {"pads", paddings},
                                    {"dilations", dilations_vec},
                                    {"groups", groups},
                                    {"data_format", data_format}});
  runner.Run(stream);
}

template <typename T, typename Context>
void Conv3dGradKernel(const Context& dev_ctx,
                      const phi::DenseTensor& input,
                      const phi::DenseTensor& filter,
                      const phi::DenseTensor& out_grad,
                      const std::vector<int>& strides,
                      const std::vector<int>& padding,
                      const std::string& padding_algorithm,
                      int groups,
                      const std::vector<int>& dilation,
                      const std::string& data_format,
                      phi::DenseTensor* input_grad,
                      phi::DenseTensor* filter_grad) {
  auto paddings = padding;
  auto dilations = dilation;

  PADDLE_ENFORCE_EQ(data_format,
                    "NCDHW",
                    phi::errors::Unimplemented(
                        "the data_format must be NCDHW in "
                        "the npu kernel of conv3d, but got data_format "
                        "= [%s]",
                        data_format));

  PADDLE_ENFORCE_EQ(
      groups,
      1,
      phi::errors::Unimplemented("the groups must be 1 in "
                                 "the npu kernel of conv3d, but got groups "
                                 "= [%d]",
                                 groups));

  phi::DenseTensor input_tensor(input);
  phi::DenseTensor filter_tensor(filter);
  phi::DenseTensor output_grad_tensor(out_grad);

  phi::DenseTensorMeta input_meta = {
      input_tensor.dtype(), input_tensor.dims(), phi::DataLayout::kNCDHW};
  input_tensor.set_meta(input_meta);

  phi::DenseTensorMeta filter_meta = {
      filter_tensor.dtype(), filter_tensor.dims(), phi::DataLayout::kNCDHW};
  filter_tensor.set_meta(filter_meta);

  phi::DenseTensorMeta output_meta = {output_grad_tensor.dtype(),
                                      output_grad_tensor.dims(),
                                      phi::DataLayout::kNCDHW};
  output_grad_tensor.set_meta(output_meta);

  // update padding and dilation
  auto in_dims = input.dims();
  auto filter_dims = filter.dims();
  phi::DDim in_data_dims;
  phi::DDim filter_data_dims;

  in_data_dims = phi::slice_ddim(in_dims, 1, in_dims.size() - 1);
  filter_data_dims = phi::slice_ddim(filter_dims, 2, in_dims.size());

  std::vector<int> ksize = phi::vectorize<int>(filter_data_dims);
  custom_kernel::UpdatePaddingAndDilation(
      &paddings, &dilations, padding_algorithm, in_data_dims, strides, ksize);

  std::vector<int> strides_vec(5, 1);
  std::vector<int> dilations_vec(5, 1);

  strides_vec[2] = strides[0];
  strides_vec[3] = strides[1];
  strides_vec[4] = strides[2];
  dilations_vec[2] = dilations[0];
  dilations_vec[3] = dilations[1];
  dilations_vec[4] = dilations[2];

  auto stream = dev_ctx.stream();

  if (filter_grad) {
    dev_ctx.template Alloc<T>(filter_grad);
    std::vector<int> filter_shape_vec = phi::vectorize<int>(filter.dims());

    phi::DenseTensor filter_grad_tensor(*filter_grad);
    phi::DenseTensorMeta filter_grad_meta = {filter_grad_tensor.dtype(),
                                             filter_grad_tensor.dims(),
                                             phi::DataLayout::kNCDHW};
    filter_grad_tensor.set_meta(filter_grad_meta);

    // Conv3DBackpropFilterD only support fp32 output, so we need cast the
    // output when the out dtype is fp16.
    phi::DenseTensor filter_grad_tmp;
    if (filter_grad->dtype() == phi::DataType::FLOAT16) {
      phi::DenseTensorMeta filter_grad_tmp_meta = {phi::DataType::FLOAT32,
                                                   filter_grad_tensor.dims(),
                                                   phi::DataLayout::kNCDHW};
      filter_grad_tmp.set_meta(filter_grad_tmp_meta);
      dev_ctx.template Alloc<float>(&filter_grad_tmp);
    } else {
      filter_grad_tmp = filter_grad_tensor;
    }

    const auto& runner = NpuOpRunner("Conv3DBackpropFilterD",
                                     {input_tensor, output_grad_tensor},
                                     {filter_grad_tmp},
                                     {{"filter_size", filter_shape_vec},
                                      {"strides", strides_vec},
                                      {"pads", paddings},
                                      {"dilations", dilations_vec},
                                      {"groups", groups},
                                      {"data_format", data_format}});
    runner.Run(stream);
    dev_ctx.Wait();
    if (filter_grad->dtype() == phi::DataType::FLOAT16) {
      const auto& cast_runner = NpuOpRunner("Cast",
                                            {filter_grad_tmp},
                                            {*filter_grad},
                                            {{"dst_type", ACL_FLOAT16}});
      cast_runner.Run(stream);
    }
  }

  if (input_grad) {
    dev_ctx.template Alloc<T>(input_grad);
    std::vector<int> input_shape_vec = phi::vectorize<int>(input.dims());

    phi::DenseTensor input_grad_tensor(*input_grad);
    phi::DenseTensorMeta input_grad_meta = {input_grad_tensor.dtype(),
                                            input_grad_tensor.dims(),
                                            phi::DataLayout::kNCDHW};
    input_grad_tensor.set_meta(input_grad_meta);

    const auto& runner = NpuOpRunner("Conv3DBackpropInputD",
                                     {filter_tensor, output_grad_tensor},
                                     {input_grad_tensor},
                                     {{"input_size", input_shape_vec},
                                      {"strides", strides_vec},
                                      {"pads", paddings},
                                      {"dilations", dilations_vec},
                                      {"groups", groups},
                                      {"data_format", data_format}});
    runner.Run(stream);
  }
}

}  // namespace custom_kernel

PD_REGISTER_PLUGIN_KERNEL(depthwise_conv2d,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::DepthwiseConv2dKernel,
                          float,
                          phi::dtype::float16) {}

PD_REGISTER_PLUGIN_KERNEL(depthwise_conv2d_grad,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::DepthwiseConv2dGradKernel,
                          float,
                          phi::dtype::float16) {}

PD_REGISTER_PLUGIN_KERNEL(conv3d,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::Conv3dKernel,
                          float,
                          phi::dtype::float16) {}

PD_REGISTER_PLUGIN_KERNEL(conv3d_grad,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::Conv3dGradKernel,
                          float,
                          phi::dtype::float16) {}
