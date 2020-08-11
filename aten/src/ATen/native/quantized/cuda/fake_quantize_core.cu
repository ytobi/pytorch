#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>
#include <ATen/cuda/CUDAApplyUtils.cuh>
#include <ATen/native/quantized/fake_quant_affine.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/cuda/Loops.cuh>
#include <thrust/tuple.h>
#include <cmath>

/* Fake quantize a tensor
Args:
  output: output tensor.
  input : input tensor.
  sc:  scale to quantize the input tensor to
  zero_point: zero_point
  quant_min: minimum quantized value
  quant_max: maximum quantized value
Returns:
  Fake quantized tensor (float dtype).
*/
namespace at {
namespace native {
void fake_quantize_tensor_kernel_cuda(
    Tensor& output,
    const Tensor& input,
    float scale,
    int64_t zero_point,
    int64_t quant_min,
    int64_t quant_max) {
  // scalar type of this function is guaranteed to be float
  float inv_scale = 1.0f / scale;
  auto iter = TensorIteratorConfig()
    .check_all_same_dtype(false)
    .add_output(output)
    .add_input(input)
    .build();
  gpu_kernel(iter,
    [=] GPU_LAMBDA (float input_val) -> float {
      return (fminf(
                quant_max,
                fmaxf(
                    quant_min,
                    static_cast<int64_t>(std::nearbyint(
                        input_val * inv_scale + zero_point)))) -
            zero_point) *
          scale;
    });
}

void fake_quantize_grad_tensor_kernel_cuda(
    Tensor& input_grad,
    const Tensor& input,
    const Tensor& output_grad,
    float scale,
    int64_t zero_point,
    int64_t quant_min,
    int64_t quant_max) {
  // scalar type of this function is guaranteed to be float
  float inv_scale = 1.0f / scale;
  auto iter = TensorIteratorConfig()
    .check_all_same_dtype(false)
    .add_output(input_grad)
    .add_input(output_grad)
    .add_input(input)
    .build();
  gpu_kernel(iter,
    [=] GPU_LAMBDA (float dy, float x) -> float {
      int64_t Xq = std::nearbyint(x * inv_scale + zero_point);
      return (Xq >= quant_min && Xq <= quant_max) * dy;
    });
}

void _fake_quantize_grad_learnable_tensor_kernel_cuda(
    TensorIterator& iter,
    float scale,
    float inv_scale,
    int64_t zero_point,
    int64_t quant_min,
    int64_t quant_max) {
  float dscale_small = quant_min - zero_point;
  float dscale_big = quant_max - zero_point;
  gpu_kernel_multiple_outputs(
    iter, [=] GPU_LAMBDA (float XInput, float dYInput) -> thrust::tuple<float, float, float> {
      float dXOutput, dZeroPointOutput, dScaleOutput;
      int64_t xq = std::nearbyint(zero_point + XInput * inv_scale);
      dXOutput = dYInput * (xq >= quant_min && xq <= quant_max);
      xq = std::max(std::min(xq, quant_max), quant_min);
      float xfq = static_cast<float>((xq - zero_point) * scale);
      if (xq == quant_min || xq == quant_max) {
        dZeroPointOutput = (dYInput) * (-1) * scale;
        dScaleOutput = (xq == quant_min) ? (dYInput * dscale_small) : (dYInput * dscale_big);
      } else {
        dZeroPointOutput = 0;
        dScaleOutput = (dYInput) * (xfq - (XInput)) * inv_scale;
      }
      return {dXOutput, dScaleOutput, dZeroPointOutput};
  });
}

REGISTER_DISPATCH(fake_quant_tensor_stub, &fake_quantize_tensor_kernel_cuda);
REGISTER_DISPATCH(fake_quant_grad_tensor_stub, &fake_quantize_grad_tensor_kernel_cuda);
REGISTER_DISPATCH(fake_quant_grad_learnable_tensor_stub, &_fake_quantize_grad_learnable_tensor_kernel_cuda);

// Fake quantize per channel

void fake_quant_per_channel_cuda(TensorIterator &iter, int64_t quant_min, int64_t quant_max) {
  gpu_kernel(iter,
    [=] GPU_LAMBDA (float input_val, float scale, int64_t zero_point) -> float {
      float inv_scale = 1.0f / scale;
      return (fminf(
                quant_max,
                fmaxf(
                    quant_min,
                    static_cast<int64_t>(std::nearbyint(
                        input_val * inv_scale + zero_point)))) -
            zero_point) *
          scale;
    });
}

void fake_quant_grad_per_channel_cuda(TensorIterator &iter, int64_t quant_min, int64_t quant_max) {
  gpu_kernel(iter,
    [=] GPU_LAMBDA (float x, float dy, float scale, int64_t zero_point) -> float {
      float inv_scale = 1.0f / scale;
      int64_t Xq = std::nearbyint(x * inv_scale + zero_point);
      return (Xq >= quant_min && Xq <= quant_max) * dy;
    });
}

void _fake_quantize_grad_learnable_scale_channel_kernel_cuda(
    Tensor& input_grad,
    const Tensor& input,
    const Tensor& output_grad,
    float scale,
    int64_t zero_point,
    int64_t quant_min,
    int64_t quant_max) {
  // scalar type of this function is guaranteed to be float
  float inv_scale = 1.0f / scale;
  float grad_small = quant_min - zero_point;
  float grad_big = quant_max - zero_point;

  auto iter = TensorIterator::binary_op(input_grad, input, output_grad);
  gpu_kernel(iter,
    [=] GPU_LAMBDA (float x, float dy) -> float {
      int64_t xq = static_cast<int64_t>(zero_point + std::nearbyint(x * inv_scale));
      xq = std::max(std::min(xq, quant_max), quant_min);
      float x_fq = static_cast<float>((xq - zero_point) * scale);
      if (xq == quant_min) {
        return dy * grad_small;
      } else if (xq == quant_max) {
        return dy * grad_big;
      }
      return dy * (x_fq - x) * inv_scale;
    });
}

void _fake_quantize_grad_learnable_zero_point_channel_kernel_cuda(
    Tensor& input_grad,
    const Tensor& input,
    const Tensor& output_grad,
    float scale,
    int64_t zero_point,
    int64_t quant_min,
    int64_t quant_max) {
  // scalar type of this function is guaranteed to be float
  float inv_scale = 1.0f / scale;
  auto iter = TensorIterator::binary_op(input_grad, input, output_grad);
  gpu_kernel(iter,
    [=] GPU_LAMBDA (float x, float dy) -> float {
      int64_t xq = static_cast<int64_t>(zero_point + std::nearbyint(x * inv_scale));
      xq = std::max(std::min(xq, quant_max), quant_min);
      if (xq == quant_min || xq == quant_max) {
        return dy * (-1) * scale;
      }
      return 0;
    });
}

REGISTER_DISPATCH(fake_quant_per_channel_stub, &fake_quant_per_channel_cuda);
REGISTER_DISPATCH(fake_quant_grad_per_channel_stub, &fake_quant_grad_per_channel_cuda);
REGISTER_DISPATCH(fake_quant_grad_learnable_scale_channel_stub, &_fake_quantize_grad_learnable_scale_channel_kernel_cuda);
REGISTER_DISPATCH(fake_quant_grad_learnable_zero_point_channel_stub, &_fake_quantize_grad_learnable_zero_point_channel_kernel_cuda);

} // namespace native
} // namespace at
