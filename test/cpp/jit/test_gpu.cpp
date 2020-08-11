#if defined(USE_CUDA)
#include <test/cpp/jit/test_base.h>

#include <torch/csrc/jit/codegen/cuda/arith.h>
#include <torch/csrc/jit/codegen/cuda/expr_evaluator.h>
#include <torch/csrc/jit/codegen/cuda/fusion.h>
#include <torch/csrc/jit/codegen/cuda/ir_all_nodes.h>
#include <torch/csrc/jit/codegen/cuda/ir_graphviz.h>
#include <torch/csrc/jit/codegen/cuda/ir_iostream.h>
#include <torch/csrc/jit/codegen/cuda/kernel.h>
#include <torch/csrc/jit/codegen/cuda/lower2device.h>
#include <torch/csrc/jit/codegen/cuda/mutator.h>
#include <torch/csrc/jit/codegen/cuda/transform_replay.h>
#include <torch/csrc/jit/codegen/cuda/transform_rfactor.h>

// fuser and IR parser
#include <torch/csrc/jit/codegen/cuda/parser.h>
#include "torch/csrc/jit/ir/irparser.h"

#include <iostream>

// Tests go in torch::jit
namespace torch {
namespace jit {

using namespace torch::jit::fuser;

static TensorView* makeDummyTensor(
    int nDims,
    DataType dtype = DataType::Float) {
  std::vector<IterDomain*> dom;
  for (int i = 0; i < nDims; i++)
    dom.push_back(new IterDomain(new Int(0), new Int()));

  return new TensorView(new TensorDomain(dom), dtype);
}

static void checkIntValue(
    const EvaluationContext* eval_context,
    Val* val,
    Int::ScalarType expected_value) {
  TORCH_CHECK(val->isAnInt());
  const auto actual_value = ExpressionEvaluator::evaluate(val, eval_context);
  TORCH_CHECK(actual_value.has_value());
  TORCH_CHECK(actual_value.value() == expected_value);
}

// 1. Test cases are void() functions.
// 2. They start with the prefix `test`

// A few smoke tests for IrGraphGenerator
// (These tests exercise IrGraphGenerator through a non-trivial IR,
//  to make sure that it runs w/o crashing. The actual output is not
//  validated)
void testGPU_IrGraphGenerator() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // Make sure we can handle empty IRs
  TORCH_CHECK(!IrGraphGenerator::toGraphviz(
                   &fusion, IrGraphGenerator::DetailLevel::Basic)
                   .empty());

  // Construct an interesting IR
  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);

  TensorView* tv2 = add(tv0, new Float(3.141));
  TensorView* tv3 = broadcast(tv0, {false, true, false, true});
  TensorView* tv4 = reductionOp(BinaryOpType::Add, {2}, new Float(0), tv3);
  TensorView* tv5 = clamp(tv4, new Float(0.f), new Float(1.f));
  TensorView* tv6 = add(tv2, tv2);

  // Another checkpoint before adding outputs
  TORCH_CHECK(!IrGraphGenerator::toGraphviz(
                   &fusion, IrGraphGenerator::DetailLevel::Explicit)
                   .empty());

  fusion.addOutput(tv6);

  tv6->merge(0);
  tv6->split(0, 4);
  tv6->axis(0)->parallelize(ParallelType::BIDx);
  tv5->reorder({{-1, 0}});
  tv2->computeAt(tv6, 1);

  // Another checkpoint with more node types
  TORCH_CHECK(!IrGraphGenerator::toGraphviz(
                   &fusion, IrGraphGenerator::DetailLevel::ComputeOnly)
                   .empty());

  for (Val* val : fusion.vals()) {
    if (!fusion.hasInput(val) &&
        val->getValType().value() == ValType::TensorView) {
      TensorView* tv = static_cast<TensorView*>(val);
      tv->axis(-1)->parallelize(ParallelType::TIDx);
    }
  }

  // Final IR graph
  TORCH_CHECK(!IrGraphGenerator::toGraphviz(
                   &fusion, IrGraphGenerator::DetailLevel::Verbose)
                   .empty());
}

void testGPU_FusionDispatch() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* f = new Float{2.f};
  std::stringstream ss1, ss2, ss3;
  ss1 << f;
  ss2 << static_cast<Val*>(f);
  ss3 << static_cast<Statement*>(f);
  TORCH_CHECK(
      ss1.str().compare(ss2.str()) == 0 && ss1.str().compare(ss3.str()) == 0,
      "Error with dispatch system where results differ by passing Float* vs Val* vs Statement*.");
}

// Evaluate basic scalar operations with constant values
void testGPU_FusionExprEvalConstants() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  EvaluationContext eval_context(&fusion);

  auto* a = new Int(7);
  auto* b = new Int(3);

  checkIntValue(&eval_context, neg(a), -7);
  checkIntValue(&eval_context, add(a, b), 10);
  checkIntValue(&eval_context, neg(mul(sub(a, b), div(a, b))), -8);
  checkIntValue(&eval_context, mod(a, b), 1);
  checkIntValue(&eval_context, ceilDiv(a, b), 3);
}

// Evaluate basic scalar operations with bound values
void testGPU_FusionExprEvalBindings() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  EvaluationContext eval_context(&fusion);

  auto* a = new Int();
  auto* b = new Int();
  auto* c = add(a, b);
  auto* d = neg(ceilDiv(c, b));
  auto* e = new Int(0);

  // trying to evaluate before binding should give empty results
  TORCH_CHECK(!ExpressionEvaluator::evaluate(a, &eval_context).has_value());
  TORCH_CHECK(!ExpressionEvaluator::evaluate(d, &eval_context).has_value());

  eval_context.bind(a, 7);
  eval_context.bind(b, 3);

  // can't bind to the results of expressions
  ASSERT_ANY_THROW(eval_context.bind(c, 100));

  // can't bind to concrete values
  ASSERT_ANY_THROW(eval_context.bind(e, 100));

  checkIntValue(&eval_context, c, 10);
  checkIntValue(&eval_context, sub(a, b), 4);
  checkIntValue(&eval_context, mod(a, b), 1);
  checkIntValue(&eval_context, ceilDiv(a, b), 3);
  checkIntValue(&eval_context, d, -4);

  eval_context.bind(a, 2);
  eval_context.bind(b, 5);

  checkIntValue(&eval_context, c, 7);
  checkIntValue(&eval_context, sub(a, b), -3);
  checkIntValue(&eval_context, mod(a, b), 2);
  checkIntValue(&eval_context, ceilDiv(a, b), 1);
  checkIntValue(&eval_context, d, -2);
}

// Evaluate expressions in a simple IR
void testGPU_FusionExprEvalBasic() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // Create a non-trivial IR
  TensorView* tv0 = makeDummyTensor(2);
  TensorView* tv1 = makeDummyTensor(2);

  fusion.addInput(tv0);
  fusion.addInput(tv1);

  TensorView* tv2 = add(tv1, new Float(2.0));
  TensorView* tv3 = add(tv0, tv2);

  fusion.addOutput(tv3);

  tv3->split(0, 4);

  tv0->computeAt(tv3, 1);
  tv1->computeAt(tv3, 1);

  tv3->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::Unroll);
  tv3->axis(1)->parallelize(ParallelType::Unroll);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  // 1. Create an evaluation context
  EvaluationContext eval_context(&fusion);

  // 2. Bind values
  //
  // IMPORTANT:
  // a. The bindings are only as stable as the Vals are in the fusion graph
  // b. You must use the original (rootDomain) extents
  //  (ex. `tv0->getRootDomain()[0]->extent()`
  //   instead of `tv0->axis(0)->extent()`)
  //
  eval_context.bind(tv0->getRootDomain()[0]->extent(), 6);
  eval_context.bind(tv0->getRootDomain()[1]->extent(), 128);
  eval_context.bind(tv1->getRootDomain()[0]->extent(), 6);
  eval_context.bind(tv1->getRootDomain()[1]->extent(), 128);

  // 3. Evaluate and check result values
  TORCH_CHECK(tv2->domain()->nDims() == 3);
  checkIntValue(&eval_context, tv2->axis(0)->rawExtent(), 2);
  checkIntValue(&eval_context, tv2->axis(1)->rawExtent(), 4);
  checkIntValue(&eval_context, tv2->axis(2)->rawExtent(), 128);

  TORCH_CHECK(tv3->domain()->nDims() == 3);
  checkIntValue(&eval_context, tv3->axis(0)->rawExtent(), 2);
  checkIntValue(&eval_context, tv3->axis(1)->rawExtent(), 4);
  checkIntValue(&eval_context, tv3->axis(2)->rawExtent(), 128);
}

// Evaluate expressions in a more complex IR
void testGPU_FusionExprEvalComplex() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);

  TensorView* tv1 = mul(tv0, new Float(-1.0));
  TensorView* tv2 = add(tv0, new Float(3.0));
  TensorView* tv3 = mul(tv0, new Float(2.0));
  TensorView* tv4 = add(tv2, tv1);
  TensorView* tv5 = add(tv4, tv3);
  TensorView* tv6 = add(tv0, tv3);

  fusion.addOutput(tv5);
  fusion.addOutput(tv6);

  tv5->reorder({{-1, 0}});

  tv6->split(0, 5);
  tv5->merge(0);

  // 1. Create an evaluation context
  EvaluationContext eval_context(&fusion);

  // 2. Bind values
  eval_context.bind(tv0->getRootDomain()[0]->extent(), 129);
  eval_context.bind(tv0->getRootDomain()[1]->extent(), 127);

  // Evaluate and check extent values
  TORCH_CHECK(tv0->domain()->nDims() == 2);
  checkIntValue(&eval_context, tv0->axis(0)->rawExtent(), 129);
  checkIntValue(&eval_context, tv0->axis(1)->rawExtent(), 127);

  TORCH_CHECK(tv3->domain()->nDims() == 2);
  checkIntValue(&eval_context, tv3->axis(0)->rawExtent(), 129);
  checkIntValue(&eval_context, tv3->axis(1)->rawExtent(), 127);

  TORCH_CHECK(tv4->domain()->nDims() == 2);
  checkIntValue(&eval_context, tv4->axis(0)->rawExtent(), 129);
  checkIntValue(&eval_context, tv4->axis(1)->rawExtent(), 127);

  TORCH_CHECK(tv5->domain()->nDims() == 1);
  checkIntValue(&eval_context, tv5->axis(0)->rawExtent(), 16383);

  TORCH_CHECK(tv6->domain()->nDims() == 3);
  checkIntValue(&eval_context, tv6->axis(0)->rawExtent(), 26);
  checkIntValue(&eval_context, tv6->axis(1)->rawExtent(), 5);
  checkIntValue(&eval_context, tv6->axis(2)->rawExtent(), 127);
}

// Evaluate expressions post lowering
void testGPU_FusionExprEvalPostLower() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // Create a non-trivial IR
  TensorView* tv0 = makeDummyTensor(2);
  TensorView* tv1 = makeDummyTensor(2);

  fusion.addInput(tv0);
  fusion.addInput(tv1);

  TensorView* tv2 = add(tv1, new Float(2.0));
  TensorView* tv3 = add(tv0, tv2);

  fusion.addOutput(tv3);

  tv3->split(0, 4);

  tv0->computeAt(tv3, 1);
  tv1->computeAt(tv3, 1);

  tv3->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::Unroll);
  tv3->axis(1)->parallelize(ParallelType::Unroll);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  auto* bid_x = add(tv3->axis(0)->rawExtent(), new Int(0));
  auto* tid_x = add(tv3->axis(-1)->rawExtent(), new Int(0));

  // Lower
  GPULower gpulw(&fusion);
  std::stringstream kernel;
  gpulw.printKernel(kernel);

  // 1. Create an evaluation context
  EvaluationContext eval_context(&fusion);

  // 2. Bind values
  eval_context.bind(tv0->getRootDomain()[0]->extent(), 6);
  eval_context.bind(tv0->getRootDomain()[1]->extent(), 128);
  eval_context.bind(tv1->getRootDomain()[0]->extent(), 6);
  eval_context.bind(tv1->getRootDomain()[1]->extent(), 128);

  // 3. Evaluate and check result values
  TORCH_CHECK(tv2->domain()->nDims() == 3);
  checkIntValue(&eval_context, tv2->axis(0)->rawExtent(), 2);
  checkIntValue(&eval_context, tv2->axis(1)->rawExtent(), 4);
  checkIntValue(&eval_context, tv2->axis(2)->rawExtent(), 128);

  TORCH_CHECK(tv3->domain()->nDims() == 3);
  checkIntValue(&eval_context, tv3->axis(0)->rawExtent(), 2);
  checkIntValue(&eval_context, tv3->axis(1)->rawExtent(), 4);
  checkIntValue(&eval_context, tv3->axis(2)->rawExtent(), 128);

  checkIntValue(&eval_context, bid_x, 2);
  checkIntValue(&eval_context, tid_x, 128);
}

void testGPU_FusionClear() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // 1. Create a dummy IR

  {
    TensorView* tv0 = makeDummyTensor(2);
    TensorView* tv1 = makeDummyTensor(2);

    fusion.addInput(tv0);
    fusion.addInput(tv1);

    TensorView* tv2 = add(tv1, new Float(2.0));
    TensorView* tv3 = add(tv0, tv2);

    fusion.addOutput(tv3);

    tv3->split(0, 4);
    tv0->computeAt(tv3, 1);
    tv1->computeAt(tv3, 1);

    tv3->axis(0)->parallelize(ParallelType::BIDx);
    tv2->axis(1)->parallelize(ParallelType::Unroll);
    tv3->axis(-1)->parallelize(ParallelType::TIDx);
  }

  // 2. Clear the IR

  fusion.clear();

  TORCH_CHECK(fusion.exprs().empty());
  TORCH_CHECK(fusion.vals().empty());

  TORCH_CHECK(fusion.inputs().empty());
  TORCH_CHECK(fusion.outputs().empty());

  TORCH_CHECK(!fusion.hasReduction());
  TORCH_CHECK(!fusion.hasBlockReduction());
  TORCH_CHECK(!fusion.hasGridReduction());

  // 3. Rebuild the IR

  {
    TensorView* tv0 = makeDummyTensor(3);
    TensorView* tv1 = makeDummyTensor(3);
    TensorView* tv2 = add(tv1, new Float(2.0));
    TensorView* tv3 = add(tv0, tv2);

    fusion.addInput(tv0);
    fusion.addInput(tv1);
    fusion.addOutput(tv3);

    tv3->reorder({{0, 2}, {2, 0}});
    tv3->split(-1, 4);
    tv3->reorder({{2, 0}, {3, 1}, {0, 3}});
    tv0->computeAt(tv3, -1);
    tv1->computeAt(tv3, -1);
  }

  prog.device_ = 0;
  prog.grid(4);
  prog.block(8);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  at::Tensor input1 = at::randn({16, 8, 8}, options);
  at::Tensor input2 = at::randn_like(input1);
  at::Tensor output = at::empty_like(input1);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input1, input2}, {output});

  at::Tensor tv2_ref = input2 + 2.0;
  at::Tensor output_ref = input1 + tv2_ref;

  TORCH_CHECK(output_ref.equal(output));
}

void testGPU_FusionCopy() {
  Fusion original_fusion;

  // Create the test IR
  {
    FusionGuard fg(&original_fusion);

    auto tv0 = makeDummyTensor(3);
    auto tv1 = makeDummyTensor(3);
    auto tv2 = add(tv1, new Float(2.0));
    auto tv3 = sub(add(tv0, mul(tv2, tv2)), tv2);

    original_fusion.addInput(tv0);
    original_fusion.addInput(tv1);
    original_fusion.addOutput(tv3);

    tv3->reorder({{0, 2}, {2, 0}});
    tv3->split(-1, 4);
    tv3->reorder({{2, 0}, {3, 1}, {0, 3}});

    tv0->computeAt(tv3, -1);
    tv1->computeAt(tv3, -1);

    tv3->axis(0)->parallelize(ParallelType::BIDx);
    tv3->axis(-1)->parallelize(ParallelType::TIDx);
  }

  // Test copy before lowering
  Fusion clone = original_fusion;

  // Compare IR dumps
  std::stringstream original_ir;
  std::stringstream clone_ir;
  original_ir << original_fusion;
  clone_ir << clone;
  ASSERT_EQ(original_ir.str(), clone_ir.str());

  // Lower original fusion
  std::stringstream original_kernel;
  {
    GPULower lower(&original_fusion);
    lower.printKernel(original_kernel);
  }

  // Make sure the "before lowering" clone was not mutated
  // while lowering the original fusion IR
  std::stringstream before_lowering_ir;
  before_lowering_ir << clone;
  ASSERT_EQ(original_ir.str(), before_lowering_ir.str());

  // Test copy after lowering (including assignment operator)
  Fusion before_lowering = clone;
  clone = original_fusion;

  // Compare IR dumps
  std::stringstream original_lowered_ir;
  std::stringstream clone_lowered_ir;
  original_lowered_ir << original_fusion;
  clone_lowered_ir << clone;
  ASSERT_EQ(original_lowered_ir.str(), clone_lowered_ir.str());

  // Lower the "before lowering" and compare kernels
  std::stringstream clone_kernel;
  {
    GPULower lower(&before_lowering);
    lower.printKernel(clone_kernel);
  }
  ASSERT_EQ(original_kernel.str(), clone_kernel.str());
}

