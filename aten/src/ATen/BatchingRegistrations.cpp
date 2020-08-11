#include <torch/library.h>
#include <ATen/VmapTransforms.h>
#include <ATen/BatchedFallback.h>
#include <ATen/ATen.h>

namespace at {

// NOTE: [What is a batching rule?]
//
// A *batching rule* implements the logic of how to call an operator on inputs
// that have zero or more additional batch dimensions. When one does a vmap, the
// dimension(s) being vmap'ed over get recorded as batch dimensions.
//
// For example, vmap(torch.add)(x, y)
// 1. wraps `x` into batched_x = BatchedTensor(x, bdims=[(lvl=1, dim=0)];
// 2. wraps `y` into batched_y = BatchedTensor(y, bdims=[(lvl=1, dim=0)];
// 3. and then runs `torch.add(batched_x, batched_y)`.

// NOTE: [When should I add a batching rule?]
// When you are adding a new operator, you'll need to add a batching rule so
// that vmap can work efficiently with said operator. If you do not, we'll attempt
// to generate a slow fallback for the batching rule (this is not yet implemented).

// NOTE: [How to write batching rules?]
// The signature of a batching rule should look like exactly like the C++ signature
// of its operator.
//
// First, see NOTE: [Logical vs physical args] in VmapTransforms.h for terminology.
//
// At a high level, what a batching rule does is the following:
// 1. Converts (logical) BatchedTensors to views on physical tensors.
// 2. Converts logical arguments (e.g. dimension indexes, shapes) to physical
//    arguments that correspond to the physical tensors.
// 3. Calls at:: operations on the physical tensors and arguments to produce
//    some physical results.
// 4. Converts physical results back to BatchedTensors.
//
// Steps 1, 2, and 4 differ for operators with different batching behaviors. When
// writing a new batching rule, please select a VmapTransform that matches the
// batching behavior of your operation. The VmapTransform provides helper functions
// to do steps (1), (2), and (4).
// (see NOTE: [What is an VmapTransform?] in VmapTransforms.h)

// Note: [Future plans]
// The API for writing a batching rule isn't stable. In the future, we'd like
// to think about the problem of translating these batching rules to TorchScript.
// Ideally batching rules in eager mode vs TorchScript would look pretty similar,
// if not use the same mechanism. In order to accomplish that we might have to
// do some refactoring.

Tensor sum_batching_rule(const Tensor& self, IntArrayRef dims, bool keepdim, optional<ScalarType> dtype) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto dims_physical = self_physical.getPhysicalDims(dims);
  auto result = at::sum(self_physical.tensor(), dims_physical, keepdim, dtype);
  return self_physical.newLogicalFromPhysical(result);
}

Tensor mul_batching_rule(const Tensor& self, const Tensor& other) {
  auto physical_args = BroadcastingVmapTransform::logicalToPhysical({self, other});
  auto result = at::mul(physical_args[0].tensor(), physical_args[1].tensor());
  return physical_args[0].newLogicalFromPhysical(result);
}

Tensor expand_batching_rule(const Tensor& self, IntArrayRef size, bool implicit) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto size_physical = self_physical.getPhysicalShape(size);
  auto self_physical_dim = self_physical.tensor().dim();

  TORCH_CHECK(self_physical_dim <= size_physical.size(),
       "expand: the number of sizes provided (", /*logical*/size.size(), ") ",
       "must be greater or equal to the number of dimensions in the tensor (",
       /*logical dim*/self.dim(), ")");

  if (self_physical_dim == size_physical.size()) {
    auto result = self_physical.tensor().expand(size_physical, implicit);
    return self_physical.newLogicalFromPhysical(result);
  }

  TORCH_INTERNAL_ASSERT(self_physical_dim < size_physical.size());
  // Here, we know we are expanding a (logical) tensor to a larger number
  // of dimensions. We have to be careful because we can't call expand directly
  // due to the presence of batch dimensions.
  //
  // As an example, let B0 be a batch dimension and consider expand(Tensor[B0, 3], [2, 3]).
  // The result should be a tensor of size [B0, 2, 3].
  // A physical view of size [B0, 3] can't directly be expanded to size [B0, 2, 3]
  // so the strategy here is to view it first as a tensor of size [B0, 1, 3] and
  // then expand.
  auto self_physical_size = self_physical.tensor().sizes();
  auto extra_dims = size_physical.size() - self_physical_dim;
  VmapDimVector view_shape(size_physical.size(), 1);
  std::copy(self_physical_size.begin(),
            self_physical_size.begin() + self_physical.numBatchDims(),
            view_shape.begin());
  std::copy(self_physical_size.begin() + self_physical.numBatchDims(),
            self_physical_size.end(),
            view_shape.begin() + self_physical.numBatchDims() + extra_dims);
  auto result = self_physical.tensor().view(view_shape).expand(size_physical, implicit);
  return self_physical.newLogicalFromPhysical(result);
}

