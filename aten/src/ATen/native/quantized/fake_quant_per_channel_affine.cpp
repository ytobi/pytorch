#include <ATen/ATen.h>
#include <ATen/Dispatch.h>
#include <ATen/NativeFunctions.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/cpu/Loops.h>
#include <ATen/native/quantized/fake_quant_affine.h>

// FakeQuantize Op for PerChannelAffine quantization scheme.
namespace at {
namespace native {

// Use REGISTER_DISPATCH to run CPU and CUDA backend.
DEFINE_DISPATCH(fake_quant_per_channel_stub);
DEFINE_DISPATCH(fake_quant_grad_per_channel_stub);
DEFINE_DISPATCH(fake_quant_grad_learnable_scale_channel_stub);
DEFINE_DISPATCH(fake_quant_grad_learnable_zero_point_channel_stub);

/* Per channel fake-quantizes the 'inputs' tensor.
Args:
  X: Forward input tensor.
  dY: Backward input tensor (_backward op only).
  scale: scale of per channel affine quantization
  zero_point: zero_point of per channel affine quantization
  axis: int specifying the axis to be quantized
  quant_min: minimum quantized value
  quant_max: maximum quantized value
Returns:
  Fake quantized tensor (double dtype).

*/

Tensor fake_quantize_per_channel_affine(
    const Tensor& self,
    const Tensor& scale,
    const Tensor& zero_point,
    int64_t axis,
    int64_t quant_min,
    int64_t quant_max) {
  TORCH_CHECK(self.scalar_type() == ScalarType::Float);
  TORCH_CHECK(scale.scalar_type() == ScalarType::Float,
              "Scale must be Float, found ", scale.scalar_type());
  TORCH_CHECK(zero_point.scalar_type() == ScalarType::Long,
              "Zero-point must be Long, found ", zero_point.scalar_type());
  TORCH_CHECK(scale.dim() == 1, "scale should be a 1-D tensor");
  TORCH_CHECK(zero_point.dim() == 1, "zero point should be a 1-D tensor");
  TORCH_CHECK(
      scale.numel() == zero_point.numel(),
      "scale and zero-point need to have the same dimensions");
  TORCH_CHECK(
      scale.numel() == self.size(axis),
      "dimensions of scale and zero-point are not consistent with input tensor")

  TORCH_CHECK(
      quant_min <= quant_max,
      "`quant_min` should be less than or \
        equal to `quant_max`.");

  TORCH_CHECK(
      at::min(zero_point).item().toLong() >= quant_min &&
          at::max(zero_point).item().toLong() <= quant_max,
      "`zero_point` must be between `quant_min` and `quant_max`.");

  TORCH_CHECK(
      axis >= 0 && axis <= self.dim(),
      "`axis` must be between 0 and number of dimensions of input");

  auto Y = at::empty_like(self, self.options(), MemoryFormat::Preserve);

  std::vector<int64_t> expected_shape(self.dim(), 1);
  expected_shape[axis] = self.size(axis);

  TensorIterator iter = TensorIteratorConfig()
    .check_all_same_dtype(false)
    .add_output(Y)
    .add_input(self)
    .add_input(native::_unsafe_view(scale, expected_shape))
    .add_input(native::_unsafe_view(zero_point, expected_shape))
    .build();

  fake_quant_per_channel_stub(iter.device_type(), iter, quant_min, quant_max);

  return Y;
}

/* Backward path for per-channel fake-quantization of the 'inputs' tensor.

Args:
  X: Forward input tensor.
  dY: Backward input tensor.
  scale: scale of per tensor affine quantization
  zero_point: zero_point of per tensor affine quantization
  axis: int ,the axis over which quantization parameters vary
  quant_min: int, minimum quantized value
  quant_max: int, maximum quantized value

Returns:
  Gradient for per channel fake quant (double dtype).

*/
Tensor fake_quantize_per_channel_affine_backward(
    const Tensor& dY,
    const Tensor& X,
    const Tensor& scale,
    const Tensor& zero_point,
    int64_t axis,
    int64_t quant_min,
    int64_t quant_max) {
  TORCH_CHECK(dY.scalar_type() == ScalarType::Float);
  TORCH_CHECK(X.scalar_type() == ScalarType::Float);
  TORCH_CHECK(scale.scalar_type() == ScalarType::Float,
              "Scale must be Float, found ", scale.scalar_type());
  TORCH_CHECK(zero_point.scalar_type() == ScalarType::Long,
              "Zero-point must be Long, found ", zero_point.scalar_type());

  TORCH_CHECK(X.sizes() == dY.sizes(), "`X` and `dY` are not the same size");
  TORCH_CHECK(
      quant_min <= quant_max,
      "`quant_min` should be less than or \
        equal to `quant_max`.");
  TORCH_CHECK(scale.dim() == 1, "scale should be a 1-D tensor");
  TORCH_CHECK(zero_point.dim() == 1, "zero point should be a 1-D tensor");
  TORCH_CHECK(
      scale.numel() == zero_point.numel(),
      "scale and zero-point need to have the same dimensions");
  TORCH_CHECK(
      scale.numel() == X.size(axis),
      "dimensions of scale and zero-point are not consistent with input tensor")

  TORCH_CHECK(
      quant_min <= quant_max,
      "`quant_min` should be less than or \
        equal to `quant_max`.");

  TORCH_CHECK(
      at::min(zero_point).item().toLong() >= quant_min &&
          at::max(zero_point).item().toLong() <= quant_max,
      "`zero_point` must be between `quant_min` and `quant_max`.");

  TORCH_CHECK(
      axis >= 0 && axis <= X.dim(),
      "`axis` must be between 0 and number of dimensions of input");

  if (X.numel() <= 0) {
    return X;
  }

  auto dX = at::empty_like(X, X.options(), MemoryFormat::Preserve);

  std::vector<int64_t> expected_shape(X.dim(), 1);
  expected_shape[axis] = X.size(axis);

  TensorIterator iter = TensorIteratorConfig()
    .check_all_same_dtype(false)
    .add_output(dX)
    .add_input(X)
    .add_input(dY)
    .add_input(native::_unsafe_view(scale, expected_shape))
    .add_input(native::_unsafe_view(zero_point, expected_shape))
    .build();

  fake_quant_grad_per_channel_stub(iter.device_type(), iter, quant_min, quant_max);

  return dX;
}

TensorIterator _build_iterator(
    const Tensor& dX,
    const Tensor& X,
    const Tensor& dY,
    const Tensor& scale,
    const Tensor& zero_point,
    std::vector<int64_t> expected_shape) {
  TensorIterator iter = TensorIteratorConfig()
    .check_all_same_dtype(false)
    .add_output(dX)
    .add_input(X)
    .add_input(dY)
    .add_input(native::_unsafe_view(scale, expected_shape))
    .add_input(native::_unsafe_view(zero_point, expected_shape))
    .build();
  return iter;
}

Tensor _get_rounded_zero_point(
    const Tensor& zero_point,
    int64_t quant_min,
    int64_t quant_max) {
  // This assumes the per channel zero point vector is single-dimensioned.
  for (int i = 0; i < zero_point.sizes()[0]; ++i) {
    zero_point[i] = static_cast<int64_t>(zero_point[i].item<float>() + 0.5);
  }
  return zero_point.clamp(quant_min, quant_max).to(at::kLong);
}

std::tuple<Tensor, Tensor> _get_scale_zero_point_per_channel_iter_grads(
    const Tensor& dY,
    const Tensor& X,
    const Tensor& scale,
    const Tensor& zero_point,
    int64_t axis,
    int64_t quant_min,
    int64_t quant_max) {

  int64_t axis_size = X.size(axis);
  std::vector<Tensor> X_flattened = at::unbind(X, axis);
  std::vector<Tensor> dY_flattened = at::unbind(dY, axis);

  Tensor dScale = at::zeros({scale.sizes()[0]});
  Tensor dZeroPoint = at::zeros({zero_point.sizes()[0]});

  for (int i = 0; i < X_flattened.size(); ++i) {
    Tensor X_i = X_flattened[i];
    Tensor dY_i = dY_flattened[i];
    auto dScale_item_vec = at::empty_like(X_i, X_i.options(), MemoryFormat::Preserve);
    auto dZeroPoint_item_vec = at::empty_like(X_i, X_i.options(), MemoryFormat::Preserve);

    float scale_i = scale[i].item<float>();
    int64_t zero_point_i = static_cast<int64_t>(std::min(std::max(zero_point[i].item<float>() + 0.5f, quant_min), quant_max));
    fake_quant_grad_learnable_scale_channel_stub(
      scale.device().type(), dScale_item_vec, X_i, dY_i, scale_i, zero_point_i, quant_min, quant_max);
    fake_quant_grad_learnable_zero_point_channel_stub(
      zero_point.device().type(), dZeroPoint_item_vec, X_i, dY_i, scale_i, zero_point_i, quant_min, quant_max);
    float scale_item = dScale_item_vec.sum().unsqueeze(0).item<float>();
    float zero_point_item = dZeroPoint_item_vec.sum().unsqueeze(0).item<float>();

    dScale[i] = scale_item;
    dZeroPoint[i] = zero_point_item;
  }
  return std::make_tuple(dScale, dZeroPoint);
}

Tensor _fake_quantize_learnable_per_channel_affine(
    const Tensor& self,
    const Tensor& scale,
    const Tensor& zero_point,
    int64_t axis,
    int64_t quant_min,
    int64_t quant_max) {
  Tensor zero_point_rounded = zero_point.to(at::kLong);
  return native::fake_quantize_per_channel_affine(
    self, scale, zero_point_rounded, axis, quant_min, quant_max);
}

std::tuple<Tensor, Tensor, Tensor> _fake_quantize_learnable_per_channel_affine_backward(
    const Tensor& dY,
    const Tensor& X,
    const Tensor& scale,
    const Tensor& zero_point,
    int64_t axis,
    int64_t quant_min,
    int64_t quant_max) {
  /* The gradients for scale and zero point are calculated as below:
     Let Xfq be the fake quantized version of X.
     Let Xq be the quantized version of X (clamped at qmin and qmax).
     Let Delta and z be the scale and the zero point.
     :math:
      \frac{d\Delta }{dx} =
        \begin{cases}
          q_{\min} - z& \text{ if } X_q= q_{\min} \\
          q_{\max} - z& \text{ if } X_q= q_{\max} \\
          (X_{fq} - X) / \Delta & \text{ else }
        \end{cases}

      \frac{dz }{dx} =
        \begin{cases}
          -\Delta& \text{ if } X_q= q_{\min} \text{ or } X_q = q_{\max} \\
          0 & \text{ else }
        \end{cases}
  */
  TORCH_CHECK(dY.scalar_type() == ScalarType::Float);
  TORCH_CHECK(X.scalar_type() == ScalarType::Float);
  TORCH_CHECK(scale.scalar_type() == ScalarType::Float);
  TORCH_CHECK(zero_point.scalar_type() == ScalarType::Float);

  TORCH_CHECK(X.sizes() == dY.sizes(), "`X` and `dY` are not the same size");
  TORCH_CHECK(
      quant_min <= 0 && quant_max >= 0,
      "Expecting `quant_min` <= 0 and `quant_max` >= 0");
  TORCH_CHECK(scale.dim() == 1, "scale should be a 1-D tensor");
  TORCH_CHECK(zero_point.dim() == 1, "zero point should be a 1-D tensor");
  TORCH_CHECK(
      scale.numel() == zero_point.numel(),
      "scale and zero-point need to have the same dimensions");
  TORCH_CHECK(
      scale.numel() == X.size(axis),
      "dimensions of scale and zero-point are not consistent with input tensor")

  TORCH_CHECK(
      at::min(zero_point).item().toLong() >= quant_min &&
          at::max(zero_point).item().toLong() <= quant_max,
      "`zero_point` must be between `quant_min` and `quant_max`.");

  TORCH_CHECK(
      axis >= 0 && axis < X.dim(),
      "`axis` must be between 0 and number of dimensions of input");

  if (X.numel() <= 0) {
    return std::make_tuple(X, scale, zero_point);
  }

  auto zero_point_rounded = _get_rounded_zero_point(zero_point, quant_min, quant_max);
  auto dX = at::empty_like(X, X.options(), MemoryFormat::Preserve);

  std::vector<int64_t> expected_shape_X(X.dim(), 1);
  expected_shape_X[axis] = X.size(axis);

  TensorIterator iter_X = native::_build_iterator(
    dX, X, dY, scale, zero_point_rounded, expected_shape_X);

  fake_quant_grad_per_channel_stub(iter_X.device_type(), iter_X, quant_min, quant_max);

  std::tuple<Tensor, Tensor> dScaleZeroPoints = native::_get_scale_zero_point_per_channel_iter_grads(
    dY, X, scale, zero_point, axis, quant_min, quant_max);

  Tensor dScale = std::get<0>(dScaleZeroPoints).to(scale.device());
  Tensor dZeroPoint = std::get<1>(dScaleZeroPoints).to(zero_point.device());

  return std::make_tuple(dX, dScale, dZeroPoint);
}
} // namespace native
} // namespace at