void testGPU_FusionMove() {
  Fusion fusion;

  // Create the test IR
  {
    FusionGuard fg(&fusion);

    auto tv0 = makeDummyTensor(3);
    auto tv1 = makeDummyTensor(3);
    auto tv2 = add(tv1, new Float(2.0));
    auto tv3 = sub(add(tv0, mul(tv2, tv2)), tv2);

    fusion.addInput(tv0);
    fusion.addInput(tv1);
    fusion.addOutput(tv3);

    tv3->reorder({{0, 2}, {2, 0}});
    tv3->split(-1, 4);
    tv3->reorder({{2, 0}, {3, 1}, {0, 3}});

    tv0->computeAt(tv3, -1);
    tv1->computeAt(tv3, -1);

    tv3->axis(0)->parallelize(ParallelType::BIDx);
    tv3->axis(-1)->parallelize(ParallelType::TIDx);
  }

  std::stringstream original_ir;
  original_ir << fusion;

  // Test move before lowering
  Fusion another_fusion = std::move(fusion);

  // Check that the original fusion is "empty"
  //
  // IMPORTANT: these checks assume knowledge of the internal
  //    implementation of the move operations. General uses
  //    should only assume that the moved-from object is in
  //    a valid, but unspecified state. This is similar to the
  //    standard library containers:
  //    https://en.cppreference.com/w/cpp/utility/move
  //
  TORCH_CHECK(fusion.exprs().empty());
  TORCH_CHECK(fusion.vals().empty());
  TORCH_CHECK(fusion.inputs().empty());
  TORCH_CHECK(fusion.outputs().empty());

  // clear() has no pre-conditions so it's valid to call on a moved-from object
  fusion.clear();

  // Compare IR dumps
  std::stringstream another_ir;
  another_ir << another_fusion;
  ASSERT_EQ(original_ir.str(), another_ir.str());

  // Lower the fusion IR
  std::stringstream kernel;
  {
    GPULower lower(&another_fusion);
    lower.printKernel(kernel);
  }

  std::stringstream lowered_ir;
  lowered_ir << another_fusion;

  // Test move assignment after lowering
  fusion = std::move(another_fusion);

  // Compare IR dumps
  std::stringstream moved_lowered_ir;
  moved_lowered_ir << fusion;
  ASSERT_EQ(lowered_ir.str(), moved_lowered_ir.str());
}

void testGPU_FusionSimpleArith() {
  std::stringstream ss1, ss2;

  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* f1 = new Float(1.f);
  Float* f2 = new Float{2.f};
  Float* f3 = new Float();

  // Disrupt the fusion to make sure guard works well
  {
    Fusion fusion2;
    FusionGuard fg(&fusion2);

    Float* f1 = new Float(1.f);
    Float* f2 = new Float(2.f);
    add(f1, f2);
    ss2 << fusion2;
  }

  new BinaryOp(BinaryOpType::Add, f3, f1, f2);
  ss1 << fusion;

  TORCH_CHECK(
      ss1.str().compare(ss2.str()) == 0,
      "Error where explicit add nodes don't match implicit add nodes.");
}

void testGPU_FusionSimpleTypePromote() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* f4 = new Float{4.f};
  Int* i1 = new Int{3};
  auto f5 = add(f4, i1);

  TORCH_CHECK(f5->getDataType() == DataType::Float);
}

class ZeroMutator : public OptOutMutator {
 public:
  Statement* mutate(Float* f) {
    if (f->isConst() && *(f->value()) == 1.0)
      return new Float(0.0);
    return f;
  }
  void mutate(Fusion* f) {
    OptOutMutator::mutate(f);
  }
};

void testGPU_FusionMutator() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* f4 = new Float{1.f};
  Int* i1 = new Int{3};
  Val* f5 = add(f4, i1);
  ZeroMutator mutator;
  mutator.mutate(&fusion);
  Val* lhs = static_cast<BinaryOp*>(fusion.origin(f5))->lhs();
  TORCH_CHECK(
      lhs->getValType().value() == ValType::Scalar &&
      lhs->getDataType().value() == DataType::Float);
  Float* flhs = static_cast<Float*>(lhs);

  TORCH_CHECK(flhs->value().value() == 0.f);
}

void testGPU_FusionRegister() {
  Fusion fusion;
  FusionGuard fg(&fusion);
  Float* v1 = new Float{1.f};
  Float* v2 = new Float{2.f};
  Val* v3 = binaryOp(BinaryOpType::Add, v1, v2);
  Val* v4 = binaryOp(BinaryOpType::Add, v1, v2);
  TORCH_CHECK(v1->name() + 1 == v2->name());
  TORCH_CHECK(v2->name() + 1 == v3->name());
  TORCH_CHECK(v3->name() + 1 == v4->name());
  TORCH_CHECK(fusion.origin(v3)->name() + 1 == fusion.origin(v4)->name());
}

// dummy expr with 2 outputs only for toposort test.
struct DummyExpr : public Expr {
  ~DummyExpr() = default;
  DummyExpr(Val* _outlhs, Val* _outrhs, Val* _lhs, Val* _rhs)
      : Expr(ExprType::UnaryOp) // Not terribly safe...
  {
    addOutput(_outlhs);
    addOutput(_outrhs);
    addInput(_lhs);
    addInput(_rhs);
    this->name_ = FusionGuard::getCurFusion()->registerExpr(this);
  }
  DummyExpr(const DummyExpr& other) = delete;
  DummyExpr& operator=(const DummyExpr& other) = delete;
  DummyExpr(DummyExpr&& other) = delete;
  DummyExpr& operator=(DummyExpr&& other) = delete;
};

void testGPU_FusionTopoSort() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // e0: v3, v2 = dummy(v1, v0)
  // e1: v4     =   add(v3, v2)
  // e2: v5     =   add(v2, v4)
  // e3: v6     =   add(v5, v5)
  Float* v0 = new Float{1.f};
  Float* v1 = new Float{2.f};
  Float* v2 = new Float();
  Float* v3 = new Float();
  Float* v4 = new Float();
  Float* v5 = new Float();
  Float* v6 = new Float();

  Expr* e0 = new DummyExpr(v3, v2, v1, v0);
  Expr* e1 = new BinaryOp(BinaryOpType::Add, v4, v3, v2);
  Expr* e2 = new BinaryOp(BinaryOpType::Add, v5, v2, v4);
  Expr* e3 = new BinaryOp(BinaryOpType::Add, v6, v5, v5);

  std::vector<Expr*> exprs = fusion.exprs();

  TORCH_CHECK(exprs.size() == 4);
  TORCH_CHECK(exprs[0] == e0);
  TORCH_CHECK(exprs[1] == e1);
  TORCH_CHECK(exprs[2] == e2);
  TORCH_CHECK(exprs[3] == e3);

  fusion.addOutput(v2);
  exprs = fusion.exprs(true);
  TORCH_CHECK(exprs.size() == 1);
  TORCH_CHECK(exprs[0] == e0);

  fusion.addOutput(v5);
  exprs = fusion.exprs(true);
  TORCH_CHECK(exprs[0] == e0);
  TORCH_CHECK(exprs[1] == e1);
  TORCH_CHECK(exprs[2] == e2);

  fusion.addOutput(v4);
  exprs = fusion.exprs(true);
  TORCH_CHECK(exprs[0] == e0);
  TORCH_CHECK(exprs[1] == e1);
  TORCH_CHECK(exprs[2] == e2);

  fusion.addOutput(v3);
  exprs = fusion.exprs(true);
  TORCH_CHECK(exprs[0] == e0);
  TORCH_CHECK(exprs[1] == e1);
  TORCH_CHECK(exprs[2] == e2);

  fusion.addOutput(v6);
  exprs = fusion.exprs(true);
  TORCH_CHECK(exprs.size() == 4);
  TORCH_CHECK(exprs[0] == e0);
  TORCH_CHECK(exprs[1] == e1);
  TORCH_CHECK(exprs[2] == e2);
  TORCH_CHECK(exprs[3] == e3);

  TORCH_CHECK(fusion.origin(v2)->name() == 0);
  TORCH_CHECK(fusion.origin(v3)->name() == 0);
  TORCH_CHECK(fusion.origin(v4)->name() == 1);
  TORCH_CHECK(fusion.origin(v5)->name() == 2);
  TORCH_CHECK(fusion.origin(v6)->name() == 3);
}

void testGPU_FusionTensor() {
  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  auto tensor = at::randn({2, 3, 4, 5}, options);
  auto sizes = tensor.sizes().vec();
  auto tensor_type = TensorType::create(tensor);

  Fusion fusion;
  FusionGuard fg(&fusion);

  auto fuser_tensor = new TensorView(tensor_type);
  TORCH_CHECK(fuser_tensor->getDataType().value() == DataType::Float);
  TORCH_CHECK(fuser_tensor->domain() != nullptr);
}

void testGPU_FusionTVSplit() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* tv = makeDummyTensor(3);

  tv = tv->split(2, 2);
  TORCH_CHECK(tv->nDims() == 4);
  Expr* outer = tv->axis(2)->extent()->getOrigin();

  TORCH_CHECK(
      outer->getExprType().value() == ExprType::BinaryOp &&
      static_cast<BinaryOp*>(outer)->getBinaryOpType() ==
          BinaryOpType::CeilDiv &&
      static_cast<BinaryOp*>(outer)->lhs()->sameAs(
          tv->getRootDomain()[2]->extent()) &&
      static_cast<Int*>(static_cast<BinaryOp*>(outer)->rhs())
          ->sameAs(new Int(2)));

  IterDomain* inner = static_cast<IterDomain*>(tv->axis(3));
  TORCH_CHECK(
      inner->extent()->isScalar() &&
      static_cast<Int*>(inner->extent())->isConst() &&
      static_cast<Int*>(inner->extent())->value().value() == 2);
}

void testGPU_FusionTVMerge() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* tv = makeDummyTensor(3);

  tv = tv->merge(1);
  Expr* axisOp = tv->axis(1)->extent()->getOrigin();

  TORCH_CHECK(
      tv->nDims() == 2 && axisOp->getExprType() == ExprType::BinaryOp &&
      static_cast<BinaryOp*>(axisOp)->getBinaryOpType() == BinaryOpType::Mul &&
      static_cast<BinaryOp*>(axisOp)->lhs() ==
          tv->getRootDomain()[1]->extent() &&
      static_cast<BinaryOp*>(axisOp)->rhs() ==
          tv->getRootDomain()[2]->extent());
}

void testGPU_FusionTVReorder() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  std::unordered_map<int, int> shift_right{{-1, 0}};

  std::unordered_map<int, int> shift_left{{0, -1}};

  std::unordered_map<int, int> shift_left_2{{0, -1}, {1, 0}, {2, 1}};

  std::unordered_map<int, int> swap{{0, 2}, {2, 0}};

  auto tv = makeDummyTensor(3);
  std::vector<IterDomain*> ref;
  ref = std::vector<IterDomain*>(
      tv->domain()->domain().begin(), tv->domain()->domain().end());

  tv->reorder(shift_left);
  for (int i = 0; i < (int)tv->nDims(); i++)
    TORCH_CHECK(ref[i]->sameAs(tv->axis(i - 1)));

  tv = makeDummyTensor(3);
  ref = std::vector<IterDomain*>(
      tv->domain()->domain().begin(), tv->domain()->domain().end());

  tv->reorder(shift_left);
  for (int i = 0; i < (int)tv->nDims(); i++)
    TORCH_CHECK(ref[i]->sameAs(tv->axis(i - 1)));

  tv = makeDummyTensor(3);
  ref = std::vector<IterDomain*>(
      tv->domain()->domain().begin(), tv->domain()->domain().end());

  tv->reorder(shift_right);
  TORCH_CHECK(ref[ref.size() - 1]->sameAs(tv->axis(0)));
  for (int i = 1; i < (int)tv->nDims(); i++)
    TORCH_CHECK(ref[i - 1]->sameAs(tv->axis(i)));

  tv = makeDummyTensor(3);
  ref = std::vector<IterDomain*>(
      tv->domain()->domain().begin(), tv->domain()->domain().end());
  tv->reorder(swap);
  TORCH_CHECK(ref[0]->sameAs(tv->axis(2)));
  TORCH_CHECK(ref[2]->sameAs(tv->axis(0)));
  TORCH_CHECK(ref[1]->sameAs(tv->axis(1)));
}

void testGPU_FusionEquality() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* fval1 = new Float();
  Float* fval1_copy = fval1;
  Float* fval2 = new Float();
  Float* fone = new Float(1.0);

  TORCH_CHECK(fval1->sameAs(fval1_copy));
  TORCH_CHECK(!fval1->sameAs(fval2));
  TORCH_CHECK(!fone->sameAs(fval1));
  TORCH_CHECK(fone->sameAs(new Float(1.0)));

  Int* ival1 = new Int();
  Int* ival1_copy = ival1;
  Int* ival2 = new Int();
  Int* ione = new Int(1);

  TORCH_CHECK(ival1->sameAs(ival1_copy));
  TORCH_CHECK(!ival1->sameAs(ival2));
  TORCH_CHECK(!ione->sameAs(ival1));
  TORCH_CHECK(ione->sameAs(new Int(1)));

  BinaryOp* add1 = new BinaryOp(BinaryOpType::Add, new Float(), fval1, ival1);
  BinaryOp* add1_copy =
      new BinaryOp(BinaryOpType::Add, new Float(), fval1, ival1);
  BinaryOp* sub1 = new BinaryOp(BinaryOpType::Sub, new Float(), fval1, ival1);

  UnaryOp* neg1 = new UnaryOp(UnaryOpType::Neg, new Float(), fval1);
  UnaryOp* neg2 = new UnaryOp(UnaryOpType::Neg, new Float(), fval2);
  UnaryOp* neg1_copy = new UnaryOp(UnaryOpType::Neg, new Float(), fval1);

  TORCH_CHECK(add1->sameAs(add1_copy));
  TORCH_CHECK(!add1->sameAs(sub1));

  TORCH_CHECK(neg1->sameAs(neg1_copy));
  TORCH_CHECK(!static_cast<Expr*>(neg1)->sameAs(add1));
  TORCH_CHECK(!neg1->sameAs(neg2));
}

void testGPU_FusionReplaceAll() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* f0 = new Float();
  Float* f1 = new Float{1.f};
  Float* f2 = new Float{2.f};
  Float* f3 = new Float();
  Float* f4 = static_cast<Float*>(add(f1, f0));

  // replace the output f4 with f3
  ReplaceAll::instancesOf(f4, f3);
  // f3 should now have an origin function
  TORCH_CHECK(fusion.origin(f3) != nullptr);

  // Should have removed f4 completely so we shouldn't have any other expr than
  // f3 construction
  TORCH_CHECK(fusion.exprs().size() == 1);

  // Replace constant Float's of value 1.f with 2.f
  ReplaceAll::instancesOf(f1, f2);
  BinaryOp* bop = static_cast<BinaryOp*>(fusion.origin(f3));
  // make sure the binary op (origin of f3) actually changed to 2.f
  TORCH_CHECK(static_cast<Float*>(bop->lhs())->sameAs(new Float{2.f}));
}

void testGPU_FusionDependency() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* f0 = new Float(0.f);
  Float* f1 = new Float(1.f);
  auto f2 = add(f0, f1);

  auto f3 = add(f2, f2);

  Float* f4 = new Float(4.f);
  Float* f5 = new Float(5.f);
  auto f6 = add(f4, f5);

  Float* f7 = new Float(7.f);
  Float* f8 = new Float(8.f);
  auto f9 = add(f7, f8);

  auto f10 = add(f6, f9);

  auto f11 = add(f3, f10);

  TORCH_CHECK(DependencyCheck::isDependencyOf(f0, f11));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f1, f11));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f2, f11));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f3, f11));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f6, f11));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f9, f11));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f0, f2));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f2, f3));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f4, f6));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f8, f10));

  TORCH_CHECK(!DependencyCheck::isDependencyOf(f11, f0));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f11, f1));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f11, f2));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f11, f3));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f11, f4));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f11, f5));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f2, f0));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f3, f2));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f6, f4));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f10, f8));

  auto dep_chain = DependencyCheck::getSingleDependencyChain(f0, f11);
  TORCH_CHECK(dep_chain.back() == f11);
  dep_chain.pop_back();
  TORCH_CHECK(dep_chain.back() == f3);
  dep_chain.pop_back();
  TORCH_CHECK(dep_chain.back() == f2);
  dep_chain.pop_back();

  dep_chain = DependencyCheck::getSingleDependencyChain(f6, f11);
  TORCH_CHECK(dep_chain.back() == f11);
  dep_chain.pop_back();
  TORCH_CHECK(dep_chain.back() == f10);
  dep_chain.pop_back();

  dep_chain = DependencyCheck::getSingleDependencyChain(f4, f11);
  TORCH_CHECK(dep_chain.back() == f11);
  dep_chain.pop_back();
  TORCH_CHECK(dep_chain.back() == f10);
  dep_chain.pop_back();
  TORCH_CHECK(dep_chain.back() == f6);
  dep_chain.pop_back();

  dep_chain = DependencyCheck::getSingleDependencyChain(f11, f2);
  TORCH_CHECK(dep_chain.empty());
}