std::vector<Tensor> chunk_batching_rule(const Tensor& self, int64_t chunks, int64_t dim) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto dim_physical = self_physical.getPhysicalDim(dim);
  auto result = at::chunk(self_physical.tensor(), chunks, dim_physical);
  self_physical.makeLogicalFromPhysicalListInplace(result);
  return result;
}

Tensor unsqueeze_batching_rule(const Tensor& self, int64_t dim) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  // NB: unsqueeze has some special handling of its `dim` argument so we can't call
  // self_physical.getPhysicalDim directly. In particular, native::unsqueeze
  // wraps the dim to (the logical dimension) + 1, so we need to do that here too.
  // https://github.com/pytorch/pytorch/blob/b623bdeabb0aa8da44285d303246e7f8ac06c2a9/aten/src/ATen/native/TensorShape.cpp#L1413
  auto dim_physical =
      self_physical.numBatchDims() + maybe_wrap_dim(dim, /*logical_dim*/self.dim() + 1);
  auto result = self_physical.tensor().unsqueeze(dim_physical);
  return self_physical.newLogicalFromPhysical(result);
}

Tensor squeeze_dim_batching_rule(const Tensor& self, int64_t dim) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto dim_physical = self_physical.getPhysicalDim(dim);
  auto result = self_physical.tensor().squeeze(dim_physical);
  return self_physical.newLogicalFromPhysical(result);
}

Tensor transpose_int_batching_rule(const Tensor& self, int64_t dim0, int64_t dim1) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto dim0_physical = self_physical.getPhysicalDim(dim0);
  auto dim1_physical = self_physical.getPhysicalDim(dim1);
  auto result = self_physical.tensor().transpose(dim0_physical, dim1_physical);
  return self_physical.newLogicalFromPhysical(result);
}

Tensor permute_batching_rule(const Tensor& self, IntArrayRef dims) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto dims_physical = self_physical.getPhysicalDims(dims);

  VmapDimVector all_dims_physical;
  all_dims_physical.reserve(self_physical.tensor().dim());
  for (int64_t bdim = 0; bdim < self_physical.numBatchDims(); bdim++) {
    all_dims_physical.push_back(bdim); 
  }
  all_dims_physical.insert(
      all_dims_physical.end(),
      dims_physical.begin(),
      dims_physical.end());
  auto result = self_physical.tensor().permute(all_dims_physical);
  return self_physical.newLogicalFromPhysical(result);
}

Tensor select_batching_rule(const Tensor& self, int64_t dim, int64_t index) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto dim_physical = self_physical.getPhysicalDim(dim);
  auto result = self_physical.tensor().select(dim_physical, index);
  return self_physical.newLogicalFromPhysical(result);
}

Tensor slice_batching_rule(const Tensor& self, int64_t dim, int64_t start, int64_t end, int64_t step) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto dim_physical = self_physical.getPhysicalDim(dim);
  auto result = self_physical.tensor().slice(dim_physical, start, end, step);
  return self_physical.newLogicalFromPhysical(result);
}

Tensor diagonal_batching_rule(const Tensor& self, int64_t offset, int64_t dim1, int64_t dim2) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto dim1_physical = self_physical.getPhysicalDim(dim1);
  auto dim2_physical = self_physical.getPhysicalDim(dim2);
  auto result = at::diagonal(self_physical.tensor(), offset, dim1_physical, dim2_physical);
  return self_physical.newLogicalFromPhysical(result);
}

Tensor movedim_batching_rule(const Tensor& self, IntArrayRef source, IntArrayRef destination) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto source_physical = self_physical.getPhysicalDims(source);
  auto destination_physical = self_physical.getPhysicalDims(destination);
  auto result = at::movedim(self_physical.tensor(), source_physical, destination_physical);
  return self_physical.newLogicalFromPhysical(result);
}

Tensor reshape_batching_rule(const Tensor& self, IntArrayRef shape) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto shape_physical = self_physical.getPhysicalShape(shape);
  auto result = self_physical.tensor().reshape(shape_physical);
  return self_physical.newLogicalFromPhysical(result);
}

