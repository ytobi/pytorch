#include <torch/csrc/jit/codegen/cuda/ir_iostream.h>
#include <torch/csrc/jit/codegen/cuda/fusion.h>
#include <torch/csrc/jit/codegen/cuda/ir_all_nodes.h>

#include <iostream>

namespace torch {
namespace jit {
namespace fuser {

// Make sure we can inline something, before we attempt to.
static void checkInlineable(const Expr* expr) {
  for (auto input : expr->inputs()) {
    TORCH_CHECK(
        input->isScalar(),
        "Printing inline computations involving values other than scalars is not currently supported.");
  }
  TORCH_CHECK(
      expr->outputs().size() == 1,
      "Cannot print inline computations if there's more than one output.");
  TORCH_CHECK(
      expr->output(0)->isScalar(),
      "Printing inline computations involving values other than scalars is not currently supported.");
}

void IRPrinter::handle(const Statement* s) {
  OptInConstDispatch::handle(s);
}

void IRPrinter::handle(const Val* v) {
  if (follow_val_map) {
    // Follow a single maping (permutation chains are not expected)
    v = FusionGuard::getCurFusion()->loweredVal(v);
    TORCH_INTERNAL_ASSERT(v == FusionGuard::getCurFusion()->loweredVal(v));
  }
  OptInConstDispatch::handle(v);
}

void IRPrinter::handle(const Expr* e) {
  OptInConstDispatch::handle(e);
}

void IRPrinter::printHeader(Fusion* fusion, const std::string& kernel_name_) {
  os << "__global__ void " << kernel_name_ << "(";

  std::vector<Val*> vals;

  for (auto val : fusion->inputs()) {
    vals.push_back(val);
  }
  for (auto val : fusion->outputs()) {
    vals.push_back(val);
  }

  for (Val* val : vals) {
    switch (val->getValType().value()) {
      case (ValType::TensorView):
        os << "Tensor<" << val->getDataType().value() << ", "
           << TensorDomain::noReductions(
                  static_cast<TensorView*>(val)->getRootDomain())
                  .size()
           << "> T" << val->name();
        break;
      case (ValType::Scalar):
        os << val->getDataType().value() << " " << val;
        break;
      default:
        TORCH_CHECK(
            false,
            "printHeader() found an input to the fusion of unexpected data type.");
    }

    if (val != vals.back())
      os << ", ";
  }

  if (fusion->hasRNG())
    os << ", unsigned long long seed, unsigned long long offset";

  if (fusion->hasGridReduction()) {
    os << ", void* work_buf, unsigned* sync_flags";
  }

  os << "){\n";
  indent_size++;
  if (fusion->hasRNG()) {
    indent();
    os << "int idx = blockIdx.x*blockDim.x + threadIdx.x;\n";
    indent();
    os << "Philox rnd(seed, idx, offset);\n";
  }
  if (fusion->hasBlockReduction() || fusion->hasGridReduction()) {
    indent();
    // TODO: Dynamic sizing possible? blockReduce originally used 1024
    // values of a given type
    os << "__shared__ float shared_mem[1024];\n";
  }
}

void IRPrinter::handle(Fusion* fusion) {
  resetIndent();
  for (const Expr* expr : fusion->exprs()) {
    handle(expr);
  }
}

void IRPrinter::handle(const TensorDomain* td) {
  if (td->nDims() == 0) {
    os << "[ 0 ]";
    return;
  }
  os << "[ ";
  for (size_t i = 0; i < td->nDims(); i++) {
    handle(td->axis(i));
    if (i != td->nDims() - 1)
      os << ", ";
  }
  os << " ]";
}

void IRPrinter::handle(const TensorView* tv) {
  os << "T" << tv->name();
  handle(tv->domain());

  if (tv->getComputeAtView() != nullptr) {
    os << " compute_at( ";
    os << "T" << tv->getComputeAtView()->name();
    os << ", " << tv->getRelativeComputeAtAxis() << " )";
  }
}

void IRPrinter::handle(const IterDomain* id) {
  if (id->isReduction())
    os << "r";
  else if (id->isBroadcast())
    os << "b";
  else
    os << "i";
  switch (id->parallel_method()) {
    case (ParallelType::Vectorize):
      os << "V";
      break;
    case (ParallelType::Unroll):
      os << "U";
      break;
    case (ParallelType::Serial):
      os << "S";
      break;
    default:
      os << id->parallel_method();
  }

  os << "{";
  if (!id->start()->isZeroInt()) {
    print_inline(id->start());
    os << " : ";
  }
  print_inline(id->extent());
  os << "}";
  if (id->isRFactorProduct())
    os << "rf";
}

void IRPrinter::handle(const TensorIndex* ti) {
  os << "T" << ti->view()->name();
  if (ti->nDims() == 0) {
    os << "[ 0 ]";
    return;
  }

  os << "[ ";
  bool first = true;
  for (auto* ind : ti->indices()) {
    if (!first)
      os << " + ";
    print_inline(ind);
    first = false;
  }
  os << " ]";
}

void IRPrinter::handle(const Bool* b) {
  if (print_inline_ && FusionGuard::getCurFusion()->origin(b) != nullptr) {
    os << "( ";
    handle(FusionGuard::getCurFusion()->origin(b));
    os << " )";
    return;
  }

  if (b->isSymbolic()) {
    os << "b" << b->name();
  } else {
    os << "bool(" << *(b->value()) << ")";
  }
}

void IRPrinter::handle(const Float* f) {
  if (print_inline_ && FusionGuard::getCurFusion()->origin(f) != nullptr) {
    os << "( ";
    handle(FusionGuard::getCurFusion()->origin(f));
    os << " )";
    return;
  }

  if (f->isSymbolic()) {
    os << "f" << f->name();
  } else {
    os << "float("
       << std::setprecision(
              std::numeric_limits<Float::ScalarType>::max_digits10)
       << *(f->value()) << ")";
  }
}

void IRPrinter::handle(const Half* h) {
  if (print_inline_ && FusionGuard::getCurFusion()->origin(h) != nullptr) {
    os << "( ";
    handle(FusionGuard::getCurFusion()->origin(h));
    os << " )";
    return;
  }

  if (h->isSymbolic()) {
    os << "h" << h->name();
  } else {
    os << "__float2half(" << *(h->value()) << ")";
  }
}

void IRPrinter::handle(const Int* i) {
  // Make sure we didn't bypass the value mapping
  // (for example calling IRPrinter::handle() with a Int*)
  TORCH_CHECK(
      !follow_val_map || i == FusionGuard::getCurFusion()->loweredVal(i));

  if (print_inline_) {
    if (auto def = FusionGuard::getCurFusion()->origin(i)) {
      os << "( ";
      handle(def);
      os << " )";
      return;
    }
  }

  if (i->isSymbolic()) {
    os << "i" << i->name();
  } else {
    os << *(i->value());
  }
}

void IRPrinter::handle(const NamedScalar* i) {
  os << i->name();
}

static bool isTV(const Val* val) {
  return val->getValType().value() == ValType::TensorView ||
      val->getValType().value() == ValType::TensorIndex;
}

// Check if we're a TensorView op that we can generate code for.
static bool isTVOp(const Expr* expr) {
  return expr->outputs().size() == 1 && isTV(expr->outputs().front());
}

void IRPrinter::handle(const UnaryOp* uop) {
  bool istvop = isTVOp(uop);
  if (!print_inline_) {
    indent();
    os << uop->out();
    if (istvop) {
      os << "\n";
      indent_size++;
      indent();
    }
    os << " = ";
  } else {
    checkInlineable(uop);
  }

  if (auto inline_uop = inline_op_str(uop->getUnaryOpType())) {
    os << inline_uop.value();
    handle(uop->in());
  } else {
    if (uop->getUnaryOpType() == UnaryOpType::Cast) {
      c10::optional<std::string> cast_str = cast_func_str(std::make_pair(
          static_cast<TensorIndex*>(uop->in())->view()->getDataType().value(),
          static_cast<TensorIndex*>(uop->out())
              ->view()
              ->getDataType()
              .value()));
      TORCH_INTERNAL_ASSERT(cast_str != c10::nullopt, "Unsupported Cast");
      os << cast_str.value();
    } else {
      os << uop->getUnaryOpType();
    }
    os << "(";
    if (uop->getUnaryOpType() == UnaryOpType::RandLike)
      os << "rnd";
    else
      handle(uop->in());
    os << ")";
  }

  if (istvop)
    indent_size--;

  if (!print_inline_)
    os << ";\n";
}

void IRPrinter::handle(const BinaryOp* bop) {
  bool istvop = isTVOp(bop);
  if (!print_inline_) {
    indent();
    os << bop->out();

    // tensor operations tend to be long, break them up into multiple lines
    if (istvop) {
      os << "\n";
      indent_size++;
      indent();
    }

    os << " = ";
  } else {
    checkInlineable(bop);
  }

  if (auto inline_bop = inline_op_str(bop->getBinaryOpType())) {
    handle(bop->lhs());
    if (istvop) {
      os << "\n";
      indent();
    }
    os << " " << inline_bop.value() << " ";
    handle(bop->rhs());
  } else {
    os << bop->getBinaryOpType() << "(";
    handle(bop->lhs());
    if (istvop) {
      os << "\n";
      indent();
    }
    os << ", ";
    handle(bop->rhs());
    os << ")";
  }

  if (istvop)
    indent_size--;

  if (!print_inline_)
    os << ";\n";
}

void IRPrinter::handle(const TernaryOp* top) {
  bool istvop = isTVOp(top);
  if (!print_inline_) {
    indent();
    os << top->out();

    // tensor operations tend to be long, break them up into multiple lines
    if (istvop) {
      os << "\n";
      indent_size++;
      indent();
    }

    os << " = ";
  } else {
    checkInlineable(top);
  }

  os << top->getTernaryOpType() << "(";
  handle(top->in1());
  if (istvop) {
    os << "\n";
    indent();
  }
  os << ", ";
  handle(top->in2());
  if (istvop) {
    os << "\n";
    indent();
  }
  os << ", ";
  handle(top->in3());
  os << ")";

  if (istvop)
    indent_size--;

  if (!print_inline_)
    os << ";\n";
}

void IRPrinter::handle(const ReductionOp* rop) {
  // Check if we've lowered yet.

  bool lowered = rop->out()->getValType() == ValType::TensorIndex;

  if (!lowered) {
    os << rop->out() << " = reduction( " << rop->in()
       << ", op = " << rop->getReductionOpType()
       << ", initial value = " << rop->init() << " )\n";
    return;
  }

  auto out = rop->out()->as<TensorIndex>();
  auto vec_domain = out->view()->domain()->domain();

  bool has_block_reduce = out->view()->hasBlockReduction();
  bool has_grid_reduce = out->view()->hasGridReduction();

  if (!has_block_reduce && !has_grid_reduce) {
    handle(new BinaryOp(rop->getReductionOpType(), out, out, rop->in()));
    return;
  }

  auto par_domains = rop->getParallelReductionDomains();
  bool tidx = par_domains.find(ParallelType::TIDx) != par_domains.end();
  bool tidy = par_domains.find(ParallelType::TIDy) != par_domains.end();
  bool tidz = par_domains.find(ParallelType::TIDz) != par_domains.end();
  bool bidx = par_domains.find(ParallelType::BIDx) != par_domains.end();
  bool bidy = par_domains.find(ParallelType::BIDy) != par_domains.end();
  bool bidz = par_domains.find(ParallelType::BIDz) != par_domains.end();

  auto d_type = rop->out()->getDataType().value();
  auto op_type = rop->getReductionOpType();
  const std::string block_result = "block_result";
  if (has_block_reduce) {
    if (has_grid_reduce) {
      indent();
      os << d_type << " " << block_result << ";\n";
    }
    indent();
    // Thread all reduce.
    os << "blockReduce< " << (tidx ? "true" : "false") << ", "
       << (tidy ? "true" : "false") << ", " << (tidz ? "true" : "false") << " >"
       << " ( ";
    if (has_grid_reduce) {
      os << block_result;
    } else {
      handle(rop->out());
    }
    os << ", ";
    handle(rop->in());
    os << ", ";
    os << "reduction_" << op_type << "_" << d_type;
    os << ", threadIdx, blockDim";
    os << ", reinterpret_cast<" << d_type << "*>(shared_mem)";
    os << ");\n";
  }
  if (has_grid_reduce) {
    indent();
    // Since block-level reduction is already done, those dimensions
    // with tidx/y/z being true do not participate in the grid reduction.
    os << "reduction::gridReduce< " << (bidx ? "true" : "false") << ", "
       << (bidy ? "true" : "false") << ", " << (bidz ? "true" : "false") << ", "
       << (!tidx ? "true" : "false") << ", " << (!tidy ? "true" : "false")
       << ", " << (!tidz ? "true" : "false") << " >"
       << " ( ";
    handle(rop->out());
    os << ", ";
    if (has_block_reduce) {
      os << block_result;
    } else {
      handle(rop->in());
    }
    os << ", ";
    os << "reduction_" << op_type << "_" << d_type;
    os << ", static_cast<" << d_type << "*>(work_buf)";
    os << ", sync_flags";
    os << ", reinterpret_cast<" << d_type << "*>(shared_mem)";
    os << ");\n";
  }
}

void IRPrinter::handle(const BroadcastOp* bop) {
  indent();
  handle(bop->out());
  os << "\n";
  indent_size++;
  indent();
  os << " = ";
  handle(bop->in());
  indent_size--;
  os << ";\n";
}

void IRPrinter::handle(const ForLoop* fl) {
  if (fl->iter_domain()->isThread() || fl->iter_domain()->isBroadcast()) {
    for (auto& expr : fl->constBody().exprs())
      handle(expr);
    return;
  }

  indent();
  os << "for(size_t ";
  handle(fl->index());
  os << " = ";
  print_inline(fl->iter_domain()->start());
  os << "; ";
  handle(fl->index());
  os << " < ";
  print_inline(fl->iter_domain()->extent());
  os << "; ++";
  handle(fl->index());
  os << " ) {\n";
  indent_size++;
  for (auto& expr : fl->constBody().exprs())
    handle(expr);

  indent_size--;
  indent();
  os << "}\n";
}

void IRPrinter::handle(const IfThenElse* ite) {
  indent();

  // IF
  os << "if ( ";
  print_inline(ite->cond());
  os << " ) { \n";

  indent_size++;
  for (auto& expr : ite->constBody().exprs()) {
    handle(expr);
  }
  indent_size--;

  // ELSE
  if (ite->hasElse()) {
    indent();
    os << "} else { \n";
    indent_size++;
    for (auto& expr : ite->constElseBody().exprs()) {
      handle(expr);
    }
    indent_size--;
  }
  indent();
  os << "}\n";
}

void IRPrinter::handle(const Allocate* a) {
  indent();
  os << a->buf_type();
  if (a->buffer()->getValType() == ValType::TensorView) {
    os << " T" << a->buffer()->name() << "[";
    print_inline(a->extent());
    os << "];\n";
  } else {
    if (a->extent()->isOneInt()) {
      os << " " << a->buffer() << ";\n";
    } else {
      TORCH_INTERNAL_ASSERT(
          false,
          "Received unexpected allocation: ",
          a->buffer(),
          " with alloc of ",
          a->extent());
    }
  }
}

void IRPrinter::handle(const Split* s) {
  os << "Split: ";
  handle(s->in());
  os << " by factor " << s->factor() << " -> ";
  handle(s->outer());
  os << ", ";
  handle(s->inner());
  os << "\n";
}

void IRPrinter::handle(const Merge* m) {
  os << "Merge: ";
  handle(m->outer());
  os << " and ";
  handle(m->inner());
  os << " -> ";
  handle(m->out());
  os << "\n";
}

namespace {

class ReductionOps : OptOutDispatch {
 public:
  std::set<std::pair<BinaryOpType, DataType>> rops;
  void handle(ReductionOp* rop) override {
    rops.emplace(std::pair<BinaryOpType, DataType>{
        rop->getReductionOpType(), rop->in()->getDataType().value()});
  }