void testGPU_FusionParser() {
  auto g = std::make_shared<Graph>();
  const auto graph0_string = R"IR(
    graph(%0 : Float(2:1),
          %1 : Float(2:1)):
      %c0 : Float(2:1) = aten::mul(%0, %1)
      %d0 : Float(2:1) = aten::mul(%c0, %0)
      return (%d0))IR";
  torch::jit::parseIR(graph0_string, g.get());

  // strides are not yet supported in the irparser.
  for (auto val : g->block()->inputs()) {
    if (val->isCompleteTensor())
      val->setType(val->type()->cast<TensorType>()->contiguous());
  }
  for (auto node : g->block()->nodes()) {
    for (auto val : node->outputs()) {
      if (val->isCompleteTensor())
        val->setType(val->type()->cast<TensorType>()->contiguous());
    }
  }

  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);
  // These can be set to anything as there are no bindings!
  // All CTAS and threads execute the same thing.
  prog.grid(4);
  prog.block(32);
  prog.device_ = 0;
  fuser::cuda::parseJitIR(g, &prog);

  // CONSIDER:
  // 1. this can be moved to a dedicated "golden" file
  // 2. use a fuzzy compare (ignore non-significant whitespaces for example)
  const std::string expected_kernel = R"(
__global__ void CUDAGeneratedKernel(Tensor<float, 1> T0, Tensor<float, 1> T1, Tensor<float, 1> T3){
  float T2[4];
  if ( ( ( ( ( ( blockIdx.x * 4 ) + ( 4 - 1 ) ) * 128 ) + threadIdx.x ) < T3.size[0] ) ) { 
    for(size_t i40 = 0; i40 < 4; ++i40 ) {
      T2[ i40 ]
         = T0[ ( ( ( ( ( blockIdx.x * 4 ) + i40 ) * 128 ) + threadIdx.x ) * T0.stride[0] ) ]
         * T1[ ( ( ( ( ( blockIdx.x * 4 ) + i40 ) * 128 ) + threadIdx.x ) * T1.stride[0] ) ];
    }
  } else { 
    for(size_t i40 = 0; i40 < 4; ++i40 ) {
      if ( ( ( ( ( ( blockIdx.x * 4 ) + i40 ) * 128 ) + threadIdx.x ) < T3.size[0] ) ) { 
        T2[ i40 ]
           = T0[ ( ( ( ( ( blockIdx.x * 4 ) + i40 ) * 128 ) + threadIdx.x ) * T0.stride[0] ) ]
           * T1[ ( ( ( ( ( blockIdx.x * 4 ) + i40 ) * 128 ) + threadIdx.x ) * T1.stride[0] ) ];
      }
    }
  }
  if ( ( ( ( ( ( blockIdx.x * 4 ) + ( 4 - 1 ) ) * 128 ) + threadIdx.x ) < T3.size[0] ) ) { 
    for(size_t i41 = 0; i41 < 4; ++i41 ) {
      T3[ ( ( ( ( ( blockIdx.x * 4 ) + i41 ) * 128 ) + threadIdx.x ) * T3.stride[0] ) ]
         = T2[ i41 ]
         * T0[ ( ( ( ( ( blockIdx.x * 4 ) + i41 ) * 128 ) + threadIdx.x ) * T0.stride[0] ) ];
    }
  } else { 
    for(size_t i41 = 0; i41 < 4; ++i41 ) {
      if ( ( ( ( ( ( blockIdx.x * 4 ) + i41 ) * 128 ) + threadIdx.x ) < T3.size[0] ) ) { 
        T3[ ( ( ( ( ( blockIdx.x * 4 ) + i41 ) * 128 ) + threadIdx.x ) * T3.stride[0] ) ]
           = T2[ i41 ]
           * T0[ ( ( ( ( ( blockIdx.x * 4 ) + i41 ) * 128 ) + threadIdx.x ) * T0.stride[0] ) ];
      }
    }
  }
}
)";

  GPULower gpulw(&fusion);
  std::stringstream actual_kernel;
  actual_kernel << "\n";
  gpulw.printKernel(actual_kernel);
  if (expected_kernel.size() != actual_kernel.str().size() ||
      expected_kernel.compare(actual_kernel.str()) != 0) {
    std::cerr
        << " Codegen mismatch, codegen possibly changed, or is incorrect. "
        << " \n ========= EXPECTED ========= \n"
        << expected_kernel << "\n========= ACTUAL ========== \n"
        << actual_kernel.str() << "\n=================" << std::endl;
    TORCH_CHECK(false);
  }
}

void testGPU_FusionForLoop() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  const auto TV0 = new TensorView(
      new TensorDomain({new IterDomain(new Int(0), new Int(16))}),
      DataType::Float);
  const auto TV1 = new TensorView(
      new TensorDomain({new IterDomain(new Int(0), new Int(16))}),
      DataType::Float);

  fusion.addInput(TV0);
  fusion.addInput(TV1);

  auto ID0 = new IterDomain(new Int(0), new Int(8));

  TensorView* TV2 = add(TV0, TV1);
  BinaryOp* op = static_cast<BinaryOp*>(TV2->getOrigin());
  fusion.addOutput(TV2);

  ForLoop* fl = new ForLoop(new Int(), ID0, {op});
  std::stringstream result;
  std::stringstream ref;
  result << fl;
  ref << "for(size_t i3{0}; i3 < iS{8}; ++i3 ) {\nT2[ iS{16} ] = T0[ iS{16} ] + T1[ iS{16} ]\n}";

  if (result.str().compare(ref.str()) == 0) {
    std::stringstream err_msg;
    err_msg << "ForLoop printing has changed or something has gone wrong. "
            << result.str() << "\n does not match reference: " << ref.str()
            << std::endl;
    TORCH_CHECK(false, err_msg.str());
  }
}

void testGPU_FusionCodeGen() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeDummyTensor(3);

  new BinaryOp(BinaryOpType::Add, tv0, new Float(0.0), new Float(1.0));
  TensorView* tv1 = add(tv0, new Float(2.0));
  TensorView* tv2 = add(tv1, new Float(3.0));
  fusion.addOutput(tv2);

  //[I0, I1, I2]
  tv2 = tv2->split(0, 4);
  //[I0o, I0i{4}, I1, I2]
  tv2 = tv2->merge(1);
  //[I0o, I0i{4}*I1, I2]
  tv2 = tv2->split(-1, 2);
  //[I0o, I0i{4}*I1, I2o, I2i{2}]
  tv2 = tv2->reorder({{0, 1}, {1, 0}, {3, 2}});
  //[I0i{4}*I1, I0o, I2i{2}, I2o]

  tv0->computeAt(tv2, -1);

  prog.device_ = 0;
  // These can be set to anything as there are no bindings!
  // All CTAS and threads execute the same thing.
  prog.grid(4);
  prog.block(32);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  at::Tensor output = at::empty({16, 8, 8}, options);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {}, {output});

  at::Tensor output_ref = at::zeros_like(output, options);
  output_ref = output_ref + 0.0 + 1.0 + 2.0 + 3.0;

  TORCH_CHECK(output_ref.equal(output));
}

void testGPU_FusionCodeGen2() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeDummyTensor(3);
  TensorView* tv1 = makeDummyTensor(3);
  TensorView* tv2 = add(tv1, new Float(2.0));
  TensorView* tv3 = add(tv0, tv2);

  fusion.addInput(tv0);
  fusion.addInput(tv1);
  fusion.addOutput(tv3);

  //[I0, I1, I2]
  tv3->reorder({{0, 2}, {2, 0}});
  //[I2, I1, I0]
  tv3->split(-1, 4);
  //[I2, I1, I0o, I0i{4}]
  tv3->reorder({{2, 0}, {3, 1}, {0, 3}});
  // I0o, I0i{4}, I1, I2]

  tv0->computeAt(tv3, -1);
  tv1->computeAt(tv3, -1);

  tv3->axis(0)->parallelize(ParallelType::BIDx);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  prog.device_ = 0;
  prog.grid(4);
  prog.block(8);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  at::Tensor input1 = at::randn({16, 8, 8}, options);
  at::Tensor input2 = at::randn_like(input1);
  ;
  at::Tensor output = at::empty_like(input1);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input1, input2}, {output});

  at::Tensor tv2_ref = input2 + 2.0;
  at::Tensor output_ref = input1 + tv2_ref;

  TORCH_CHECK(output_ref.equal(output));
}

void testGPU_FusionSimplePWise() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);
  // dimensionality of the problem
  int nDims = 3;

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(nDims);
  TensorView* tv1 = makeDummyTensor(nDims);

  // Register your inputs
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // Do math with it, it returns a `Val*` but can be static_casted back to
  // TensorView
  TensorView* tv2 = add(tv1, new Float(2.0));
  TensorView* tv3 = add(tv0, tv2);

  // Register your outputs
  fusion.addOutput(tv3);

  // Do transformations, remember, transformations are outputs to inputs
  // This doesn't have to be in this order
  tv3->merge(1);
  tv3->merge(0);

  // Split by n_threads
  tv3->split(-1, 128 * 2);
  tv3->split(-1, 128);

  // For all inputs, computeAt the output inline, temporaries should be squeezed
  // between them
  tv0->computeAt(tv3, -1);
  tv1->computeAt(tv3, -1);

  // Parallelize TV3
  tv3->axis(0)->parallelize(ParallelType::BIDx);
  tv3->axis(-2)->parallelize(ParallelType::TIDy);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  prog.device_ = 0;
  prog.grid(64); //   1 CTA
  prog.block(128, 2); // 256 Threads

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  at::Tensor input1 = at::randn({64, 2, 128}, options);
  at::Tensor input2 = at::rand_like(input1);
  at::Tensor output = at::empty_like(input1);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input1, input2}, {output});

  at::Tensor tv2_ref = input2 + 2.0;
  at::Tensor output_ref = input1 + tv2_ref;

  TORCH_CHECK(output_ref.equal(output));
}

void testGPU_FusionExecKernel() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(2);
  TensorView* tv1 = makeDummyTensor(2);

  // Register your inputs
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // Do math with it, it returns a `Val*` but can be static_casted back to
  // TensorView
  TensorView* tv2 = add(tv1, new Float(2.0));
  TensorView* tv3 = add(tv0, tv2);

  // Register your outputs
  fusion.addOutput(tv3);

  tv3->merge(0);
  tv3->split(0, 128);
  tv3->split(0, 4);

  // For all inputs, computeAt the output inline, temporaries should be squeezed
  // between them
  tv0->computeAt(tv3, 1);
  tv1->computeAt(tv3, 1);

  // Parallelize TV3
  tv3->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::Unroll);
  tv3->axis(1)->parallelize(ParallelType::Unroll);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  prog.device_ = 0;
  prog.grid(1); // 1 CTA
  prog.block(128); // 128 Threads

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  at::Tensor input1 = at::ones({1, 128}, options);
  at::Tensor input2 = at::ones_like(input1);

  at::Tensor output = at::empty_like(input1);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input1, input2}, {output});

  at::Tensor check = at::full({1, 128}, 4, options);
  ;
  TORCH_CHECK(output.equal(check));
}

int ceilDiv_(int a, int b) {
  return (a + b - 1) / b;
}

