#include "Verification/SymAbsAI/Domains/MemRegions.h"

#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Expression.h"
#include "Verification/SymAbsAI/Core/MemoryModel.h"
#include "Verification/SymAbsAI/Core/ParamStrategy.h"

#include <string>
// #include "Verification/SymAbsAI/Domains/Combinators.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Type.h>

using namespace symabs_ai;
using namespace domains;

namespace {
template <typename T>
unique_ptr<AbstractValue> ForPointerSizePairs(const FunctionContext &fctx,
                                              llvm::BasicBlock *bb,
                                              bool after) {
  Product *result = new Product(fctx);
  auto vars = fctx.valuesAvailableIn(bb, after);
  auto &mctx = fctx.getModuleContext();

  for (int i = 0; i < (int)vars.size(); i++) {
    if (!vars[i]->getType()->isPointerTy())
      continue;
    llvm::Type *pointee_type = vars[i]->getType()->getPointerElementType();

    int bytes = mctx.getDataLayout()->getTypeAllocSize(pointee_type);
    Expression byte_factor =
        ConcreteState::Value(&fctx.getZ3(), bytes, fctx.getPointerSize());

    for (int j = 0; j < (int)vars.size(); j++) {
      z3::sort sort = fctx.sortForType(vars[j]->getType());
      if (!(sort.is_bv() && (int)sort.bv_size() == fctx.getPointerSize()))
        continue;
      Expression rhs = vars[j];
      T *ptr = new T(fctx, vars[i], rhs, byte_factor);
      result->add(unique_ptr<AbstractValue>(ptr));
    }
  }

  result->finalize();
  return unique_ptr<AbstractValue>(result);
}
} // namespace

