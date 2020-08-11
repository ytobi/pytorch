
#include <torch/csrc/jit/codegen/cuda/fusion.h>
#include <torch/csrc/jit/codegen/cuda/ir_all_nodes.h>
#include <torch/csrc/jit/codegen/cuda/ir_cloner.h>
#include <torch/csrc/jit/codegen/cuda/ir_printer.h>
#include <torch/csrc/jit/codegen/cuda/kernel.h>
#include <torch/csrc/jit/codegen/cuda/lower2device.h>

namespace torch {
namespace jit {
namespace fuser {

static thread_local Fusion* ACTIVE_FUSION = nullptr;

FusionGuard::FusionGuard(Fusion* fusion) {
  prev_fusion = ACTIVE_FUSION;
  ACTIVE_FUSION = fusion;
}

FusionGuard::FusionGuard(const cuda::CudaKernel* cuda_kernel) {
  prev_fusion = ACTIVE_FUSION;
  ACTIVE_FUSION = cuda_kernel->fusion_.get();
}

FusionGuard::~FusionGuard() {
  ACTIVE_FUSION = prev_fusion;
}

Fusion* FusionGuard::getCurFusion() {
  return ACTIVE_FUSION;
}

void ExprSort::handle(Expr* expr) {
  exprs.push_back(expr);
}

std::vector<Expr*> ExprSort::getExprs(
    Fusion* fusion,
    bool from_outputs_only,
    bool breadth_first,
    bool respect_compute_at) {
  ExprSort es;
  es.traverse(fusion, from_outputs_only, breadth_first, respect_compute_at);
  return es.exprs;
}

void InputsOf::handle(Val* v) {
  if (FusionGuard::getCurFusion()->origin(v) == nullptr)
    inputs.emplace(v);
}

std::unordered_set<Val*> InputsOf::output(Fusion* fusion, Val* output_) {
  TORCH_CHECK(
      fusion->hasOutput(output_),
      "Asked for the inputs of ",
      output_,
      " however, it is not an output of the provided fusion.");
  InputsOf io;
  io.traverseFrom(FusionGuard::getCurFusion(), {output_}, false);
  return io.inputs;
}

void swap(Fusion& a, Fusion& b) noexcept {
  using std::swap;

  // Swap the content
  swap(a.val_set_, b.val_set_);
  swap(a.expr_set_, b.expr_set_);
  swap(a.val_deque_, b.val_deque_);

  swap(a.val_type_name_map_, b.val_type_name_map_);
  swap(a.val_name_counter_, b.val_name_counter_);
  swap(a.expr_name_counter_, b.expr_name_counter_);

  swap(a.origin_, b.origin_);
  swap(a.uses_, b.uses_);
  swap(a.values_map_, b.values_map_);

  swap(a.inputs_, b.inputs_);
  swap(a.outputs_, b.outputs_);

  // Fixup the Statement::fusion_ links for a
  for (auto val : a.val_set_) {
    val->fusion_ = &a;
  }
  for (auto expr : a.expr_set_) {
    expr->fusion_ = &a;
  }

  // Fixup the Statement::fusion_ links for b
  for (auto val : b.val_set_) {
    val->fusion_ = &b;
  }
  for (auto expr : b.expr_set_) {
    expr->fusion_ = &b;
  }
}

Fusion::Fusion(const Fusion& other) {
  IrCloner ir_cloner(this);

  for (auto val : other.val_set_) {
    val_set_.insert(ir_cloner.clone(val));
  }

  for (auto expr : other.expr_set_) {
    expr_set_.insert(ir_cloner.clone(expr));
  }

  for (auto val : other.val_deque_) {
    val_deque_.push_back(ir_cloner.clone(val));
  }

  val_type_name_map_ = other.val_type_name_map_;
  val_name_counter_ = other.val_name_counter_;
  expr_name_counter_ = other.expr_name_counter_;

  for (const auto& kv : other.origin_) {
    auto val = ir_cloner.clone(kv.first);
    auto expr = ir_cloner.clone(kv.second);
    origin_.insert({val, expr});
  }

  for (const auto& kv : other.uses_) {
    auto val = ir_cloner.clone(kv.first);
    std::unordered_set<Expr*> val_uses;
    for (auto expr : kv.second) {
      val_uses.insert(ir_cloner.clone(expr));
    }
    uses_.insert({val, std::move(val_uses)});
  }

  for (const auto& kv : other.values_map_) {
    auto from_val = ir_cloner.clone(kv.first);
    auto to_val = ir_cloner.clone(kv.second);
    values_map_.insert({from_val, to_val});
  }

  inputs_ = ir_cloner.clone(other.inputs_);
  outputs_ = ir_cloner.clone(other.outputs_);
}

Fusion::Fusion(Fusion&& other) noexcept {
  swap(*this, other);
}

Fusion& Fusion::operator=(const Fusion& other) {
  Fusion copy(other);
  clear();
  swap(*this, copy);
  return *this;
}

Fusion& Fusion::operator=(Fusion&& other) noexcept {
  clear();
  swap(*this, other);
  return *this;
}

Fusion::~Fusion() {
  clear();
}

void Fusion::clear() noexcept {
  // Free the owned values
  for (auto ptr : val_set_) {
    delete ptr;
  }

  // Free the owned expressions
  for (auto ptr : expr_set_) {
    delete ptr;
  }

  val_set_.clear();
  val_deque_.clear();
  expr_set_.clear();

  for (auto& kv : val_type_name_map_) {
    kv.second = 0;
  }

  val_name_counter_ = 0;
  expr_name_counter_ = 0;

  origin_.clear();
  uses_.clear();
  values_map_.clear();

  inputs_.clear();
  outputs_.clear();
}

void Fusion::removeExpr(Expr* expr) {
  assertInFusion(expr, "Cannot remove expr ");
  // If we hit this error too frequently, we could lighten the restrictions so
  // that removing something that doesn't exist simply does nothing. For now,
  // we're going with the strictest model which errors.

  for (auto out : expr->outputs())
    if (origin_.find(out) != origin_.end())
      if (origin_.find(out)->second == expr)
        origin_.erase(out);

  for (auto inp : expr->inputs()) {
    if (uses_.find(inp) != uses_.end()) {
      if (uses_.find(inp)->second.find(expr) != uses_.find(inp)->second.end()) {
        uses_.find(inp)->second.erase(expr);
      }
    }
  }

  expr_set_.erase(expr);

  delete expr;
}

void Fusion::removeVal(Val* val) {
  assertInFusion(val, "Cannot remove val ");

  for (Val* inp : inputs())
    if (val->sameAs(inp))
      TORCH_CHECK(false, "Cannot remove val as it is an input of the fusion.");

  for (Val* out : outputs())
    if (val->sameAs(out))
      TORCH_CHECK(false, "Cannot remove val as it is an output of the fusion.");

  Expr* orig = origin(val);
  if (orig != nullptr)
    removeExpr(origin(val));

  for (Expr* use : unordered_uses(val))
    removeExpr(use);

  val_set_.erase(val);

  for (auto it = val_deque_.begin(); it != val_deque_.end(); it++)
    if (*it == val) {
      val_deque_.erase(it);
      break;
    }

  delete val;
}

void Fusion::addInput(Val* const input) {
  assertInFusion(input, "Cannot register input ");

  if (input->getValType().value() == ValType::TensorView) {
    auto tv = input->as<TensorView>();
    if (tv->hasReduction())
      TORCH_WARN_ONCE(
          "Registered input ",
          input,
          " has a reduction axis, but this does nothing in the fusion.");
  }

  TORCH_CHECK(
      input->getOrigin() == nullptr,
      input,
      " cannot be registered as an input as it is used as an output of an expression (",
      input->getOrigin(),
      ").");

  inputs_.push_back(input);
}

void Fusion::addOutput(Val* const output) {
  assertInFusion(output, "Cannot register output ");
  if (output->getValType().value() == ValType::TensorView) {
    auto tv = output->as<TensorView>();
    if (TensorDomain::hasBroadcast(tv->getRootDomain()))
      // Go to the root as we can merge bcast and
      // non-bcast dims, making a non-bcast dim.
      TORCH_CHECK( // Should we warn instead?
          false,
          output,
          " cannot be registered as an output as it has a broadcast axis.");
  }
  outputs_.push_back(output);
}

bool Fusion::inFusion(const Statement* stmt) const {
  bool infusion = stmt->fusion() == this;
  Statement* nonconst_stmt = const_cast<Statement*>(stmt);

  if (stmt->isExpr())
    infusion &=
        expr_set_.find(static_cast<Expr*>(nonconst_stmt)) != expr_set_.end();
  if (stmt->isVal())
    infusion &=
        val_set_.find(static_cast<Val*>(nonconst_stmt)) != val_set_.end();

  return infusion;
}

void Fusion::assertInFusion(const Statement* stmt, const std::string& msg)
    const {
  if (inFusion(stmt))
    return;
  TORCH_CHECK(false, msg, " it was not found in the active fusion.");
}

std::vector<Expr*> Fusion::exprs(
    bool from_outputs_only,
    bool breadth_first,
    bool respect_compute_at) {
  if (breadth_first)
    TORCH_INTERNAL_ASSERT(false, "Not implemented yet.");
  return ExprSort::getExprs(
      this, from_outputs_only, breadth_first, respect_compute_at);
}

std::unordered_set<Val*> Fusion::inputsOf(Val* val) {
  return InputsOf::output(this, val);
}

void Fusion::validateInputs() {
  std::unordered_set<Val*> all_inputs;
  for (Val* out : outputs()) {
    for (Val* input : inputsOf(out)) {
      all_inputs.insert(input);
    }
  }
  for (Val* input : all_inputs) {
    if (!input->isConstScalar())
      TORCH_CHECK(
          hasInput(input),
          "Could not figure out how ",
          input,
          " is generated, however it was not specified as an input.");
  }
}

void Fusion::print() {
  FusionGuard fg(this);
  std::cout << "%kernel {\n";
  IRMathPrinter op_exprs(std::cout);
  op_exprs.handle(this);
  IRTransformPrinter t_exprs(std::cout);
  t_exprs.handle(this);
  std::cout << "}\n";
}

void Fusion::printValuesMap() {
  IRPrinter ir_printer(std::cout);
  ir_printer.follow_val_map = false;
  std::cout << "\nValues map\n";
  std::cout << "--------------------\n";
  for (const auto& kv : values_map_) {
    ir_printer.handle(kv.first);
    std::cout << " -> ";
    ir_printer.handle(kv.second);
    std::cout << "\n";
  }
  std::cout << "--------------------\n\n";
}

void Fusion::printKernel() {
  FusionGuard fg(this);
  GPULower lower(this);
  lower.printKernel(std::cout);
}

void Fusion::printMath() {
  FusionGuard fg(this);
  for (auto expr : exprs(true))
    std::cout << expr;
}

void Fusion::printTransforms() {
  FusionGuard fg(this);
  IRTransformPrinter t_exprs(std::cout);
  t_exprs.handle(this);
}

StmtNameType Fusion::registerVal(Val* val) {
  if (val->fusion()) {
    if (val->fusion() != this) {
      TORCH_CHECK(false, val, " was not found in the active fusion.");
    }
    if (inFusion(val)) {
      return val->name();
    }
  }
  val_set_.emplace(val);
  val_deque_.push_back(val);
  return getValName(*(val->getValType()));
}

StmtNameType Fusion::registerExpr(Expr* expr) {
  if (expr->fusion()) {
    if (expr->fusion() != this) {
      TORCH_CHECK(false, expr, " was not found in the active fusion.");
    }
    if (inFusion(expr)) {
      return expr->name();
    }
  }

  for (Val* input : expr->inputs()) {
    assertInFusion(input, "Input to expr is invalid, ");
    if (uses_.find(input) == uses_.end()) {
      uses_[input] = {expr};
    } else {
      uses_.find(input)->second.emplace(expr);
    }
  }

  for (Val* output : expr->outputs()) {
    assertInFusion(output, "Output to expr is invalid, ");
    auto it = origin_.find(output);
    if (it != origin_.end()) {
      removeExpr(it->second); // will also remove origin entry
    }

    origin_[output] = expr;
  }

  expr_set_.emplace(expr);
  return getExprName();
}

StmtNameType Fusion::registerStatement(Statement* stmt) {
  if (inFusion(stmt))
    return stmt->name();

  if (stmt->isVal()) {
    return registerVal(static_cast<Val*>(stmt));
  } else if (stmt->isExpr()) {
    return registerExpr(static_cast<Expr*>(stmt));
  }

  TORCH_INTERNAL_ASSERT(
      false,
      "Could not register statement as Fusion could not recognize its type.");
  return UNINITIALIZED_STMTNAMETYPE;
}

bool Fusion::used(Val* val) const {
  assertInFusion(val, "Cannot detect if val was used, ");
  return (uses_.find(val) != uses_.end()) &&
      (uses_.find(val)->second.size() > 0);
}

const std::unordered_set<Val*>& Fusion::vals() const noexcept {
  return val_set_;
}

const std::deque<Val*>& Fusion::deterministic_vals() const noexcept {
  return val_deque_;
}

const std::unordered_set<Expr*>& Fusion::unordered_exprs() const noexcept {
  return expr_set_;
}

std::unordered_set<Expr*> Fusion::unordered_uses(Val* val) const {
  assertInFusion(val, "Cannot detect where val was used, ");
  if (uses_.find(val) != uses_.end()) {
    auto ret = uses_.find(val)->second;
    return ret;
  }
  return std::unordered_set<Expr*>();
}

Expr* Fusion::origin(Val* val) const {
  assertInFusion(val, "Cannot dettect the origin of val, ");
  auto it = origin_.find(val);

  if (it == origin_.end())
    return nullptr;

  return it->second;
}

const Expr* Fusion::origin(const Val* val) const {
  assertInFusion(val, "Cannot dettect the origin of val, ");
  auto it = origin_.find(const_cast<Val*>(val));
  if (it == origin_.end())
    return nullptr;
  return it->second;
}

bool Fusion::hasInput(const Val* val) const {
  return std::find(inputs_.begin(), inputs_.end(), val) != inputs_.end();
}

bool Fusion::hasOutput(const Val* val) const {
  return std::find(outputs_.begin(), outputs_.end(), val) != outputs_.end();
}

void Fusion::replaceInput(Val* replace, Val* with) {
  std::replace(inputs_.begin(), inputs_.end(), replace, with);
}

void Fusion::replaceOutput(Val* replace, Val* with) {
  std::replace(outputs_.begin(), outputs_.end(), replace, with);
}

StmtNameType Fusion::getValName(ValType vtype) {
  if (val_type_name_map_.find(vtype) != val_type_name_map_.end())
    return val_type_name_map_[vtype]++;
  return val_name_counter_++;
}

StmtNameType Fusion::getExprName() {
  return expr_name_counter_++;
}

// Indicate to kernel to set itself up to generate random numbers
bool Fusion::hasRNG() {
  for (auto expr : exprs(true))
    if (expr->getExprType() == ExprType::UnaryOp)
      if (static_cast<UnaryOp*>(expr)->getUnaryOpType() ==
          UnaryOpType::RandLike)
        return true;
  return false;
}

// Indicate to kernel to set itself up to generate random numbers
bool Fusion::hasReduction() {
  for (auto expr : exprs(true))
    for (auto out : expr->outputs())
      if (out->getValType() == ValType::TensorView)
        if (static_cast<TensorView*>(out)->hasReduction())
          return true;

  return false;
}

bool Fusion::hasBlockReduction() {
  for (auto expr : exprs(true))
    for (auto out : expr->outputs())
      if (out->getValType() == ValType::TensorView)
        if (static_cast<TensorView*>(out)->hasBlockReduction())
          return true;

  return false;
}

bool Fusion::hasGridReduction() {
  for (auto expr : exprs(true))
    for (auto out : expr->outputs())
      if (out->getValType() == ValType::TensorView)
        if (static_cast<TensorView*>(out)->hasGridReduction())
          return true;

  return false;
}

} // namespace fuser
} // namespace jit
} // namespace torch