void testGPU_FusionAdvancedComputeAt() {
  // Case 1
  /*
   * tv1 = tv0 * -1
   * tv2 = tv0 + 3
   * tv3 = tv0 * 2
   * tv4 = tv2 + tv1
   * tv5 = tv4 + tv3
   * tv6 = tv0 + tv3
   */
  {
    torch::jit::fuser::cuda::CudaKernel prog;
    Fusion& fusion = *prog.fusion_;
    FusionGuard fg(&fusion);

    TensorView* tv0 = makeDummyTensor(2);
    fusion.addInput(tv0);

    TensorView* tv1 = mul(tv0, new Float(-1.0));
    TensorView* tv2 = add(tv0, new Float(3.0));
    TensorView* tv3 = mul(tv0, new Float(2.0));
    TensorView* tv4 = add(tv2, tv1);

    TensorView* tv5 = add(tv4, tv3);
    TensorView* tv6 = add(tv0, tv3);

    fusion.addOutput(tv5);
    fusion.addOutput(tv6);

    tv0->computeAt(tv3, 1);

    // Check propagation of this computeAt.
    TORCH_CHECK(tv0->getComputeAtView() == tv3);
    TORCH_CHECK(tv1->getComputeAtView() == tv4);
    TORCH_CHECK(tv2->getComputeAtView() == tv4);
    TORCH_CHECK(tv3->getComputeAtView() == tv6);
    TORCH_CHECK(tv4->getComputeAtView() == tv5);
    TORCH_CHECK(tv5->getComputeAtView() == tv6);
    TORCH_CHECK(!tv6->hasComputeAt());

    // Lets setup to actually run
    tv6->merge(0);
    tv6->split(0, 128);
    tv6->split(0, 4);

    tv6->axis(0)->parallelize(ParallelType::BIDx);

    tv0->computeAt(tv6, 1);

    TORCH_CHECK(tv0->getComputeAtView() == tv6 && tv0->nDims() == 3);
    TORCH_CHECK(tv1->getComputeAtView() == tv4 && tv1->nDims() == 3);
    TORCH_CHECK(tv2->getComputeAtView() == tv4 && tv2->nDims() == 3);
    TORCH_CHECK(tv3->getComputeAtView() == tv6 && tv3->nDims() == 3);
    TORCH_CHECK(tv4->getComputeAtView() == tv5 && tv4->nDims() == 3);
    TORCH_CHECK(tv5->getComputeAtView() == tv6 && tv5->nDims() == 3);
    TORCH_CHECK(!tv6->hasComputeAt());

    for (Val* val : fusion.vals()) {
      if (!fusion.hasInput(val) &&
          val->getValType().value() == ValType::TensorView) {
        TensorView* tv = static_cast<TensorView*>(val);
        tv->axis(1)->parallelize(ParallelType::Unroll);
        tv->axis(-1)->parallelize(ParallelType::TIDx);
      }
    }

    auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

    at::Tensor t0 = at::randn({129, 127}, options);

    auto t1 = t0.mul({-1.0});
    auto t2 = t0.add({3.0});
    auto t3 = t0.mul({2.0});
    auto t4 = t2.add(t1);
    auto t5 = t4.add(t3);
    auto t6 = t0.add(t3);

    at::Tensor kernel_tv5 = at::empty_like(t0, options);
    at::Tensor kernel_tv6 = at::empty_like(t0, options);

    prog.device_ = 0;

    int blocks = ceilDiv_(
        ceilDiv_(t0.numel(), 128), 4); // numel / unroll factor / threads
    prog.grid(blocks);
    prog.block(128);
    torch::jit::fuser::cuda::compileKernel(&prog);
    torch::jit::fuser::cuda::runTestKernel(
        &prog, {t0}, {kernel_tv5, kernel_tv6});

    TORCH_CHECK(at::allclose(kernel_tv5, t5));
    TORCH_CHECK(at::allclose(kernel_tv6, t6));
  }

  // Case 2
  /*
   * tv1 = tv0 * -1
   * tv2 = tv0 + 3
   * tv3 = tv0 * 2
   * tv4 = tv2 + tv1
   * tv5 = tv4 + tv3
   * tv6 = tv5 + tv3
   */
  {
    torch::jit::fuser::cuda::CudaKernel prog;
    Fusion& fusion = *prog.fusion_;
    FusionGuard fg(&fusion);

    TensorView* tv0 = makeDummyTensor(2);
    fusion.addInput(tv0);

    TensorView* tv1 = mul(tv0, new Float(-1.0));
    TensorView* tv2 = add(tv0, new Float(3.0));
    TensorView* tv3 = mul(tv0, new Float(2.0));
    TensorView* tv4 = add(tv2, tv1);

    TensorView* tv5 = add(tv4, tv3);
    TensorView* tv6 = add(tv5, tv3);

    fusion.addOutput(tv5);
    fusion.addOutput(tv6);

    tv2->computeAt(tv4, 1);

    TORCH_CHECK(!tv0->hasComputeAt());
    TORCH_CHECK(!tv1->hasComputeAt());
    TORCH_CHECK(tv2->getComputeAtView() == tv4);
    TORCH_CHECK(!tv3->hasComputeAt());
    TORCH_CHECK(!tv4->hasComputeAt());
    TORCH_CHECK(!tv5->hasComputeAt());
    TORCH_CHECK(!tv6->hasComputeAt());

    // Lets setup to actually run
    tv6->merge(0);
    tv6->split(0, 128);
    tv6->split(0, 4);

    tv6->axis(0)->parallelize(ParallelType::BIDx);

    tv0->computeAt(tv6, 1);

    for (Val* val : fusion.vals()) {
      if (!fusion.hasInput(val) &&
          val->getValType().value() == ValType::TensorView) {
        TensorView* tv = static_cast<TensorView*>(val);

        tv->axis(1)->parallelize(ParallelType::Unroll);
        tv->axis(-1)->parallelize(ParallelType::TIDx);
      }
    }

    auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
    at::Tensor t0 = at::randn({129, 127}, options);

    auto t1 = t0.mul({-1.0});
    auto t2 = t0.add({3.0});
    auto t3 = t0.mul({2.0});
    auto t4 = t2.add(t1);
    auto t5 = t4.add(t3);
    auto t6 = t5.add(t3);

    at::Tensor kernel_tv5 = at::empty_like(t0, options);
    at::Tensor kernel_tv6 = at::empty_like(t0, options);

    prog.device_ = 0;

    int blocks = ceilDiv_(
        ceilDiv_(t0.numel(), 128), 4); // numel / unroll factor / threads
    prog.grid(blocks);
    prog.block(128);
    torch::jit::fuser::cuda::compileKernel(&prog);
    torch::jit::fuser::cuda::runTestKernel(
        &prog, {t0}, {kernel_tv5, kernel_tv6});

    GPULower gpulw(&fusion);
    std::stringstream actual_kernel;
    gpulw.printKernel(actual_kernel);

    TORCH_CHECK(at::allclose(kernel_tv5, t5), actual_kernel.str());
    TORCH_CHECK(at::allclose(kernel_tv6, t6));
  }

  // Case 3
  // T2 = T1 * 0.979361
  // T3 = T2 * T0
  {
    torch::jit::fuser::cuda::CudaKernel prog;
    Fusion& fusion = *prog.fusion_;
    FusionGuard fg(&fusion);

    TensorView* tv0 = makeDummyTensor(4);
    fusion.addInput(tv0);

    TensorView* tv1 = makeDummyTensor(4);
    fusion.addInput(tv1);

    TensorView* tv2 = mul(tv1, new Float(.979361));
    TensorView* tv3 = mul(tv2, tv0);

    fusion.addOutput(tv3);

    // Lets setup to actually run
    while (tv3->nDims() > 1)
      tv3->merge(0);
    tv3->split(0, 128);
    tv3->split(0, 4);

    tv0->computeAt(tv3, 1);
    tv1->computeAt(tv3, 1);

    tv3->axis(0)->parallelize(ParallelType::BIDx);

    for (Val* val : fusion.vals()) {
      if (!fusion.hasInput(val) &&
          val->getValType().value() == ValType::TensorView) {
        TensorView* tv = static_cast<TensorView*>(val);

        tv->axis(1)->parallelize(ParallelType::Unroll);
        tv->axis(-1)->parallelize(ParallelType::TIDx);
      }
    }

    auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
    at::Tensor t0 = at::randn({129, 127, 63, 65}, options);
    at::Tensor t1 = at::rand_like(t0, options);

    auto t2 = t1.mul({0.979361});
    auto t3 = t2.mul(t0);

    at::Tensor kernel_tv3 = at::empty_like(t0, options);

    prog.device_ = 0;

    int blocks = ceilDiv_(
        ceilDiv_(t0.numel(), 128), 4); // numel / unroll factor / threads

    prog.grid(blocks);
    prog.block(128);
    torch::jit::fuser::cuda::compileKernel(&prog);
    torch::jit::fuser::cuda::runTestKernel(&prog, {t0, t1}, {kernel_tv3});

    GPULower gpulw(&fusion);
    std::stringstream actual_kernel;
    gpulw.printKernel(actual_kernel);

    TORCH_CHECK(at::allclose(kernel_tv3, t3), actual_kernel.str());
  }

  // Case 4
  // T4 = T2 - T3
  // T5 = T1 + T4
  // T6 = T5 - T0
  {
    torch::jit::fuser::cuda::CudaKernel prog;
    Fusion& fusion = *prog.fusion_;
    FusionGuard fg(&fusion);

    TensorView* tv0 = makeDummyTensor(4);
    fusion.addInput(tv0);

    TensorView* tv1 = makeDummyTensor(4);
    fusion.addInput(tv1);

    TensorView* tv2 = makeDummyTensor(4);
    fusion.addInput(tv2);

    TensorView* tv3 = makeDummyTensor(4);
    fusion.addInput(tv3);

    TensorView* tv4 = sub(tv2, tv3);
    TensorView* tv5 = add(tv1, tv4);
    TensorView* tv6 = sub(tv5, tv0);

    fusion.addOutput(tv6);

    // Lets setup to actually run
    while (tv6->nDims() > 1)
      tv6->merge(0);
    tv6->split(0, 128);
    tv6->split(0, 4);

    tv0->computeAt(tv6, 1);
    tv1->computeAt(tv6, 1);
    tv2->computeAt(tv6, 1);
    tv3->computeAt(tv6, 1);

    tv6->axis(0)->parallelize(ParallelType::BIDx);

    for (Val* val : fusion.vals()) {
      if (!fusion.hasInput(val) &&
          val->getValType().value() == ValType::TensorView) {
        TensorView* tv = static_cast<TensorView*>(val);

        tv->axis(1)->parallelize(ParallelType::Unroll);
        tv->axis(-1)->parallelize(ParallelType::TIDx);
      }
    }

    auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
    at::Tensor t0 = at::randn({129, 127, 63, 65}, options);
    at::Tensor t1 = at::rand_like(t0, options);
    at::Tensor t2 = at::rand_like(t0, options);
    at::Tensor t3 = at::rand_like(t0, options);

    auto t4 = t2.sub(t3);
    auto t5 = t1.add(t4);
    auto t6 = t5.sub(t0);

    at::Tensor kernel_tv6 = at::empty_like(t0, options);

    prog.device_ = 0;

    int blocks = ceilDiv_(
        ceilDiv_(t0.numel(), 128), 4); // numel / unroll factor / threads

    prog.grid(blocks);
    prog.block(128);
    torch::jit::fuser::cuda::compileKernel(&prog);
    torch::jit::fuser::cuda::runTestKernel(
        &prog, {t0, t1, t2, t3}, {kernel_tv6});

    GPULower gpulw(&fusion);
    std::stringstream actual_kernel;
    gpulw.printKernel(actual_kernel);

    TORCH_CHECK(at::allclose(kernel_tv6, t6), actual_kernel.str());
  }
}

void testGPU_FusionScalarInputs() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);
  TensorView* tv1 = makeDummyTensor(2);
  fusion.addInput(tv1);

  Float* f0 = new Float();
  fusion.addInput(f0);
  Float* f1 = new Float();
  fusion.addInput(f1);
  Float* f2 = new Float();
  fusion.addInput(f2);
  Float* f3 = new Float();
  fusion.addInput(f3);
  Val* f4 = mul(f0, f1);
  Val* f5 = sub(f2, f3);

  TensorView* tv2 = sub(tv1, f4);
  TensorView* tv3 = add(tv0, f5);
  TensorView* tv4 = mul(tv3, tv2);

  fusion.addOutput(tv4);

  // Lets setup to actually run
  while (tv4->nDims() > 1)
    tv4->merge(0);
  tv4->split(0, 128);
  tv4->split(0, 4);

  tv0->computeAt(tv4, 1);
  tv1->computeAt(tv4, 1);

  tv4->axis(0)->parallelize(ParallelType::BIDx);

  for (Val* val : fusion.vals()) {
    if (!fusion.hasInput(val) &&
        val->getValType().value() == ValType::TensorView) {
      TensorView* tv = static_cast<TensorView*>(val);

      tv->axis(1)->parallelize(ParallelType::Unroll);
      tv->axis(-1)->parallelize(ParallelType::TIDx);
    }
  }

  // f4 = f0 * f1
  // f5 = f2 - f3
  // t2 = t1 - f4
  // t3 = t0 + f5
  // t4 = t3 * t2

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  float fl0 = 0.1;
  float fl1 = -0.2;
  float fl2 = 0.3;
  float fl3 = -0.4;
  float fl4 = fl0 * fl1;
  float fl5 = fl2 - fl3;

  at::Tensor t0 = at::randn({129, 127}, options);
  at::Tensor t1 = at::rand_like(t0, options);

  auto t2 = t1.sub(fl4);
  auto t3 = t0.add(fl5);
  auto t4 = t3.mul(t2);

  at::Tensor kernel_tv4 = at::empty_like(t0, options);

  prog.device_ = 0;

  int blocks =
      ceilDiv_(ceilDiv_(t0.numel(), 128), 4); // numel / unroll factor / threads

  prog.grid(blocks);
  prog.block(128);
  torch::jit::fuser::cuda::compileKernel(&prog);
  at::Scalar test(fl0);

  torch::jit::fuser::cuda::runTestKernel(
      &prog,
      {t0,
       t1,
       at::Scalar(fl0),
       at::Scalar(fl1),
       at::Scalar(fl2),
       at::Scalar(fl3)},
      {kernel_tv4});

  GPULower gpulw(&fusion);
  std::stringstream actual_kernel;
  gpulw.printKernel(actual_kernel);

  TORCH_CHECK(at::allclose(kernel_tv4, t4), actual_kernel.str());
}

void testGPU_FusionLoopUnroll() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(3);
  TensorView* tv1 = makeDummyTensor(3);

  // Register your inputs
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // Do math with it, it returns a `Val*` but can be static_casted back to
  // TensorView
  TensorView* tv2 = add(tv1, new Float(2.0));
  TensorView* tv3 = add(tv0, tv2);

  // Register your outputs
  fusion.addOutput(tv3);

  int block_size = 16;

  tv3->merge(0, 1);
  tv3->merge(0, 1);

  tv3->split(0, block_size);
  tv3->split(0, 4);

  // For all inputs, computeAt the output inline, temporaries should be squeezed
  // between them
  tv0->computeAt(tv3, 1);
  tv1->computeAt(tv3, 1);

  // Parallelize
  tv2->axis(1)->parallelize(ParallelType::Unroll);
  tv3->axis(1)->parallelize(ParallelType::Unroll);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);
  tv3->axis(0)->parallelize(ParallelType::BIDx);

  int inp_size = 129 * 13 * 3;

  prog.device_ = 0;
  prog.grid((inp_size + 63) / 64);
  prog.block(block_size);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  at::Tensor input0 = at::rand({129, 13, 3}, options);
  at::Tensor input1 = at::rand({129, 13, 3}, options);

  at::Tensor output = at::empty_like(input1);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input0, input1}, {output});

  TORCH_CHECK(output.equal(input0.add(input1.add(2.0))));
}

/*
 * Helper function for single op testing that generates a codegen operand
 */

Val* gen_jit_operand(std::pair<ValType, DataType> desc) {
  if (desc.first == ValType::TensorView) {
    return makeDummyTensor(2, desc.second);
  } else if (desc.first == ValType::Scalar) {
    if (desc.second == DataType::Float)
      return new Float();
    else if (desc.second == DataType::Int)
      return new Int();
    else
      TORCH_CHECK("Not currently supported type", desc.first);
  } else {
    TORCH_CHECK("Not currently supported type", desc.first);
  }
  return nullptr;
}

/*
 * Helper function for single op testing that generates an ATen operand
 */

IValue gen_aten_operand(
    std::pair<ValType, DataType> desc,
    int blocks,
    int threads,
    bool rand) {
  if (desc.first == ValType::TensorView) {
    if (desc.second == DataType::Float) {
      auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
      if (rand)
        return IValue(at::rand({blocks, threads}, options));
      else
        return IValue(at::empty({blocks, threads}, options));
    } else if (desc.second == DataType::Half) {
      auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
      if (rand)
        return IValue(at::rand({blocks, threads}, options));
      else
        return IValue(at::empty({blocks, threads}, options));
    } else if (desc.second == DataType::Bool) {
      if (rand) {
        auto options =
            at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
        return IValue(at::rand({blocks, threads}, options).to(at::kBool));
      } else {
        auto options =
            at::TensorOptions().dtype(at::kBool).device(at::kCUDA, 0);
        return IValue(at::empty({blocks, threads}, options));
      }
    } else {
      TORCH_CHECK("Not currently supported type", desc.second)
    }
  } else if (desc.first == ValType::Scalar) {
    if (desc.second == DataType::Float)
      return IValue(at::Scalar(1.f));
    else if (desc.second == DataType::Int)
      return IValue(at::Scalar(1));
    else
      TORCH_CHECK("Not currently supported type", desc.first);
  } else {
    TORCH_CHECK("Not currently supported type", desc.first);
  }
  return nullptr;
}

/*
 * Templatized Helper Function To generate single Op comparison between the
 * JIT codegen for Cuda and the ATen Library.
 */

using OutputPair = std::pair<ValType, DataType>;
template <
    typename AtenFunc,
    typename JitFunc,
    typename InputTuple,
    size_t... NumInputs>
void test_op(
    int blocks,
    int threads,
    std::string op_str,
    AtenFunc af,
    JitFunc jf,
    OutputPair op,
    InputTuple it,
    std::index_sequence<NumInputs...>) {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Generate Input JIT function Inputs and add them as Inputs to the Fusion
  // Graph
  std::array<Val*, sizeof...(NumInputs)> jit_inputs = {
      gen_jit_operand(std::get<NumInputs>(it))...};
  std::for_each(jit_inputs.begin(), jit_inputs.end(), [&fusion](Val* v) {
    fusion.addInput(v);
  });
  TensorView* out =
      static_cast<TensorView*>(jf(std::get<NumInputs>(jit_inputs)...));
  fusion.addOutput(out);

  std::for_each(jit_inputs.begin(), jit_inputs.end(), [out](Val* v) {
    if (v->getValType() == ValType::TensorView)
      static_cast<TensorView*>(v)->computeAt(out, -1);
  });
  out->axis(0)->parallelize(ParallelType::BIDx);
  out->axis(-1)->parallelize(ParallelType::TIDx);

  prog.device_ = 0;
  prog.grid(blocks);
  prog.block(threads);
  torch::jit::fuser::cuda::compileKernel(&prog);

  std::array<IValue, sizeof...(NumInputs)> aten_inputs = {gen_aten_operand(
      std::get<NumInputs>(it), blocks, threads, /*rand*/ true)...};
  const at::ArrayRef<IValue> aten_inputs_ivalues(aten_inputs);

  at::Tensor output =
      gen_aten_operand(op, blocks, threads, /*rand*/ false).toTensor();
  std::vector<at::Tensor> output_vect = {output};
  cudaDeviceSynchronize();
  if (fusion.hasRNG())
    at::manual_seed(0);
  torch::jit::fuser::cuda::runTestKernel(
      &prog, aten_inputs_ivalues, output_vect);
  cudaDeviceSynchronize();

  if (fusion.hasRNG())
    at::manual_seed(0);
  at::Tensor ref_output = af(aten_inputs);
  cudaDeviceSynchronize(); // This sync shouldn't be necessary;

  std::function<std::string()> aten_inputs_to_str =
      [&aten_inputs]() -> std::string {
    int input_cnt = 1;
    std::stringstream ss;
    std::for_each(
        aten_inputs.begin(), aten_inputs.end(), [&input_cnt, &ss](IValue& iv) {
          ss << "\nINPUT" << input_cnt++ << ": " << iv.toTensor();
        });
    return ss.str();
  };

  at::Tensor diff;
  if (output.scalar_type() == at::kBool) {
    diff = at::eq(output, ref_output);
  } else {
    diff = at::sub(output, ref_output);
  }

  TORCH_CHECK(
      (output.scalar_type() == at::kBool
           ? output.equal(ref_output)
           :
           // The absolute Tolerance was raised to 1e-07 from 1e-08 to allow
           // allow for the remainder function to pass.
           output.allclose(ref_output, /*rtol*/ 1e-05, /*atol*/ 1e-07)),
      "\nOp Type: -- ",
      op_str,
      " -- had a mismatch.",
      aten_inputs_to_str(),
      "\nJIT: ",
      output,
      "\nREF: ",
      ref_output,
      "\nDIFF: ",
      diff,
      "\n");
}

/*
 *  Templatized Helper Function that uses variadic templates to
 *  process a variable length Input Tuple of different Operand Type.
 */