namespace symabs_ai {
namespace domains {
NoAlias::NoAlias(const FunctionContext &fctx, const RepresentedValue left,
                 RepresentedValue right)
    : BooleanValue(fctx), Left_(left), Right_(right) {
  MM_ = dynamic_cast<const memory::BlockModel *>(&fctx.getMemoryModel());
  if (!MM_) {
    llvm_unreachable("Inappropriate MemoryModel for ValidRegion domain!");
  }
}

void NoAlias::prettyPrint(PrettyPrinter &out) const {
  std::string left_name = Left_->getName().str();
  std::string right_name = Right_->getName().str();

  out << "(" << left_name << ", " << right_name << "): ";

  switch (Val_) {
  case BOTTOM:
    out << pp::bottom;
    break;
  case TOP:
    out << "may alias";
    break;
  case TRUE:
    out << "must not alias";
    break;
  case FALSE:
    out << "must alias";
    break;
  }
}

z3::expr NoAlias::makePredicate(const ValueMapping &vmap) const {
  z3::expr l = vmap.getFullRepresentation(Left_);
  z3::expr r = vmap.getFullRepresentation(Right_);
  return MM_->no_alias(l, r);
}

ValidRegion::ValidRegion(const FunctionContext &fctx, RepresentedValue ptr)
    : BooleanValue(fctx), Ptr_(ptr) {
  MM_ = dynamic_cast<const memory::BlockModel *>(&fctx.getMemoryModel());
  if (!MM_) {
    llvm_unreachable("Inappropriate MemoryModel for ValidRegion domain!");
  }
}

z3::expr ValidRegion::makePredicate(const ValueMapping &vmap) const {
  z3::expr ptr = vmap.getFullRepresentation(Ptr_);
  z3::expr mem = vmap.memory();

  return MM_->valid_region(mem, ptr);
}

void ValidRegion::prettyPrint(PrettyPrinter &out) const {
  std::string ptr_name = Ptr_->getName().str();

  out << ptr_name << pp::rightarrow;

  switch (Val_) {
  case BOTTOM:
    out << pp::bottom;
    break;
  case TOP:
    out << "possibly invalid region";
    break;
  case TRUE:
    out << "definitely valid region";
    break;
  case FALSE:
    out << "definitely invalid region";
    break;
  }
}

ConstantRegion::ConstantRegion(const FunctionContext &fctx,
                               RepresentedValue value)
    : SimpleConstProp(fctx, value), Ptr_(value) {
  MM_ = dynamic_cast<const memory::BlockModel *>(&fctx.getMemoryModel());
  assert(MM_ && "Inappropriate MemoryModel for ConstantRegion domain!");
}

AbstractValue *ConstantRegion::clone() const {
  return new ConstantRegion(*this);
}

bool ConstantRegion::updateWith(const ConcreteState &cstate) {
  if (isTop())
    return false;

  const z3::model *mod = cstate.getModel();
  if (mod == nullptr) {
    // we cannot get region information if the model is not available (i.e.
    // in dynamic analysis) but we don't need to be sound in that case so
    // we can ignore the update
    return false;
  }

  z3::expr mem = cstate.getValueMapping().memory();
  z3::expr ptr = cstate.getValueMapping().getFullRepresentation(Value_);

  if (!expr_to_bool(mod->eval(MM_->valid_region(mem, ptr), true))) {
    havoc();
    return true;
  }

  ConstantRegion other(*FunctionContext_, Value_);
  other.Bottom_ = false;
  other.Top_ = false;
  other.Constant_ = mod->eval(MM_->region_size(mem, ptr), true);
  return joinWith(other);
}

z3::expr ConstantRegion::toFormula(const ValueMapping &vmap,
                                   z3::context &zctx) const {
  z3::expr result = zctx.bool_val(true);
  z3::expr ptr = vmap.getFullRepresentation(Value_);

  if (isBottom())
    result = zctx.bool_val(false);
  else if (isTop())
    result = zctx.bool_val(true);
  else {
    z3::expr size = (z3::expr)Constant_;
    result = (MM_->valid_region(vmap.memory(), ptr) &&
              (MM_->region_size(vmap.memory(), ptr) == size));
  }

  return result;
}

bool ConstantRegion::isJoinableWith(const AbstractValue &other) const {
  if (auto *other_val = dynamic_cast<const ConstantRegion *>(&other)) {
    if (other_val->Value_ == Value_ &&
        other_val->FunctionContext_ == FunctionContext_) {
      return true;
    }
  }
  return false;
}

void ConstantRegion::prettyPrint(PrettyPrinter &out) const {
  out << Value_ << pp::rightarrow;

  if (isTop())
    out << pp::top;
  else if (isBottom())
    out << pp::bottom;
  else
    out << "region of size " << repr(Constant_) << " (if valid)";
}

VariableRegion::VariableRegion(const FunctionContext &fctx,
                               RepresentedValue ptr, const Expression &expr,
                               const Expression &factor)
    : BooleanValue(fctx), Ptr_(ptr), Expr_(expr), Fact_(factor) {
  MM_ = dynamic_cast<const memory::BlockModel *>(&fctx.getMemoryModel());
  assert(MM_ && "Inappropriate MemoryModel for VariableRegion domain!");
}

void VariableRegion::prettyPrint(PrettyPrinter &out) const {
  std::string ptr_name = Ptr_->getName().str();

  out << ptr_name << pp::rightarrow;

  switch (Val_) {
  case BOTTOM:
    out << pp::bottom;
    break;
  case TOP:
    out << pp::top;
    break;
  case TRUE:
    out << "region of size == " << Fact_ << " * " << Expr_ << " (if valid)";
    break;
  case FALSE:
    out << "region of size != " << Fact_ << " * " << Expr_ << " (if valid)";
    break;
  }
}

z3::expr VariableRegion::makePredicate(const ValueMapping &vmap) const {
  z3::expr p = vmap.getFullRepresentation(Ptr_);
  z3::expr e = Expr_.toFormula(vmap);
  assert(e.is_bv());
  z3::expr f = Fact_.toFormula(vmap);
  assert(f.is_bv());
  z3::expr m = vmap.memory();
  unsigned bits = e.get_sort().bv_size();
  z3::expr e_zext = z3_ext::zext(bits, e);
  z3::expr f_zext = z3_ext::zext(bits, f);
  z3::expr no_ovf = e.ctx().bv_val(0, bits) ==
                    z3_ext::extract(2 * bits - 1, bits, e_zext * f_zext);

  return (MM_->region_size(m, p) == f * e) && no_ovf;
}

MemoryRegion::MemoryRegion(const FunctionContext &fctx, RepresentedValue ptr)
    : Product(fctx), Ptr_(ptr) {}

void MemoryRegion::prettyPrint(PrettyPrinter &out) const {
  PrettyPrinter::Entry block(&out, "MemoryRegion");
  out << "MemRegion information for " << Ptr_ << ":\n";
  if (isTop()) {
    out << "  " << pp::top;
    return;
  }

  if (isBottom()) {
    out << "  " << pp::bottom;
    return;
  }

  for (auto &x : getValues()) {
    if (x->isTop() && !dynamic_cast<ValidRegion *>(x.get()))
      continue;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpotentially-evaluated-expression"
#endif
    PrettyPrinter::Entry block(&out, typeid(*x).name());
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    x->prettyPrint(out);
  }
}

unique_ptr<AbstractValue> MemoryRegion::Create(const FunctionContext &fctx,
                                               llvm::BasicBlock *bb,
                                               bool after) {
  Product *result = new Product(fctx);
  auto vars = fctx.valuesAvailableIn(bb, after);
  auto &mctx = fctx.getModuleContext();

  for (int i = 0; i < (int)vars.size(); i++) {
    if (!vars[i]->getType()->isPointerTy())
      continue;

    Product *temp = new MemoryRegion(fctx, vars[i]);
    temp->add(std::make_unique<ValidRegion>(fctx, vars[i]));
    temp->add(std::make_unique<ConstantRegion>(fctx, vars[i]));

    llvm::Type *pointee_type = vars[i]->getType()->getPointerElementType();

    int bytes = mctx.getDataLayout()->getTypeAllocSize(pointee_type);
    Expression byte_factor =
        ConcreteState::Value(&fctx.getZ3(), bytes, fctx.getPointerSize());

    for (int j = 0; j < (int)vars.size(); j++) {
      z3::sort sort = fctx.sortForType(vars[j]->getType());
      if (!(sort.is_bv() && (int)sort.bv_size() == fctx.getPointerSize()))
        continue;
      Expression rhs = vars[j];
      auto *ptr = new RestrictedVarRegion(fctx, vars[i], rhs, byte_factor);
      temp->add(unique_ptr<AbstractValue>(ptr));
    }
    temp->finalize();
    result->add(unique_ptr<AbstractValue>(temp));
  }

  result->finalize();
  return unique_ptr<AbstractValue>(result);
}
} // namespace domains
} // namespace symabs_ai

