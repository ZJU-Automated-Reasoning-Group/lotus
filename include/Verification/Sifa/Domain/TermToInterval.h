//===-- Verification/Sifa/Domain/TermToInterval.h -------------------------===//
//
// Term/Value to interval conversion (Ultimate TermToInterval-aligned).
//
// Ultimate's TermToInterval evaluates an SMT term to one interval given
// variable -> interval assignments. In lotus we provide:
//   - evaluate(Value*, varToInterval): LLVM path for ConstantInt, add, sub,
//   etc.
//   - SMT overload throws for API parity (SMT-only in Ultimate).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_TERMTOINTERVAL_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_TERMTOINTERVAL_H

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

#include "Verification/Sifa/Domain/IntervalDomain.h"

#include <stdexcept>
#include <unordered_map>

namespace lotus {
namespace sifa {

/// Ultimate TermToInterval: evaluate(term, varToInterval) -> one Interval.
/// LLVM path: ConstantInt/add/sub supported; otherwise returns top.
struct TermToInterval {
  /// LLVM path (Ultimate-aligned): evaluate \p V using \p varToInterval for
  /// operands. ConstantInt -> point interval; Add/Sub -> interval arithmetic;
  /// unknown -> top.
  static Interval evaluate(
      const llvm::Value *V,
      const std::unordered_map<const llvm::Value *, Interval> &varToInterval) {
    if (!V)
      return Interval::top();
    if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(V)) {
      if (C->getBitWidth() > 64)
        return Interval::top();
      int64_t val = C->getSExtValue();
      Interval i;
      i.lo = val;
      i.hi = val;
      i.isBottom_ = false;
      return i;
    }
    auto it = varToInterval.find(V);
    if (it != varToInterval.end())
      return it->second;

    if (const auto *I = llvm::dyn_cast<llvm::BinaryOperator>(V)) {
      Interval L = evaluate(I->getOperand(0), varToInterval);
      Interval R = evaluate(I->getOperand(1), varToInterval);
      if (L.isBottom() || R.isBottom())
        return Interval::bottom();
      switch (I->getOpcode()) {
      case llvm::Instruction::Add:
        return L.add(R);
      case llvm::Instruction::Sub:
        return L.subtract(R);
      default:
        break;
      }
    }
    return Interval::top();
  }

  /// SMT path: Ultimate API; throws in lotus (no SMT).
  template <typename Term>
  static Interval evaluate(const Term &,
                           const std::unordered_map<const void *, Interval> &) {
    throw std::runtime_error(
        "TermToInterval::evaluate(SMT): SMT-only; use evaluate(Value*, ...).");
  }
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_TERMTOINTERVAL_H