template <typename AtenFunc, typename JitFunc, typename InputTuple>
void test_op(
    int blocks,
    int threads,
    std::string op_str,
    AtenFunc af,
    JitFunc jf,
    OutputPair op,
    InputTuple it) {
  static constexpr auto size = std::tuple_size<InputTuple>::value;
  test_op(
      blocks,
      threads,
      op_str,
      af,
      jf,
      op,
      it,
      std::make_index_sequence<size>{});
}

void testGPU_FusionUnaryOps() {
  using OpTuple =
      std::tuple<at::Tensor (*)(const at::Tensor&), UnaryOpType, std::string>;

  // [Note: explicit tuple type for uniform initialization list]
  // Tuple type must be explicitly specified for each uniform initialization
  // list within the vector to make this code compatible with some old env
  // which we still need to support. eg. gcc 5.4 + cuda 9.2.
  std::vector<OpTuple> ops{
      OpTuple{at::abs, UnaryOpType::Abs, "abs"},
      OpTuple{at::acos, UnaryOpType::Acos, "acos"},
      OpTuple{at::asin, UnaryOpType::Asin, "asin"},
      OpTuple{at::atan, UnaryOpType::Atan, "atan"},
      // There does not appear to be an appropriate ATen function for atanh
      // OpTuple{at::atanh,      UnaryOpType::Atanh,      "atanh"      },
      OpTuple{at::ceil, UnaryOpType::Ceil, "ceil"},
      OpTuple{at::cos, UnaryOpType::Cos, "cos"},
      OpTuple{at::cosh, UnaryOpType::Cosh, "cosh"},
      OpTuple{at::erf, UnaryOpType::Erf, "erf"},
      OpTuple{at::erfc, UnaryOpType::Erfc, "erfc"},
      OpTuple{at::exp, UnaryOpType::Exp, "exp"},
      OpTuple{at::expm1, UnaryOpType::Expm1, "expm1"},
      OpTuple{at::floor, UnaryOpType::Floor, "floor"},
      OpTuple{at::frac, UnaryOpType::Frac, "frac"},
      OpTuple{at::gelu, UnaryOpType::Gelu, "gelu"},
      OpTuple{at::lgamma, UnaryOpType::Lgamma, "lgamma"},
      OpTuple{at::log, UnaryOpType::Log, "log"},
      OpTuple{at::log10, UnaryOpType::Log10, "log10"},
      OpTuple{at::log1p, UnaryOpType::Log1p, "log1p"},
      OpTuple{at::log2, UnaryOpType::Log2, "log2"},
      OpTuple{at::neg, UnaryOpType::Neg, "neg"},
      OpTuple{at::reciprocal, UnaryOpType::Reciprocal, "reciprocal"},
      OpTuple{at::relu, UnaryOpType::Relu, "relu"},
      OpTuple{at::round, UnaryOpType::Round, "round"},
      OpTuple{at::rsqrt, UnaryOpType::Rsqrt, "rsqrt"},
      OpTuple{at::sigmoid, UnaryOpType::Sigmoid, "sigmoid"},
      OpTuple{at::sin, UnaryOpType::Sin, "sin"},
      OpTuple{at::sinh, UnaryOpType::Sinh, "sinh"},
      OpTuple{at::sqrt, UnaryOpType::Sqrt, "sqrt"},
      OpTuple{at::tan, UnaryOpType::Tan, "tan"},
      OpTuple{at::tanh, UnaryOpType::Tanh, "tanh"},
      OpTuple{at::trunc, UnaryOpType::Trunc, "trunc"}};

  std::for_each(ops.begin(), ops.end(), [](OpTuple& op) {
    test_op(
        /*blocks*/ 640,
        /*threads*/ 64,
        /*name*/ std::get<2>(op),
        /*Aten Func   */
        [&op](std::array<IValue, 1>& vals) {
          return std::get<0>(op)(vals[0].toTensor());
        },
        /*JIT  Func   */
        [&op](Val* in1) -> Val* { return unaryOp(std::get<1>(op), in1); },
        /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
        /*Inputs Tuple*/
        std::make_tuple(std::make_pair(ValType::TensorView, DataType::Float)));
  });

  test_op(
      /*blocks*/ 128,
      /*threads*/ 64,
      /*name*/ "rand_like",
      /*Aten Func   */
      [](std::array<IValue, 1>& vals) {
        return at::rand_like(vals[0].toTensor());
      },
      /*JIT  Func   */
      [](Val* in1) -> Val* { return unaryOp(UnaryOpType::RandLike, in1); },
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(std::make_pair(ValType::TensorView, DataType::Float)));
}

void testGPU_FusionBinaryOps() {
  using AtenFuncSig = at::Tensor (*)(const at::Tensor&, const at::Tensor&);
  using OpTuple = std::tuple<AtenFuncSig, BinaryOpType, std::string>;

  // see [Note: explicit tuple type for uniform initialization list]
  std::vector<OpTuple> logic_ops{OpTuple{at::eq, BinaryOpType::Eq, "eq"},
                                 OpTuple{at::ge, BinaryOpType::GE, "ge"},
                                 OpTuple{at::gt, BinaryOpType::GT, "gt"},
                                 OpTuple{at::le, BinaryOpType::LE, "le"},
                                 OpTuple{at::lt, BinaryOpType::LT, "lt"},
                                 OpTuple{at::ne, BinaryOpType::NE, "ne"}};

  std::for_each(logic_ops.begin(), logic_ops.end(), [](OpTuple& op) {
    test_op(
        /*blocks*/ 640,
        /*threads*/ 64,
        /*name*/ std::get<2>(op),
        /*Aten Func   */
        [&op](std::array<IValue, 2>& vals) {
          return std::get<0>(op)(vals[0].toTensor(), vals[1].toTensor());
        },
        /*JIT  Func   */
        [&op](Val* in1, Val* in2) -> Val* {
          return binaryOp(std::get<1>(op), in1, in2);
        },
        /*Output      */ std::make_pair(ValType::TensorView, DataType::Bool),
        /*Inputs Tuple*/
        std::make_tuple(
            std::make_pair(ValType::TensorView, DataType::Float),
            std::make_pair(ValType::TensorView, DataType::Float)));
  });

  // see [Note: explicit tuple type for uniform initialization list]
  std::vector<OpTuple> math_ops{
      OpTuple{at::atan2, BinaryOpType::Atan2, "atan2"},
      OpTuple{at::div, BinaryOpType::Div, "div"},
      OpTuple{at::fmod, BinaryOpType::Fmod, "fmod"},
      OpTuple{at::max, BinaryOpType::Max, "max"},
      OpTuple{at::min, BinaryOpType::Min, "min"},
      OpTuple{at::mul, BinaryOpType::Mul, "mul"},
      OpTuple{at::pow, BinaryOpType::Pow, "pow"},
      // NOTE: Remainder does not match the Aten impl exactly
      // despite using an identical function.
      OpTuple{at::remainder, BinaryOpType::Remainder, "remainder"},
  };

  std::for_each(math_ops.begin(), math_ops.end(), [](OpTuple& op) {
    test_op(
        /*blocks*/ 640,
        /*threads*/ 64,
        /*name*/ std::get<2>(op),
        /*Aten Func   */
        [&op](std::array<IValue, 2>& vals) {
          return std::get<0>(op)(vals[0].toTensor(), vals[1].toTensor());
        },
        /*JIT  Func   */
        [&op](Val* in1, Val* in2) -> Val* {
          return binaryOp(std::get<1>(op), in1, in2);
        },
        /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
        /*Inputs Tuple*/
        std::make_tuple(
            std::make_pair(ValType::TensorView, DataType::Float),
            std::make_pair(ValType::TensorView, DataType::Float)));
  });

  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "add_alpha",
      /*Aten Func   */
      [](std::array<IValue, 3>& vals) {
        return at::add(
            vals[0].toTensor(), vals[1].toTensor(), vals[2].toScalar());
      },
      /*JIT  Func   */ static_cast<Val* (*)(Val*, Val*, Val*)>(&add_alpha),
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::Scalar, DataType::Float)));
  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "sub_alpha",
      /*Aten Func   */
      [](std::array<IValue, 3>& vals) {
        return at::sub(
            vals[0].toTensor(), vals[1].toTensor(), vals[2].toScalar());
      },
      /*JIT  Func   */ static_cast<Val* (*)(Val*, Val*, Val*)>(&sub_alpha),
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::Scalar, DataType::Float)));
}

void testGPU_FusionTernaryOps() {
  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "clamp",
      /*Aten Func   */
      [](std::array<IValue, 1>& vals) {
        return at::clamp(vals[0].toTensor(), 0.f, 1.f);
      },
      /*JIT  Func   */
      [](Val* in1) -> Val* {
        return clamp(in1, new Float(0.f), new Float(1.f));
      },
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(std::make_pair(ValType::TensorView, DataType::Float)));
  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "threshold",
      /*Aten Func   */
      [](std::array<IValue, 1>& vals) {
        return at::threshold(vals[0].toTensor(), 0.f, 1.f);
      },
      /*JIT  Func   */
      [](Val* in1) -> Val* {
        return threshold(in1, new Float(0.f), new Float(1.f));
      },
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(std::make_pair(ValType::TensorView, DataType::Float)));
  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "where",
      /*Aten Func   */
      [](std::array<IValue, 3>& vals) {
        return at::where(
            vals[0].toTensor(), vals[1].toTensor(), vals[2].toTensor());
      },
      /*JIT  Func   */ static_cast<Val* (*)(Val*, Val*, Val*)>(&where),
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(
          std::make_pair(ValType::TensorView, DataType::Bool),
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float)));
}

void testGPU_FusionCompoundOps() {
  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "lerp",
      /*Aten Func   */
      [](std::array<IValue, 3>& vals) {
        return at::lerp(
            vals[0].toTensor(), vals[1].toTensor(), vals[2].toTensor());
      },
      /*JIT  Func   */ static_cast<Val* (*)(Val*, Val*, Val*)>(&lerp),
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float)));
  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "addcmul",
      /*Aten Func   */
      [](std::array<IValue, 4>& vals) {
        return at::addcmul(
            vals[0].toTensor(),
            vals[1].toTensor(),
            vals[2].toTensor(),
            vals[3].toScalar());
      },
      /*JIT  Func   */ static_cast<Val* (*)(Val*, Val*, Val*, Val*)>(&addcmul),
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::Scalar, DataType::Float)));
}

void testGPU_FusionCastOps() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeDummyTensor(2, DataType::Half);

  TensorView* intrm1 = castOp(DataType::Float, tv0);
  TensorView* out = castOp(DataType::Half, intrm1);

  fusion.addInput(tv0);
  fusion.addOutput(out);
  tv0->computeAt(out, -1);

  out->axis(0)->parallelize(ParallelType::BIDx);
  out->axis(-1)->parallelize(ParallelType::TIDx);

  prog.device_ = 0;
  prog.grid(1);
  prog.block(4);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);

  at::Tensor input1 = at::rand({1, 4}, options);
  at::Tensor output = at::empty_like(input1);
  at::Tensor ref_output = at::empty_like(input1);

  std::array<IValue, 1> inputs = {input1};
  const at::ArrayRef<IValue> input_ivalues(inputs);
  std::vector<at::Tensor> outputs{{output}};

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, input_ivalues, outputs);

  ref_output = at::_cast_Half(at::_cast_Float(input1));

  TORCH_CHECK(
      output.equal(ref_output),
      "\nOp Type: -- ",
      "cast FP16->FP32->FP16",
      " -- had a mismatch.\n",
      "IN1 : ",
      input1,
      "\n",
      "JIT: ",
      output,
      "\n",
      "REF: ",
      ref_output,
      "\n");
}

// We want split/merge/reorder all tested both on and off rfactor domains, also
// want compute at into the rfactor domain, and into its consumer
void testGPU_FusionRFactorReplay() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(2);

  // Register your inputs
  fusion.addInput(tv0);

  // Do math with it, it returns a `Val*` but can be static_casted back to
  // TensorView
  TensorView* tv1 = sum(tv0, {1});
  // tv1[I0, R1]
  tv1->split(0, 32);
  // tv1[I0o, I0i{32}, R1]
  tv1->split(0, 16);
  // tv1[I0oo, I0oi{16}, I0i{32}, R1]
  tv1->split(-1, 8);
  // tv1[I0oo, I0oi{16}, I0i{32}, R1o, R1i{8}]
  tv1->split(-2, 4);
  // tv1[I0oo, I0oi{16}, I0i{32}, R1oo, R1oi{4}, R1i{8}]
  tv1->reorder({{0, -2}, {2, -1}, {-3, 0}, {-1, 1}});
  // tv1[R1oo, R1i{8}, I0oi{16}, R1oi{4}, I0oo, I0i{32}]

  tv1->merge(0);
  tv1->merge(-2);

  // tv1[R1oo*R1i{8}, I0oi{16}, R1oi{4}, I0oo*I0i{32}]
  TensorDomain* new_domain = TransformRFactor::runReplay(tv1->domain(), {0});
  // new_domain[r(R1oo*R1i{8})rf, I0oi{16}, ir1oi{4}rf, I0oo*I0i{32}]

  TensorDomain* new_domain2 = TransformRFactor::runReplay2(tv1->domain(), {0});
  // new_domain2[                 I0oi{16},           , I0oo*I0i{32}, R1oi{4}]

  // Move rfactor axis to end, keep iter rfactor axis
  new_domain->reorder({{0, -1}, {2, 2}});

  // Replay casp, replay new_domain2 as new_domain
  // reordered_new_domain[I0oi{16}, I0oo*I0i{32}, ir1oi{4}rf, R(R1oo*R1i{8})rf]
  auto replay_casp = TransformReplay::replayCasP(new_domain2, new_domain, 2);
  TensorDomain* casp = replay_casp.first;
  // new_domain[I0oi{16}, I0oo*I0i{32}, ir1oi{4}rf, R(R1oo*R1i{8})rf]
  //       casp[I0oi{16}, I0oo*I0i{32},  R1oi{4}]

  casp->split(1, 2);
  // casp      [I0oi{16}, (I0oo*I0i{32})o, I(Ioo*I0i)i{2}, ir1oi{4} ]
  // new_domain[I0oi{16},  I0oo*I0i{32}  ,                 ir1oi{4}rf,
  // R(R1oo*R1i{8})rf]

  auto replay_pasc = TransformReplay::replayPasC(new_domain, casp, 2);
  TensorDomain* pasc = replay_pasc.first;
  // pasc      [I0oi{16}, (I0oo*I0i{32})o, I(Ioo*I0i)i{2}, ir1oi{4}rf,
  // R(R1oo*R1i{8})rf]

  TORCH_CHECK(
      new_domain->nDims() - 1 == new_domain2->nDims(),
      casp->nDims() == new_domain2->nDims() + 1,
      pasc->nDims() == new_domain->nDims() + 1,
      "Error in rfactor, number of dimensions is not correct.");

  TORCH_CHECK(
      !casp->sameAs(new_domain2) && !pasc->sameAs(new_domain) &&
          !new_domain->sameAs(new_domain2) &&
          !tv1->domain()->sameAs(new_domain) &&
          !tv1->domain()->sameAs(new_domain2),
      "Error in rfactor, number of dimensions is not correct.");

  auto dom = new_domain->rootDomain();
  TORCH_CHECK(
      !dom[0]->isReduction() &&
          std::any_of(
              dom.begin(),
              dom.end(),
              [](IterDomain* id) { return id->isReduction(); }) &&
          std::any_of(
              dom.begin(),
              dom.end(),
              [](IterDomain* id) { return id->isRFactorProduct(); }),
      "Error in rFactor, there seems to be something wrong in root domain.");

  auto dom2 = new_domain2->rootDomain();
  TORCH_CHECK(
      !dom2[0]->isReduction() &&
          std::any_of(
              dom2.begin(),
              dom2.end(),
              [](IterDomain* id) { return id->isReduction(); }),
      "Error in rFactor, there seems to be something wrong in root domain.");
}