namespace {
DomainConstructor::Register
    NoAlias("NoAlias",
            "domain for expressing whether two pointers point to distinct"
            " memory regions",
            [](const FunctionContext &fctx, llvm::BasicBlock *loc, bool after) {
              return params::ForPointerPairs<symabs_ai::domains::NoAlias>(
                  fctx, loc, after, true);
            });

DomainConstructor::Register ValidRegion(
    "ValidRegion",
    " domain for expressing whether a pointer points to a valid"
    " memory region",
    [](const FunctionContext &fctx, llvm::BasicBlock *loc, bool after) {
      return params::ForPointers<symabs_ai::domains::ValidRegion>(fctx, loc,
                                                                  after);
    });

DomainConstructor::Register ConstRegion(
    "ConstRegion",
    " domain for expressing a constant size of the memory region"
    " pointed to by a pointer if it points to a valid region",
    [](const FunctionContext &fctx, llvm::BasicBlock *loc, bool after) {
      return params::ForPointers<symabs_ai::domains::ConstantRegion>(fctx, loc,
                                                                     after);
    });

DomainConstructor::Register VarRegion(
    "VarRegion",
    " domain for expressing a size of the memory region"
    " pointed to by a pointer in terms of an expression if it"
    " points to a valid region",
    [](const FunctionContext &fctx, llvm::BasicBlock *loc, bool after) {
      return ForPointerSizePairs<symabs_ai::domains::VariableRegion>(fctx, loc,
                                                                     after);
    });

DomainConstructor::Register MemRegion(
    "MemRegion", " domain for expressing memory region related information",
    [](const FunctionContext &fctx, llvm::BasicBlock *loc, bool after) {
      return MemoryRegion::Create(fctx, loc, after);
    });
} // namespace
