#include "Checker/Pulse/Checker/PulseCheckerUtils.h"

#include "Checker/Pulse/Core/PulseFormula.h"

#include <limits>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace pulse {
namespace detail {

static llvm::Optional<int64_t> getI64Constant(const llvm::Value *v,
                                              bool is_signed) {
  if (!v)
    return llvm::None;
  auto *CI = llvm::dyn_cast<llvm::ConstantInt>(v);
  if (!CI)
    return llvm::None;
  if (CI->getBitWidth() > 64)
    return llvm::None;
  if (is_signed)
    return CI->getSExtValue();
  if (CI->getBitWidth() == 64 && CI->isNegative())
    return llvm::None;
  return static_cast<int64_t>(CI->getZExtValue());
}

static llvm::ICmpInst::Predicate swapIcmpSides(llvm::ICmpInst::Predicate p) {
  using P = llvm::ICmpInst::Predicate;
  switch (p) {
  case P::ICMP_SLT:
    return P::ICMP_SGT;
  case P::ICMP_SLE:
    return P::ICMP_SGE;
  case P::ICMP_SGT:
    return P::ICMP_SLT;
  case P::ICMP_SGE:
    return P::ICMP_SLE;
  case P::ICMP_ULT:
    return P::ICMP_UGT;
  case P::ICMP_ULE:
    return P::ICMP_UGE;
  case P::ICMP_UGT:
    return P::ICMP_ULT;
  case P::ICMP_UGE:
    return P::ICMP_ULE;
  default:
    return p;
  }
}

bool isNullPointerConstantValue(const llvm::Value *v) {
  if (!v)
    return false;
  if (llvm::isa<llvm::ConstantPointerNull>(v))
    return true;
  if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(v)) {
    if (CE->getOpcode() == llvm::Instruction::IntToPtr) {
      if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(CE->getOperand(0)))
        return CI->isZero();
    }
  }
  if (auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(v)) {
    if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(I2P->getOperand(0)))
      return CI->isZero();
  }
  return false;
}

llvm::ICmpInst::Predicate invertIcmpPred(llvm::ICmpInst::Predicate p) {
  using P = llvm::ICmpInst::Predicate;
  switch (p) {
  case P::ICMP_EQ:
    return P::ICMP_NE;
  case P::ICMP_NE:
    return P::ICMP_EQ;
  case P::ICMP_SLT:
    return P::ICMP_SGE;
  case P::ICMP_SLE:
    return P::ICMP_SGT;
  case P::ICMP_SGT:
    return P::ICMP_SLE;
  case P::ICMP_SGE:
    return P::ICMP_SLT;
  case P::ICMP_ULT:
    return P::ICMP_UGE;
  case P::ICMP_ULE:
    return P::ICMP_UGT;
  case P::ICMP_UGT:
    return P::ICMP_ULE;
  case P::ICMP_UGE:
    return P::ICMP_ULT;
  default:
    return p;
  }
}

bool applyIntegerIcmpConstraint(PulseFormula &formula,
                                llvm::ICmpInst::Predicate p, AbstractValue lhs,
                                AbstractValue rhs) {
  using P = llvm::ICmpInst::Predicate;
  formula.addIntegerConstraint(lhs);
  formula.addIntegerConstraint(rhs);

  const llvm::Value *rhs_v = rhs.getValue();
  const llvm::Value *lhs_v = lhs.getValue();

  bool rhs_is_const = llvm::isa_and_nonnull<llvm::ConstantInt>(rhs_v);
  bool lhs_is_const = llvm::isa_and_nonnull<llvm::ConstantInt>(lhs_v);
  if (lhs_is_const && !rhs_is_const) {
    std::swap(lhs, rhs);
    std::swap(lhs_v, rhs_v);
    p = swapIcmpSides(p);
    rhs_is_const = llvm::isa_and_nonnull<llvm::ConstantInt>(rhs_v);
  }

  if (p == P::ICMP_EQ)
    return formula.addEquality(lhs, rhs);
  if (p == P::ICMP_NE)
    return formula.addDisequality(lhs, rhs);

  const bool is_signed = (p == P::ICMP_SLT || p == P::ICMP_SLE ||
                          p == P::ICMP_SGT || p == P::ICMP_SGE);

  if (rhs_is_const) {
    auto c_opt = getI64Constant(rhs_v, is_signed);
    if (!c_opt)
      return true;
    const int64_t c = *c_opt;

    auto add_lower = [&](int64_t lb) {
      return formula.addBounds(lhs, lb, std::numeric_limits<int64_t>::max());
    };
    auto add_upper = [&](int64_t ub) {
      return formula.addBounds(lhs, std::numeric_limits<int64_t>::min(), ub);
    };

    switch (p) {
    case P::ICMP_SLT:
    case P::ICMP_ULT:
      if (c == std::numeric_limits<int64_t>::min())
        return false;
      return add_upper(c - 1);
    case P::ICMP_SLE:
    case P::ICMP_ULE:
      return add_upper(c);
    case P::ICMP_SGT:
    case P::ICMP_UGT:
      if (c == std::numeric_limits<int64_t>::max())
        return false;
      return add_lower(c + 1);
    case P::ICMP_SGE:
    case P::ICMP_UGE:
      return add_lower(c);
    default:
      return true;
    }
  }

  LinearConstraint lc;
  lc.terms.emplace_back(lhs, 1);
  lc.terms.emplace_back(rhs, -1);
  lc.constant = 0;
  switch (p) {
  case P::ICMP_SLT:
  case P::ICMP_ULT:
    lc.kind = ConstraintKind::Less;
    break;
  case P::ICMP_SLE:
  case P::ICMP_ULE:
    lc.kind = ConstraintKind::LessEqual;
    break;
  case P::ICMP_SGT:
  case P::ICMP_UGT:
    lc.kind = ConstraintKind::Greater;
    break;
  case P::ICMP_SGE:
  case P::ICMP_UGE:
    lc.kind = ConstraintKind::GreaterEqual;
    break;
  default:
    return true;
  }
  return formula.addLinearConstraint(lc);
}

} // namespace detail
} // namespace pulse