// Start off simple, block on the outer dim
// block stride + thread all reduce + unrolling on inner dim
void testGPU_FusionReduction() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);

  // tv1[I0, R1] = tv0[I0, I1]
  TensorView* tv1 = reductionOp(BinaryOpType::Add, {1}, new Float(0), tv0);
  fusion.addOutput(tv1);

  TORCH_CHECK(fusion.hasReduction(), "Could not detect reduction in fusion.");

  tv1->split(1, 128);
  // tv1[I0, R1o, R1i{128}] = tv0[I0, I1]
  tv1->split(1, 4);
  // tv1[I0, R1oo, R1oi{4}, R1i{128}] = tv0[I0, I1]

  TensorView* tv2 = tv1->rFactor({1});
  // tv2[I0, R1oo, Ir1oi{4}, Ir1i{128}] = tv0[I0, I1]
  // tv1[I0,        R1oi{4},  R1i{128}] = tv2[I0, R1oo, Ir1oi{4}, Ir1i{128}]

  TensorView* tv3 = tv1->rFactor({1});
  // tv2[I0, R1oo, Ir1oi{4}, Ir1i{128}] = tv0[I0, I1]
  // tv3[I0,        R1oi{4}, Ir1i{128}] = tv2[I0, R1oo, Ir1oi{4}, Ir1i{128}]
  // tv1[I0,                  R1i{128}] = tv3[I0,        R1oi{4}, Ir1i{128}]

  // Incrementally, can print in between for debugging
  tv0->computeAt(tv2, 1);
  tv2->computeAt(tv3, 1);
  tv3->computeAt(tv1, 1);

  // Re do it all at once, because why not.
  tv0->computeAt(tv1, 1);

  tv2->axis(2)->parallelize(ParallelType::Unroll);
  tv1->axis(0)->parallelize(ParallelType::BIDx);

  tv1->axis(-1)->parallelize(ParallelType::TIDx);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  int numel_x = 65000;
  int numel_y = 1025;

  prog.device_ = 0;
  prog.grid(numel_x);
  prog.block(128);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::rand({numel_x, numel_y}, options);
  at::Tensor cg_output = at::empty({numel_x}, options);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

  auto aten_output = input.sum({1});
  TORCH_CHECK(aten_output.allclose(cg_output));
}

void testGPU_FusionReduction2() {
  {
    torch::jit::fuser::cuda::CudaKernel prog;
    Fusion& fusion = *prog.fusion_;
    FusionGuard fg(&fusion);

    // Set up your input tensor views
    TensorView* tv0 = makeDummyTensor(2);
    fusion.addInput(tv0);

    // tv1[I0, R1] = tv0[I0, I1]
    TensorView* tv1 = reductionOp(BinaryOpType::Add, {1}, new Float(0), tv0);

    fusion.addOutput(tv1);

    // switches to try some different scenarios. maybe we should iterate on all
    // permutations.
    bool bind_bidx = true;
    bool bind_tidx = true;
    bool bind_tidy = true;
    bool bind_unroll = true;

    int numel_x = 1025; // Cannot exceed block dim max size / tidy
    int numel_y = 129;
    int tidx = 16;
    int tidy = 8;
    int unroll_factor = 4;

    int bidx = bind_tidy ? ceilDiv_(numel_x, tidy) : numel_x;

    tv1->split(1, tidx);
    // tv1[I0, R1o, R1i{tidx}] = tv0[I0, I1]

    tv1->split(1, unroll_factor);
    // tv1[I0, R1oo, R1oi{unroll}, R1i{tidx}] = tv0[I0, I1]

    tv1->split(0, tidy);

    TensorView* tv2 = tv1->rFactor({-3});
    // tv2[I0,             >R1oo<, Ir1oi{unroll}, Ir1i{tidx}]
    // tv1[I0o, I0i{tidy},          R1oi{unroll},  R1i{tidx}]

    TensorView* tv3 = tv1->rFactor({-2});
    // tv2[I0,             >R1oo<, Ir1oi{unroll}, Ir1i{tidx}]
    // tv3[I0,                      R1oi{unroll}, Ir1i{tidx}]
    // tv1[I0o, I0i{tidy},                         R1i{tidx}]

    tv0->computeAt(tv1, -2);

    if (bind_unroll)
      tv2->axis(-2)->parallelize(ParallelType::Unroll);
    if (bind_bidx)
      tv1->axis(0)->parallelize(ParallelType::BIDx);
    if (bind_tidy)
      tv1->axis(1)->parallelize(ParallelType::TIDy);

    if (bind_tidx) {
      tv2->axis(-1)->parallelize(ParallelType::TIDx);
      tv3->axis(-1)->parallelize(ParallelType::TIDx);
      tv1->axis(-1)->parallelize(ParallelType::TIDx);
    }

    prog.device_ = 0;
    prog.grid(bind_bidx ? bidx : 1);
    prog.block(bind_tidx ? tidx : 1, bind_tidy ? tidy : 1);

    auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
    at::Tensor input = at::rand({numel_x, numel_y}, options);
    at::Tensor cg_output = at::empty({numel_x}, options);

    torch::jit::fuser::cuda::compileKernel(&prog);
    torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

    c10::cuda::CUDAStream stream = c10::cuda::getCurrentCUDAStream();
    AT_CUDA_CHECK(cudaStreamSynchronize(stream));

    auto aten_output = input.sum({1});
    TORCH_CHECK(aten_output.allclose(cg_output));
  }

  {
    // What if Z participates in the reduction with X?
    torch::jit::fuser::cuda::CudaKernel prog;
    Fusion& fusion = *prog.fusion_;
    FusionGuard fg(&fusion);

    // Set up your input tensor views
    TensorView* tv0 = makeDummyTensor(2);
    fusion.addInput(tv0);

    // tv1[I0, R1] = tv0[I0, I1]
    TensorView* tv1 = reductionOp(BinaryOpType::Add, {1}, new Float(0), tv0);

    fusion.addOutput(tv1);

    int numel_x = 1025; // Cannot exceed block dim max size / tidy
    int numel_y = 129;
    int tidx = 16;
    int tidz = 8;

    tv1->split(1, tidz);
    // tv1[I0, R1o, R1i{tidz}] = tv0[I0, I1]

    tv1->split(1, tidx);
    // tv1[I0, R1oo, R1oi{tidx}, R1i{tidz}] = tv0[I0, I1]

    TensorView* tv2 = tv1->rFactor({-3});
    // tv2[I0,  >R1oo<, Ir1oi{tidx}, Ir1i{tidz}]
    // tv1[I0o,          R1oi{tidx},  R1i{tidz}]

    tv0->computeAt(tv1, -3);

    tv1->axis(0)->parallelize(ParallelType::BIDx);
    tv1->axis(-2)->parallelize(ParallelType::TIDx);
    tv1->axis(-1)->parallelize(ParallelType::TIDz);

    tv2->axis(-2)->parallelize(ParallelType::TIDx);
    tv2->axis(-1)->parallelize(ParallelType::TIDz);

    prog.device_ = 0;
    prog.grid(numel_x);
    prog.block(tidx, 1, tidz);

    auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
    at::Tensor input = at::rand({numel_x, numel_y}, options);
    at::Tensor cg_output = at::empty({numel_x}, options);

    torch::jit::fuser::cuda::compileKernel(&prog);
    torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

    c10::cuda::CUDAStream stream = c10::cuda::getCurrentCUDAStream();
    AT_CUDA_CHECK(cudaStreamSynchronize(stream));

    auto aten_output = input.sum({1});
    TORCH_CHECK(aten_output.allclose(cg_output));
  }
}

// TODO: Fix and reenable this test.
void testGPU_FusionReduction3() {
  {
    torch::jit::fuser::cuda::CudaKernel prog;
    Fusion& fusion = *prog.fusion_;
    FusionGuard fg(&fusion);

    // Set up your input tensor views
    TensorView* tv0 = makeDummyTensor(2);
    TensorView* tv1 = makeDummyTensor(2);

    TensorView* tv2 = add(tv0, tv1);
    // tv2[I0, I1] = tv0[I0, I1] + tv1[I0, I1]

    fusion.addInput(tv0);
    fusion.addInput(tv1);

    TensorView* tv3 = reductionOp(BinaryOpType::Add, {1}, new Float(0), tv2);
    // tv3[I0, R1] = tv2[I0, I1]

    TensorView* tv4 = makeDummyTensor(1);
    fusion.addInput(tv4);

    // tv5[I0] = tv3[I0, R1] * tv4[I0]
    TensorView* tv5 = mul(tv3, tv4);
    fusion.addOutput(tv5);

    int tidx = 16;

    // RFactor the reduction
    tv3->split(1, tidx);
    // tv3[I0, R1o, R1i{tidx}] = tv2[I0, I1]

    TensorView* tv6 = tv3->rFactor({-2});
    // tv6[I0, R1o, iR1i{tidx}] = tv2[I0, I1]
    // tv3[I0,       R1i{tidx}] = tv3[I0, I1]
    tv2->computeAt(tv6, 2);

    // Compute at inline with tv5 (only 1D)
    tv6->computeAt(tv3, 1);
    tv3->computeAt(tv5, 1);

    tv5->axis(0)->parallelize(ParallelType::BIDx);

    // Intermediate tensors only need this, but doesn't hurt to do on inputs
    // tv0, 1, 4
    tv2->axis(-1)->parallelize(ParallelType::TIDx);
    tv3->axis(-1)->parallelize(ParallelType::TIDx);
    tv6->axis(-1)->parallelize(ParallelType::TIDx);

    int numel_x = 1025;
    int numel_y = 129;
    int bidx = numel_x;

    prog.device_ = 0;
    prog.grid(bidx);
    prog.block(tidx);

    auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
    at::Tensor t0 = at::rand({numel_x, numel_y}, options);
    at::Tensor t1 = at::rand({numel_x, numel_y}, options);
    auto t2 = t0.add(t1);
    auto t3 = t2.sum({1});
    at::Tensor t4 = at::rand({numel_x}, options);
    auto t5 = t3.mul(t4);

    at::Tensor cg_output = at::empty({numel_x}, options);

    torch::jit::fuser::cuda::compileKernel(&prog);
    torch::jit::fuser::cuda::runTestKernel(&prog, {t0, t1, t4}, {cg_output});

    c10::cuda::CUDAStream stream = c10::cuda::getCurrentCUDAStream();
    AT_CUDA_CHECK(cudaStreamSynchronize(stream));

    TORCH_CHECK(
        t5.allclose(cg_output), "Error of: ", t5.sub(cg_output).abs().max());
  }
}

void testGPU_FusionReduction4() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(3);

  fusion.addInput(tv0);

  TensorView* tv1 = reductionOp(BinaryOpType::Add, {1}, new Float(0), tv0);

  fusion.addOutput(tv1);

  int bidy = 2;
  int tidy = 4;
  int tidx = 5;

  int dim1 = 11;

  tv1->split(-2, tidy);

  TensorView* tv2 = tv1->rFactor({-3});

  tv0->computeAt(tv1, 1);

  tv1->axis(0)->parallelize(ParallelType::BIDy);

  for (auto* val : fusion.vals()) {
    if (val->getValType().value() == ValType::TensorView)
      val->as<TensorView>()->axis(-1)->parallelize(ParallelType::TIDx);
  }

  tv2->axis(-2)->parallelize(ParallelType::TIDy);
  tv1->axis(-2)->parallelize(ParallelType::TIDy);

  prog.device_ = 0;
  prog.grid(1, bidy);
  prog.block(tidx, tidy);
  torch::jit::fuser::cuda::compileKernel(&prog);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::randn({bidy, dim1, tidx}, options);

  at::Tensor cg_output = at::empty({bidy, tidx}, options);

  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

  auto aten_output = input.sum({1});
  TORCH_CHECK(aten_output.allclose(cg_output));
}

void testGPU_FusionReduction5() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  const int bdimx = 64;
  const int bdimy = 8;

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(3);
  fusion.addInput(tv0);

  // tv1[I0, R1, R2] = tv0[I0, I1, I2]
  TensorView* tv1 = reductionOp(BinaryOpType::Add, {1, 2}, new Float(0), tv0);
  fusion.addOutput(tv1);

  TORCH_CHECK(fusion.hasReduction(), "Could not detect reduction in fusion.");

  tv1->split(2, bdimx);
  // tv1[I0, R1, R2o, R2i{128}] = tv0[I0, I1, I2]
  tv1->split(1, bdimy);
  // tv1[I0, R1o, R1i{8}, R2o, R2i{128}] = tv0[I0, I1, I2]

  TensorView* tv2 = tv1->rFactor({3});
  // tv2[I0, I1o, I1i{8}, R2o, I2i{128}] = tv0[I0, I1, I2]
  // tv1[I0, R1o, R1i{8},      R2i{128}] = tv2[I0, I1o, I1i{8}, R2o, I2i{128}]

  TensorView* tv3 = tv1->rFactor({1});
  // tv2[I0, I1o, I1i{8}, R2o, I2i{128}] = tv0[I0, I1, I2]
  // tv3[I0, R1o, I1i{8},      I2i{128}] = tv2[I0, I1o, I1i{8}, R2o, I2i{128}]
  // tv1[I0,      R1i{8},      R2i{128}] = tv3[I0, R1o, I1i{8},      I2i{128}]

  tv3->computeAt(tv1, 1);
  tv2->computeAt(tv3, 2);

  tv1->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(0)->parallelize(ParallelType::BIDx);
  tv3->axis(0)->parallelize(ParallelType::BIDx);

  tv1->axis(-1)->parallelize(ParallelType::TIDx);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  tv1->axis(-2)->parallelize(ParallelType::TIDy);
  tv3->axis(-2)->parallelize(ParallelType::TIDy);
  tv2->axis(-3)->parallelize(ParallelType::TIDy);

  int numel_x = 650;
  int numel_y = 1000;
  int numel_z = 1000;

  prog.device_ = 0;
  prog.grid(numel_x);
  prog.block(bdimx, bdimy);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::rand({numel_x, numel_y, numel_z}, options);
  at::Tensor cg_output = at::empty({numel_x}, options);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

  auto aten_output = input.sum({1, 2});
  TORCH_CHECK(aten_output.allclose(cg_output));
}

void testGPU_FusionReductionTFT() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);

  // tv1[I0, R1] = tv0[I0, I1]
  TensorView* tv1 = reductionOp(BinaryOpType::Add, {1}, new Float(0), tv0);

  fusion.addOutput(tv1);

  int numel_x = 1025;
  int numel_y = 129;
  int tidx = 16;
  int tidy = 8;
  int tidz = 8;

  tv1->split(1, tidx);
  // tv1[I0, R1o, R1i{tidx}]

  tv1->split(1, tidz);
  // tv1[I0, R1oo, R1Oi{tidz}, R1R1i{tidx}]

  tv1->split(0, tidy);
  // tv1[I0o, I0i, R1oo, R1Oi{tidz}, R1R1i{tidx}]

  TensorView* tv2 = tv1->rFactor({2});
  // tv2[I0o, I0i, R1oo, I1Oi{tidz}, I11i{tidx}]
  // tv1[I0o, I0i,       R1Oi{tidz}, R1R1i{tidx}]

  tv2->computeAt(tv1, 2);

  tv1->axis(1)->parallelize(ParallelType::TIDy);

  tv2->axis(-1)->parallelize(ParallelType::TIDx);
  tv1->axis(-1)->parallelize(ParallelType::TIDx);

  tv1->axis(-2)->parallelize(ParallelType::TIDz);
  tv2->axis(-2)->parallelize(ParallelType::TIDz);

  prog.device_ = 0;
  prog.grid(1);
  prog.block(tidx, tidy, tidz);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::rand({numel_x, numel_y}, options);
  at::Tensor cg_output = at::empty({numel_x}, options);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

  c10::cuda::CUDAStream stream = c10::cuda::getCurrentCUDAStream();
  AT_CUDA_CHECK(cudaStreamSynchronize(stream));

  auto aten_output = input.sum({1});
  TORCH_CHECK(aten_output.allclose(cg_output));
}

void testGPU_FusionSimpleBCast() {
  {
    torch::jit::fuser::cuda::CudaKernel prog;
    Fusion& fusion = *prog.fusion_;
    FusionGuard fg(&fusion);

    // Set up your input tensor views
    TensorView* tv0 = makeDummyTensor(2);
    TensorView* tv1 = makeDummyTensor(2);
    fusion.addInput(tv0);
    fusion.addInput(tv1);

    TensorView* tv2 = broadcast(tv0, {false, false, true});
    TensorView* tv3 = broadcast(tv1, {true, false, false});

    TensorView* tv4 = add(tv2, tv3);
    tv4->split(-1, 4);
    tv4->split(0, 8);
    fusion.addOutput(tv4);

    tv0->computeAt(tv4, -1);
    tv1->computeAt(tv4, -1);

    tv4->axis(0)->parallelize(ParallelType::BIDx);
    tv4->axis(-1)->parallelize(ParallelType::TIDx);

    constexpr int x = 63, y = 33, z = 15;

    auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

    at::Tensor t0 = at::randn({x, y}, options);
    at::Tensor t1 = at::randn({y, z}, options);

    at::Tensor cg_output = at::empty({x, y, z}, options);

    prog.device_ = 0;
    prog.grid(ceilDiv_(x, 8));
    prog.block(4);
    torch::jit::fuser::cuda::compileKernel(&prog);
    torch::jit::fuser::cuda::runTestKernel(&prog, {t0, t1}, {cg_output});

    auto t2 = t0.unsqueeze(-1).expand({x, y, z});
    auto t3 = t1.expand({x, y, z});
    auto t4 = t2.add(t3);

    TORCH_CHECK(t4.allclose(cg_output));
  }

  {
    torch::jit::fuser::cuda::CudaKernel prog;
    Fusion& fusion = *prog.fusion_;
    FusionGuard fg(&fusion);

    // Set up your input tensor views
    TensorView* tv0 = makeDummyTensor(2);
    TensorView* tv1 = makeDummyTensor(2);
    fusion.addInput(tv0);
    fusion.addInput(tv1);

    // TODO add pointwise ops on the begining before the bcast.

    TensorView* tv2 = broadcast(tv0, {false, false, true});
    TensorView* tv3 = broadcast(tv1, {true, false, false});

    TensorView* tv4 = add(tv2, tv3);

    tv4->merge(0, 1);

    fusion.addOutput(tv4);

    tv0->computeAt(tv4, -1);
    tv1->computeAt(tv4, -1);

    tv4->axis(0)->parallelize(ParallelType::BIDx);

    constexpr int x = 63, y = 33, z = 15;

    auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

    at::Tensor t0 = at::randn({x, y}, options);
    at::Tensor t1 = at::randn({y, z}, options);

    at::Tensor cg_output = at::empty({x, y, z}, options);

    prog.device_ = 0;
    prog.grid(x * y);
    prog.block(1);
    torch::jit::fuser::cuda::compileKernel(&prog);
    torch::jit::fuser::cuda::runTestKernel(&prog, {t0, t1}, {cg_output});

    auto t2 = t0.unsqueeze(-1).expand({x, y, z});
    auto t3 = t1.expand({x, y, z});
    auto t4 = t2.add(t3);

    TORCH_CHECK(t4.allclose(cg_output));
  }
}