  using OptOutDispatch::handle;

  static std::set<std::pair<BinaryOpType, DataType>> get(Fusion* fusion) {
    ReductionOps ROPs;
    for (auto expr : fusion->exprs(true)) {
      ROPs.handle(expr);
    }
    return ROPs.rops;
  }
};

} // namespace

void IRPrinter::printReductionOps(Fusion* fusion) {
  auto a = new NamedScalar("a", DataType::Null);
  auto b = new NamedScalar("b", DataType::Null);
  for (auto rop_pair : ReductionOps::get(fusion)) {
    auto op_type = rop_pair.first;
    auto d_type = rop_pair.second;

    indent();
    os << "__device__ void reduction_" << op_type << "_" << d_type << "("
       << d_type << "& a, "
       << "const " << d_type << " b) {\n";
    indent_size++;
    handle(new BinaryOp(op_type, a, a, b));
    indent_size--;
    indent();
    os << "}\n";
  }
}

void IRPrinter::printKernel(
    const std::vector<Expr*>& exprs,
    const std::string& kernel_name) {
  Fusion* fusion = FusionGuard::getCurFusion();
  printReductionOps(fusion);
  printHeader(fusion, kernel_name);
  for (auto* expr : exprs) {
    handle(expr);
  }
  os << "}\n";
}

std::ostream& operator<<(std::ostream& os, const Statement* stmt) {
  IRPrinter p(os);
  p.handle(stmt);
  return os;
}

std::ostream& operator<<(std::ostream& os, Fusion* f) {
  IRPrinter p(os);
  FusionGuard guard(f);
  p.handle(f);
  return os;
}

std::ostream& operator<<(std::ostream& os, Fusion& f) {
  return os << &f;
}

} // namespace fuser
} // namespace jit
} // namespace torch