std::vector<Tensor> split_batching_rule(const Tensor& self, int64_t split_size, int64_t dim) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto dim_physical = self_physical.getPhysicalDim(dim);
  auto result = at::split(self_physical.tensor(), split_size, dim_physical);
  self_physical.makeLogicalFromPhysicalListInplace(result);
  return result;
}

std::vector<Tensor> split_with_sizes_batching_rule(const Tensor& self, IntArrayRef split_sizes, int64_t dim) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto dim_physical = self_physical.getPhysicalDim(dim);
  auto result = at::split_with_sizes(self_physical.tensor(), split_sizes, dim_physical);
  self_physical.makeLogicalFromPhysicalListInplace(result);
  return result;
}

std::vector<Tensor> unbind_batching_rule(const Tensor& self, int64_t dim) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto dim_physical = self_physical.getPhysicalDim(dim);
  auto result = at::unbind(self_physical.tensor(), dim_physical);
  self_physical.makeLogicalFromPhysicalListInplace(result);
  return result;
}

Tensor unfold_batching_rule(const Tensor& self, int64_t dim, int64_t size, int64_t step) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto dim_physical = self_physical.getPhysicalDim(dim);
  auto result = self_physical.tensor().unfold(dim_physical, size, step);
  return self_physical.newLogicalFromPhysical(result);
}

Tensor view_batching_rule(const Tensor& self, IntArrayRef size) {
  auto self_physical = MultiBatchVmapTransform::logicalToPhysical(self);
  auto size_physical = self_physical.getPhysicalShape(size);
  auto result = self_physical.tensor().view(size_physical);
  return self_physical.newLogicalFromPhysical(result);
}

TORCH_LIBRARY_IMPL(_, Batched, m) {
  m.fallback(torch::CppFunction::makeFromBoxedFunction<&batchedTensorForLoopFallback>());
}

TORCH_LIBRARY_IMPL(aten, Batched, m) {
  // NB: Ideally we would like some operators, like size.int, to "fallthrough"
  // to the underlying implementation. However, because a BatchedTensor is a
  // Tensor wrapper, it only has one dispatch key (Batched) on it. The resolution
  // here is to just directly call the underlying implementation.
  m.impl("size.int", static_cast<int64_t (*)(const Tensor&, int64_t)>(native::size));
  m.impl("_add_batch_dim", native::_add_batch_dim);
  m.impl("_remove_batch_dim", native::_remove_batch_dim);

  m.impl_UNBOXED("sum.dim_IntList", sum_batching_rule);
  m.impl_UNBOXED("mul.Tensor", mul_batching_rule);

  // view operations
  m.impl("chunk", chunk_batching_rule);
  m.impl("diagonal", diagonal_batching_rule);
  m.impl("expand", expand_batching_rule);
  m.impl("expand_as", native::expand_as); // composite wrt autograd
  m.impl("movedim.intlist", movedim_batching_rule);
  m.impl("movedim.int", static_cast<Tensor(*)(const Tensor&,int64_t,int64_t)>(native::movedim)); // composite wrt autograd
  // NB: static_cast because there's another variant of narrow. However, we don't
  // want to support the other variant yet bc it isn't documented...
  m.impl("narrow", static_cast<Tensor(*)(const Tensor&,int64_t,int64_t,int64_t)>(native::narrow)); // composite wrt autograd
  m.impl("numpy_T", native::numpy_T); // composite wrt autograd
  m.impl("permute", permute_batching_rule);
  m.impl("reshape", reshape_batching_rule);
  m.impl("reshape_as", native::reshape_as); // composite wrt autograd
  m.impl("select.int", select_batching_rule);
  m.impl("slice.Tensor", slice_batching_rule);
  m.impl("split.Tensor", split_batching_rule);
  m.impl("split_with_sizes", split_with_sizes_batching_rule);
  m.impl("squeeze.dim", squeeze_dim_batching_rule);
  m.impl("t", native::t); // composite wrt autograd
  m.impl("transpose.int", transpose_int_batching_rule);
  m.impl("unbind.int", unbind_batching_rule);
  m.impl("unfold", unfold_batching_rule);
  m.impl("unsqueeze", unsqueeze_batching_rule);
  m.impl("view", view_batching_rule);
  m.impl("view_as", native::view_as); // composite wrt autograd
}

} // namespace at