void testGPU_FusionSimpleGemm() {
  {
    torch::jit::fuser::cuda::CudaKernel prog;
    Fusion& fusion = *prog.fusion_;
    FusionGuard fg(&fusion);

    // Set up your input tensor views
    TensorView* tv0 = makeDummyTensor(2); // M, K
    TensorView* tv1 = makeDummyTensor(2); // K, N
    fusion.addInput(tv0);
    fusion.addInput(tv1);

    TensorView* tv2 = broadcast(tv0, {false, false, true});
    // tv2[I0, I1, B] = tv0[I0, I1]

    TensorView* tv3 = broadcast(tv1, {true, false, false});
    // tv3[B, I1, I2] = tv1[I1, I2]

    // tv4[I0, I1, I2] = tv2[I0, I1, B] * tv3[B, I1, I2]
    TensorView* tv4 = mul(tv2, tv3);
    // tv5[I0, R1, I2] = tv4[I0, I1, I2]
    TensorView* tv5 = sum(tv4, {1});
    fusion.addOutput(tv5);

    tv5->split(1, 32);
    // tv5[I0, R1o, R1i{32}, I2]

    auto tv6 = tv5->rFactor({1});
    // tv6[I0, R1o, I1i{32}, I2] = tv4[I0, I1, I2]
    // tv5[I0,    , R1i{32}, I2] = tv6[I0, R1o, I1i{32}, I2]

    tv5->split(0, 4);
    tv5->split(-1, 4);
    // tv5[I0o, I0i{4}, R1i{32}, I2o, I2i{4}]
    // tv5[I0o, I0i{4}, R1i{32}, I2o, I2i{4}]

    tv0->computeAt(tv5, -1);
    tv1->computeAt(tv5, -1);

    // tv6[I0o, I0i{4}, R1o, I1i{32}, I2o, I2i{4}]
    // tv5[I0o, I0i{4},    , R1i{32}, I2o, I2i{4}]
    //--> (line symbolizes compute at location)
    // tv4[I0o, I0i{4}, I1i{32}, I2o, I2i{4}|, I1o]
    // tv6[I0o, I0i{4}, I1i{32}, I2o, I2i{4}|, R1o]
    // tv5[I0o, I0i{4}, R1i{32}, I2o, I2i{4}|]

    tv0->computeAt(tv6, -1);
    tv1->computeAt(tv6, -1);
    // tv4[I0o, I0i{4}, I1i{32}, I2o, I2i{4}, I1o |]
    // tv6[I0o, I0i{4}, I1i{32}, I2o, I2i{4}, R1o |]
    // tv5[I0o, I0i{4}, R1i{32}, I2o, I2i{4}|]

    tv5->axis(0)->parallelize(ParallelType::BIDz);
    tv5->axis(1)->parallelize(ParallelType::TIDz);

    tv5->axis(-2)->parallelize(ParallelType::BIDy);
    tv5->axis(-1)->parallelize(ParallelType::TIDy);

    tv5->axis(2)->parallelize(ParallelType::TIDx);
    tv6->axis(2)->parallelize(ParallelType::TIDx);

    constexpr int M = 65, K = 33, N = 17;

    auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

    at::Tensor t0 = at::randn({M, K}, options);
    at::Tensor t1 = at::randn({K, N}, options);

    at::Tensor cg_output = at::empty({M, N}, options);

    prog.device_ = 0;
    prog.grid(1, ceilDiv_(N, 4), ceilDiv_(M, 4));

    prog.block(32, 4, 4);
    torch::jit::fuser::cuda::compileKernel(&prog);
    torch::jit::fuser::cuda::runTestKernel(&prog, {t0, t1}, {cg_output});

    auto t2 = t0.matmul(t1);
    TORCH_CHECK(
        t2.allclose(cg_output, 1e-5, 1e-5),
        "Error of: ",
        t2.sub(cg_output).abs().max());
  }
}

// This test currently requires a combination of broadcast and reduction
// operations and parellelization strategy that is currently not supported.
// It is a goal to get this example working and this test is added so we
// can continue working on getting this example fixed. Right now it
// produces an incorrect result. Either we need to error coherently on the
// optimization strategy we don't support and set this test to one we do support
// or we need to get this schedule working correctly.
void testGPU_FusionSoftmax() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* input_tv0 = makeDummyTensor(3);
  fusion.addInput(input_tv0);

  TensorView* max_val_tv1 =
      reductionOp(BinaryOpType::Max, {2}, new Float(0), input_tv0);
  TensorView* bcast_max_tv2 = broadcast(max_val_tv1, {false, false, true});
  TensorView* exp_tv3 = sub(input_tv0, bcast_max_tv2);
  TensorView* sum_exp_tv4 =
      reductionOp(BinaryOpType::Add, {2}, new Float(0), exp_tv3);
  TensorView* bcast_sum_tv5 = broadcast(sum_exp_tv4, {false, false, true});
  TensorView* output_tv6 = div(exp_tv3, bcast_sum_tv5);

  max_val_tv1->split(-1, 32);
  TensorView* max_val_rf_tv7 = max_val_tv1->rFactor({-2});
  sum_exp_tv4->split(-1, 32);
  TensorView* sum_exp_rf_tv8 = sum_exp_tv4->rFactor({-2});

  exp_tv3->computeAt(sum_exp_rf_tv8, 2);

  max_val_rf_tv7->axis(0)->parallelize(ParallelType::BIDx);
  max_val_tv1->axis(0)->parallelize(ParallelType::BIDx);
  bcast_max_tv2->axis(0)->parallelize(ParallelType::BIDx);
  sum_exp_rf_tv8->axis(0)->parallelize(ParallelType::BIDx);
  sum_exp_tv4->axis(0)->parallelize(ParallelType::BIDx);
  bcast_sum_tv5->axis(0)->parallelize(ParallelType::BIDx);
  output_tv6->axis(0)->parallelize(ParallelType::BIDx);

  max_val_rf_tv7->axis(1)->parallelize(ParallelType::BIDy);
  max_val_tv1->axis(1)->parallelize(ParallelType::BIDy);
  bcast_max_tv2->axis(1)->parallelize(ParallelType::BIDy);
  sum_exp_rf_tv8->axis(1)->parallelize(ParallelType::BIDy);
  sum_exp_tv4->axis(1)->parallelize(ParallelType::BIDy);
  bcast_sum_tv5->axis(1)->parallelize(ParallelType::BIDy);
  output_tv6->axis(1)->parallelize(ParallelType::BIDy);

  max_val_rf_tv7->axis(-1)->parallelize(ParallelType::TIDx);
  max_val_tv1->axis(-1)->parallelize(ParallelType::TIDx);
  bcast_max_tv2->axis(-1)->parallelize(ParallelType::TIDx);
  exp_tv3->axis(-1)->parallelize(ParallelType::TIDx);
  sum_exp_rf_tv8->axis(-1)->parallelize(ParallelType::TIDx);
  sum_exp_tv4->axis(-1)->parallelize(ParallelType::TIDx);
  bcast_sum_tv5->axis(-1)->parallelize(ParallelType::TIDx);
  output_tv6->axis(-1)->parallelize(ParallelType::TIDx);

  fusion.addOutput(output_tv6);

  prog.device_ = 0;
  prog.grid(32, 32);
  prog.block(32);
  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor t0 = at::randn({32, 32, 128}, options);
  at::Tensor cg_output = at::empty({32, 32, 128}, options);
  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {t0}, {cg_output});

  auto t2 = at::_softmax(t0, -1, false);
  // TORCH_CHECK(
  //     t2.allclose(cg_output, 1e-5, 1e-5),
  //     "Error of: ",
  //     t2.sub(cg_output).abs().max());
}
// Similar to FusionReduction but uses grid reduction
void testGPU_FusionGridReduction1() {
  const int gdimx = 32;
  const int bdimx = 128;
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);

  // tv1[I0, R1] = tv0[I0, I1]
  TensorView* tv1 = reductionOp(BinaryOpType::Add, {1}, new Float(0), tv0);
  fusion.addOutput(tv1);

  TORCH_CHECK(fusion.hasReduction(), "Could not detect reduction in fusion.");

  tv1->split(1, bdimx);
  // tv1[I0, R1o, R1i{128}] = tv0[I0, I1]
  tv1->split(1, gdimx);
  // tv1[I0, R1oo, R1oi{32}, R1i{128}] = tv0[I0, I1]

  TensorView* tv2 = tv1->rFactor({1});
  // tv2[I0, R1oo, Ir1oi{32}, Ir1i{128}] = tv0[I0, I1]
  // tv1[I0,        R1oi{32},  R1i{128}] = tv2[I0, R1oo, Ir1oi{32}, Ir1i{128}]

  // Incrementally, can print in between for debugging
  tv0->computeAt(tv2, 1);
  tv2->computeAt(tv1, 1);

  // Re do it all at once, because why not.
  tv0->computeAt(tv1, 1);

  tv1->axis(0)->parallelize(ParallelType::BIDy);
  tv1->axis(1)->parallelize(ParallelType::BIDx);
  tv2->axis(2)->parallelize(ParallelType::BIDx);

  tv1->axis(-1)->parallelize(ParallelType::TIDx);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);

  int numel_x = 10000;
  int numel_y = 65000;

  prog.device_ = 0;
  prog.grid(gdimx, numel_x);
  prog.block(bdimx);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::rand({numel_x, numel_y}, options);
  at::Tensor cg_output = at::empty({numel_x}, options);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

  auto aten_output = input.sum({1});
  TORCH_CHECK(aten_output.allclose(cg_output));
}

// Same test as the above but uses BIDy and TIDx for reduction
void testGPU_FusionGridReduction2() {
  const int gdimy = 32;
  const int bdimx = 128;
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);

  // tv1[I0, R1] = tv0[I0, I1]
  TensorView* tv1 = reductionOp(BinaryOpType::Add, {1}, new Float(0), tv0);
  fusion.addOutput(tv1);

  TORCH_CHECK(fusion.hasReduction(), "Could not detect reduction in fusion.");

  tv1->split(1, bdimx);
  // tv1[I0, R1o, R1i{128}] = tv0[I0, I1]
  tv1->split(1, gdimy);
  // tv1[I0, R1oo, R1oi{32}, R1i{128}] = tv0[I0, I1]

  TensorView* tv2 = tv1->rFactor({1});
  // tv2[I0, R1oo, Ir1oi{32}, Ir1i{128}] = tv0[I0, I1]
  // tv1[I0,        R1oi{32},  R1i{128}] = tv2[I0, R1oo, Ir1oi{32}, Ir1i{128}]

  // Incrementally, can print in between for debugging
  tv0->computeAt(tv2, 1);
  tv2->computeAt(tv1, 1);

  // Re do it all at once, because why not.
  tv0->computeAt(tv1, 1);

  tv1->axis(0)->parallelize(ParallelType::BIDx);
  tv1->axis(1)->parallelize(ParallelType::BIDy);
  tv2->axis(2)->parallelize(ParallelType::BIDy);

  tv1->axis(-1)->parallelize(ParallelType::TIDx);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);

  int numel_x = 10000;
  int numel_y = 65000;

  prog.device_ = 0;
  prog.grid(numel_x, gdimy);
  prog.block(bdimx);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::rand({numel_x, numel_y}, options);
  at::Tensor cg_output = at::empty({numel_x}, options);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

  auto aten_output = input.sum({1});
  TORCH_CHECK(aten_output.allclose(cg_output));
}

// Same test but uses BIDy and BIDz for reduction. No TID used.
void testGPU_FusionGridReduction3dim1() {
  const int gdimz = 32;
  const int gdimy = 128;
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);

  // tv1[I0, R1] = tv0[I0, I1]
  TensorView* tv1 = reductionOp(BinaryOpType::Add, {1}, new Float(0), tv0);
  fusion.addOutput(tv1);

  TORCH_CHECK(fusion.hasReduction(), "Could not detect reduction in fusion.");

  tv1->split(1, gdimy);
  // tv1[I0, R1o, R1i{128}] = tv0[I0, I1]
  tv1->split(1, gdimz);
  // tv1[I0, R1oo, R1oi{32}, R1i{128}] = tv0[I0, I1]

  TensorView* tv2 = tv1->rFactor({1});
  // tv2[I0, R1oo, Ir1oi{32}, Ir1i{128}] = tv0[I0, I1]
  // tv1[I0,        R1oi{32},  R1i{128}] = tv2[I0, R1oo, Ir1oi{32}, Ir1i{128}]

  // Incrementally, can print in between for debugging
  tv0->computeAt(tv2, 1);
  tv2->computeAt(tv1, 1);

  // Re do it all at once, because why not.
  tv0->computeAt(tv1, 1);

  tv1->axis(0)->parallelize(ParallelType::BIDx);
  tv1->axis(1)->parallelize(ParallelType::BIDz);
  tv2->axis(2)->parallelize(ParallelType::BIDz);

  tv1->axis(-1)->parallelize(ParallelType::BIDy);
  tv2->axis(-1)->parallelize(ParallelType::BIDy);

  int numel_x = 100;
  int numel_y = 6500;

  prog.device_ = 0;
  prog.grid(numel_x, gdimy, gdimz);
  // This number should not affect the output as TIDx is not
  // used. All threads in a thread block redundantly computes the
  // same value.
  prog.block(128);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::rand({numel_x, numel_y}, options);
  at::Tensor cg_output = at::empty({numel_x}, options);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

  auto aten_output = input.sum({1});
  TORCH_CHECK(aten_output.allclose(cg_output));
}

// Same as testGPU_FusionGridReduction3dim1 but reduces dimension 0
void testGPU_FusionGridReduction3dim0() {
  const int rdim = 0;
  const int gdimy = 128;
  const int gdimz = 32;
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);

  // tv1[R0, I1] = tv0[I0, I1]
  TensorView* tv1 = reductionOp(BinaryOpType::Add, {rdim}, new Float(0), tv0);
  fusion.addOutput(tv1);

  TORCH_CHECK(fusion.hasReduction(), "Could not detect reduction in fusion.");

  tv1->split(rdim, gdimy);
  // tv1[R0o, R0i{128}, I1] = tv0[I0, I1]
  tv1->split(rdim, gdimz);
  // tv1[R0oo, R0oi{32}, R0i{128}, I1] = tv0[I0, I1]

  TensorView* tv2 = tv1->rFactor({rdim});
  // tv2[R0oo, I0oi{32}, I0i{128}, I1] = tv0[I0, I1]
  // tv1[      R0oi{32}, R0i{128}, I1] = tv2[R0oo, I0oi{32}, I0i{128}, I1]

  // Note that computeAt isn't going to make anything better as there
  // is no dynamically sized dimension.

  // Map parallelism as [Serial, BIDz, BIDy, BIDx]
  tv1->axis(-1)->parallelize(ParallelType::BIDx);
  tv2->axis(-1)->parallelize(ParallelType::BIDx);
  tv1->axis(-2)->parallelize(ParallelType::BIDy);
  tv2->axis(-2)->parallelize(ParallelType::BIDy);
  tv1->axis(-3)->parallelize(ParallelType::BIDz);
  tv2->axis(-3)->parallelize(ParallelType::BIDz);

  int numel_x = 6500;
  int numel_y = 100;

  prog.device_ = 0;
  prog.grid(numel_y, gdimy, gdimz);
  // This number should not affect the output as TIDx is not
  // used. All threads in a thread block redundantly computes the
  // same value.
  prog.block(1);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::rand({numel_x, numel_y}, options);
  at::Tensor cg_output = at::empty({numel_y}, options);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

  auto aten_output = input.sum({0});
  TORCH_CHECK(aten_output.allclose(cg_output));
}

// This is similar to the FusionReduction, but swaps BIDx and TIDx
void testGPU_FusionGridReduction4() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  const int bdimx = 128;
  const int gdimx = 1024;

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);

  // tv1[I0, R1] = tv0[I0, I1]
  TensorView* tv1 = reductionOp(BinaryOpType::Add, {1}, new Float(0), tv0);
  fusion.addOutput(tv1);

  TORCH_CHECK(fusion.hasReduction(), "Could not detect reduction in fusion.");

  tv1->split(1, gdimx);
  // tv1[I0, R1o, R1i{1024}] = tv0[I0, I1]
  tv1->split(1, 4);
  // tv1[I0, R1oo, R1oi{4}, R1i{128}] = tv0[I0, I1]

  TensorView* tv2 = tv1->rFactor({1});
  // tv2[I0, R1oo, Ir1oi{4}, Ir1i{1024}] = tv0[I0, I1]
  // tv1[I0,        R1oi{4},  R1i{1024}] = tv2[I0, R1oo, Ir1oi{4}, Ir1i{1024}]

  TensorView* tv3 = tv1->rFactor({1});
  // tv2[I0, R1oo, Ir1oi{4}, Ir1i{1024}] = tv0[I0, I1]
  // tv3[I0,        R1oi{4}, Ir1i{1024}] = tv2[I0, R1oo, Ir1oi{4}, Ir1i{1024}]
  // tv1[I0,                  R1i{1024}] = tv3[I0,        R1oi{4}, Ir1i{1024}]

  // Incrementally, can print in between for debugging
  tv0->computeAt(tv2, 1);
  tv2->computeAt(tv3, 1);
  tv3->computeAt(tv1, 1);

  // Re do it all at once, because why not.
  tv0->computeAt(tv1, 1);

  tv2->axis(2)->parallelize(ParallelType::Unroll);
  tv1->axis(0)->parallelize(ParallelType::TIDx);

  tv1->axis(-1)->parallelize(ParallelType::BIDx);
  tv2->axis(-1)->parallelize(ParallelType::BIDx);
  tv3->axis(-1)->parallelize(ParallelType::BIDx);

  int numel_x = bdimx;
  int numel_y = 65000;

  prog.device_ = 0;
  prog.grid(gdimx);
  prog.block(bdimx);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::rand({numel_x, numel_y}, options);
  at::Tensor cg_output = at::empty({numel_x}, options);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

  auto aten_output = input.sum({1});
  TORCH_CHECK(aten_output.allclose(cg_output));
}

// Grid reduction with 2D thread blocks but only TIDx and BIDx are
// mapped to a reduction dim
void testGPU_FusionGridReduction5() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  const int bdimx = 64;
  const int bdimy = 16;
  const int gdimx = 4;

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);

  // tv1[I0, R1] = tv0[I0, I1]
  TensorView* tv1 = reductionOp(BinaryOpType::Add, {1}, new Float(0), tv0);
  fusion.addOutput(tv1);

  TORCH_CHECK(fusion.hasReduction(), "Could not detect reduction in fusion.");

  tv1->split(1, bdimx);
  // tv1[I0, R1o, R1i{64}] = tv0[I0, I1]
  tv1->split(1, gdimx);
  // tv1[I0, R1oo, R1oi{4}, R1i{64}] = tv0[I0, I1]

  TensorView* tv2 = tv1->rFactor({1});
  // tv2[I0, R1oo, Ir1oi{4}, Ir1i{64}] = tv0[I0, I1]
  // tv1[I0,        R1oi{4},  R1i{64}] = tv2[I0, R1oo, Ir1oi{4}, Ir1i{64}]

  tv0->computeAt(tv1, 1);

  tv1->axis(-1)->parallelize(ParallelType::TIDx);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);

  tv1->axis(-2)->parallelize(ParallelType::BIDx);
  tv2->axis(-2)->parallelize(ParallelType::BIDx);

  tv1->axis(0)->parallelize(ParallelType::TIDy);

  int numel_x = bdimy;
  int numel_y = 6500;

  prog.device_ = 0;
  prog.grid(gdimx);
  prog.block(bdimx, bdimy);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::rand({numel_x, numel_y}, options);
  at::Tensor cg_output = at::empty({numel_x}, options);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

  auto aten_output = input.sum({1});
  TORCH_CHECK(aten_output.allclose(cg_output));
}

// Similar to FusionGridReduction1 but with 3D tensors
void testGPU_FusionGridReduction6() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(3);
  fusion.addInput(tv0);

  // tv1[I0, R1, R2] = tv0[I0, I1, I2]
  TensorView* tv1 = reductionOp(BinaryOpType::Add, {1, 2}, new Float(0), tv0);
  fusion.addOutput(tv1);

  TORCH_CHECK(fusion.hasReduction(), "Could not detect reduction in fusion.");

  // Splitting for TID
  tv1->split(2, 128);
  // tv1[I0, R1, R2o, R2i{128}] = tv0[I0, I1, I2]

  // Splitting for BID
  tv1->split(1, 128);

  // tv1[I0, R1o, R1i{128}, R2o, R2i{128}] = tv0[I0, I1, I2]

  TensorView* tv2 = tv1->rFactor({3});
  // tv2[I0, I1o, I1i{128}, R2o, I2i{128}]
  // tv1[I0, R1o, R1i{128},      R2i{128}]

  TensorView* tv3 = tv1->rFactor({1});
  // tv2[I0, I1o, I1i{128}, R2o, I2i{128}]
  // tv3[I0, R1o, I1i{128},      I2i{128}]
  // tv1[I0,      R1i{128},      R2i{128}]

  tv3->computeAt(tv1, 1);
  tv2->computeAt(tv3, 3);

  tv1->axis(0)->parallelize(ParallelType::BIDy);

  tv1->axis(-1)->parallelize(ParallelType::TIDx);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  tv1->axis(-2)->parallelize(ParallelType::BIDx);
  tv2->axis(-3)->parallelize(ParallelType::BIDx);
  tv3->axis(-2)->parallelize(ParallelType::BIDx);

  int numel_x = 6500;
  int numel_y = 200;
  int numel_z = numel_y;

  prog.device_ = 0;
  prog.grid(128, numel_x);
  prog.block(128);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::rand({numel_x, numel_y, numel_z}, options);
  at::Tensor cg_output = at::empty({numel_x}, options);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

  auto aten_output = input.sum({1, 2});
  TORCH_CHECK(aten_output.allclose(cg_output));
}

void testGPU_FusionNonRedAxisBind() {
  int bid_x = 3;
  int tid_x = 2;
  int red_dim = 0;

  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);

  TensorView* tv1 =
      reductionOp(BinaryOpType::Add, {red_dim}, new Float(0), tv0);
  fusion.addOutput(tv1);

  tv1->split(-1, tid_x);
  tv1->axis(-2)->parallelize(ParallelType::BIDx);
  tv1->axis(-1)->parallelize(ParallelType::TIDx);

  prog.device_ = 0;
  prog.grid(bid_x);
  prog.block(tid_x);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::rand({16, bid_x * tid_x}, options);
  at::Tensor cg_output = at::empty({bid_x * tid_x}, options);

  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {cg_output});

  auto aten_output = input.sum({red_dim});

  TORCH_CHECK(
      aten_output.allclose(cg_output),
      "Error of: ",
      aten_output.sub(cg_output).abs().max());
}

void testGPU_FusionSplitBCast() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* input_tv0 = makeDummyTensor(3);
  TensorView* input_tv1 = makeDummyTensor(3);
  fusion.addInput(input_tv0);
  fusion.addInput(input_tv1);

  TensorView* sum_tv2 =
      reductionOp(BinaryOpType::Add, {2}, new Float(0), input_tv0);
  TensorView* bcast_tv3 = broadcast(sum_tv2, {false, false, true});
  TensorView* output_tv4 = div(input_tv1, bcast_tv3);

  sum_tv2->split(-1, 32);
  TensorView* sum_rf_tv5 = sum_tv2->rFactor({-2});

  bcast_tv3->split(-1, 32);
  output_tv4->split(-1, 32);

  sum_rf_tv5->axis(0)->parallelize(ParallelType::BIDx);
  sum_tv2->axis(0)->parallelize(ParallelType::BIDx);
  bcast_tv3->axis(0)->parallelize(ParallelType::BIDx);
  output_tv4->axis(0)->parallelize(ParallelType::BIDx);

  sum_rf_tv5->axis(1)->parallelize(ParallelType::BIDy);
  sum_tv2->axis(1)->parallelize(ParallelType::BIDy);
  bcast_tv3->axis(1)->parallelize(ParallelType::BIDy);
  output_tv4->axis(1)->parallelize(ParallelType::BIDy);

  sum_rf_tv5->axis(-1)->parallelize(ParallelType::TIDx);
  sum_tv2->axis(-1)->parallelize(ParallelType::TIDx);
  bcast_tv3->axis(-1)->parallelize(ParallelType::TIDx);
  output_tv4->axis(-1)->parallelize(ParallelType::TIDx);

  fusion.addOutput(output_tv4);

  prog.device_ = 0;
  prog.grid(32, 32);
  prog.block(32);
  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor t0 = at::randn({32, 32, 128}, options);
  at::Tensor t1 = at::randn({32, 32, 128}, options);
  at::Tensor cg_output = at::empty({32, 32, 128}, options);
  torch::jit::fuser::cuda::compileKernel(&prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {t0, t1}, {cg_output});
}

void testGPU_FusionBCastInnerDim() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);

  // reduce then broadcast
  auto tv1 = sum(tv0, {0});
  auto tv2 = broadcast(tv1, {false, true});

  TORCH_CHECK(!tv2->axis(0)->isReduction() && tv2->axis(1)->isBroadcast());
}

void testGPU_FusionBCastReduce() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(2);

  auto tv1 = broadcast(tv0, {true, false, false});
  auto tv2 = sum(tv1, {1});
  TORCH_CHECK(
      tv2->axis(0)->isBroadcast() && tv2->axis(1)->isReduction() &&
      !tv2->axis(2)->isBroadcast() && !tv2->axis(2)->isReduction());
}

// Multiple consumer reduction with computeAt
// https://github.com/csarofeen/pytorch/issues/110
void testGPU_FusionReductionMultiConsumer() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);
  TensorView* tv0 = makeDummyTensor(2);
  fusion.addInput(tv0);
  auto tv1 = unaryOp(UnaryOpType::Exp, tv0);
  auto tv2 = reductionOp(BinaryOpType::Max, {-1}, new Float(0), tv1);
  auto tv3 = reductionOp(BinaryOpType::Min, {-1}, new Float(0), tv1);
  auto tv4 = add(tv2, tv3);
  fusion.addOutput(tv4);
  tv1->computeAt(tv2, -1);

  TORCH_CHECK(
      (tv1->getComputeAtView() == tv2 || tv1->getComputeAtView() == tv3) &&
      tv1->getThisComputeAtAxis() == 2 && tv1->getRelativeComputeAtAxis() == 2);
}

void testGPU_FusionComputeAtExprOrder() {
  {
    for (int i = 0; i < 2; ++i) {
      torch::jit::fuser::cuda::CudaKernel prog;
      Fusion& fusion = *prog.fusion_;
      FusionGuard fg(&fusion);

      // Set up your input tensor views
      TensorView* tv0 = makeDummyTensor(1);
      fusion.addInput(tv0);

      auto tv1 = add(tv0, new Float(1));
      auto tv2 = add(tv0, new Float(1));
      TensorView* tv3 = add(tv1, tv2);
      if (i == 0) {
        tv1->computeAt(tv3, -1);
        fusion.addOutput(tv2);
      } else {
        tv2->computeAt(tv3, -1);
        fusion.addOutput(tv1);
      }
      fusion.addOutput(tv3);

      prog.device_ = 0;
      prog.grid(1);
      prog.block(1);

      torch::jit::fuser::cuda::compileKernel(&prog);

      auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
      at::Tensor input = at::rand({100}, options);
      at::Tensor output2 = at::empty_like(input, options);
      at::Tensor output3 = at::empty_like(input, options);
      torch::jit::fuser::cuda::runTestKernel(
          &prog, {input}, {output2, output3});
      auto aten_output = (input + 1) * 2;
      TORCH_CHECK(
          aten_output.allclose(output3),
          "Error of: ",
          aten_output.sub(output3).abs().max());
    }
  }
  {
    torch::jit::fuser::cuda::CudaKernel prog;
    Fusion& fusion = *prog.fusion_;
    FusionGuard fg(&fusion);

    // Set up your input tensor views
    TensorView* tv0 = makeDummyTensor(2);
    fusion.addInput(tv0);

    auto tv1 = add(tv0, new Float(1));
    auto tv2 = add(tv0, new Float(1));
    TensorView* tv3 = add(tv1, tv2);
    fusion.addOutput(tv3);

    tv3->split(-1, 32);

    tv1->computeAt(tv3, -1);
    tv2->computeAt(tv3, -2);

    prog.device_ = 0;
    prog.grid(1);
    prog.block(1);

    torch::jit::fuser::cuda::compileKernel(&prog);

    auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
    at::Tensor input = at::rand({100, 100}, options);
    at::Tensor output = at::empty_like(input, options);
    torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {output});
    auto aten_output = (input + 1) * 2;
    TORCH_CHECK(
        aten_output.allclose(output),
        "Error of: ",
        aten_output.sub(output).abs().max());
  }
}

void testGPU_FusionZeroDimComputeAt() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeDummyTensor(1);
  fusion.addInput(tv0);

  auto tv1 = sum(tv0, {0});
  auto tv2 = add(tv1, new Float(1));
  fusion.addOutput(tv2);
  TORCH_CHECK(tv2->nDims() == 0);
  tv1->computeAt(tv2, 0);

  prog.device_ = 0;
  prog.grid(1);
  prog.block(1);

  torch::jit::fuser::cuda::compileKernel(&prog);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::rand({100}, options);
  at::Tensor output = at::empty({}, options);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {output});
  auto aten_output = input.sum() + 1;
  TORCH_CHECK(
      aten_output.allclose(output),
      "Error of: ",
      aten_output.sub(output).abs().max());
}

void testGPU_FusionZeroDimBroadcast() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeDummyTensor(0);
  fusion.addInput(tv0);

  auto tv1 = broadcast(tv0, {true, true});
  TORCH_CHECK(tv1->nDims() == 2);

  TensorView* tv2 = makeDummyTensor(2);
  fusion.addInput(tv2);

  auto tv3 = add(tv1, tv2);
  auto tv4 = sum(tv3, {0, 1});
  fusion.addOutput(tv4);

  tv3->computeAt(tv4, -1);

  prog.device_ = 0;
  prog.grid(1);
  prog.block(1);

  torch::jit::fuser::cuda::compileKernel(&prog);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input1 = at::rand({}, options);
  at::Tensor input2 = at::rand({10, 10}, options);
  at::Tensor output = at::empty({}, options);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input1, input2}, {output});
  auto aten_output =
      (input1.unsqueeze(-1).unsqueeze(-1).expand({10, 10}) + input2).sum();
  TORCH_CHECK(
      aten_output.allclose(output),
      "Error of: ",
      aten_output.sub(output).abs().max());
}

void testGPU_FusionZeroDimReduction() {
  torch::jit::fuser::cuda::CudaKernel prog;
  Fusion& fusion = *prog.fusion_;
  FusionGuard fg(&fusion);

  const int bdimx = 32;
  const int gdimx = 32;

  TensorView* tv0 = makeDummyTensor(1);
  fusion.addInput(tv0);

  auto tv1 = sum(tv0, {0});
  fusion.addOutput(tv1);

  tv1->split(0, bdimx);
  tv1->split(0, gdimx);
  auto tv2 = tv1->rFactor({0});

  tv1->axis(-1)->parallelize(ParallelType::TIDx);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);
  tv1->axis(-2)->parallelize(ParallelType::BIDx);
  tv2->axis(-2)->parallelize(ParallelType::BIDx);

  prog.device_ = 0;
  prog.grid(gdimx);
  prog.block(bdimx);

  torch::jit::fuser::cuda::compileKernel(&prog);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor input = at::rand({1000}, options);
  at::Tensor output = at::empty({}, options);
  torch::jit::fuser::cuda::runTestKernel(&prog, {input}, {output});
  auto aten_output = input.sum();
  TORCH_CHECK(
      aten_output.allclose(output),
      "Error of: ",
      aten_output.sub(output).abs().max());
}

} // namespace jit
} // namespace torch
#endif // #if defined(USE_CUDA)
