//===-- Verification/Sifa/Domain/DnfToExplicitValue.h
//----------------------===//
//
// DNF / condition to explicit value conversion (Ultimate
// DnfToExplicitValue-aligned).
//
// Ultimate's DnfToExplicitValue converts a DNF disjunct (SMT term) into
// explicit variable = constant form. In lotus we provide:
//   - tryGetConstant(Value*): optional constant for ConstantInt.
//   - tryGetEqConstant(Value*): for x==c or c==x (ICmpInst) returns (var,
//   const).
//   - convert(Term): SMT path; throws.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_DNFTOEXPLICITVALUE_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_DNFTOEXPLICITVALUE_H

#include "llvm/ADT/Optional.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

#include <stdexcept>
#include <utility>

namespace lotus {
namespace sifa {

/// Ultimate DnfToExplicitValue: convert(DNF disjunct) -> explicit var=const.
/// LLVM path: tryGetConstant, tryGetEqConstant; SMT convert() throws.
class DnfToExplicitValue {
public:
  /// LLVM path: if \p V is a ConstantInt (fits in 64-bit), return its value.
  static llvm::Optional<int64_t> tryGetConstant(const llvm::Value *V) {
    if (!V)
      return llvm::None;
    const auto *C = llvm::dyn_cast<llvm::ConstantInt>(V);
    if (!C || C->getBitWidth() > 64)
      return llvm::None;
    return C->getSExtValue();
  }

  /// LLVM path: if \p V is an equality comparison (icmp eq x, c or c, x),
  /// return (variable, constant). Otherwise None.
  static llvm::Optional<std::pair<const llvm::Value *, int64_t>>
  tryGetEqConstant(const llvm::Value *V) {
    const auto *I = llvm::dyn_cast<llvm::ICmpInst>(V);
    if (!I || I->getPredicate() != llvm::CmpInst::ICMP_EQ)
      return llvm::None;
    auto c0 = tryGetConstant(I->getOperand(0));
    auto c1 = tryGetConstant(I->getOperand(1));
    if (c0 && !c1)
      return std::make_pair(I->getOperand(1), *c0);
    if (!c0 && c1)
      return std::make_pair(I->getOperand(0), *c1);
    return llvm::None;
  }

  /// Ultimate: TermTransformer.convert(term). SMT-only; throws in lotus.
  template <typename Term> void convert(const Term &) {
    throw std::runtime_error("DnfToExplicitValue::convert: SMT-only; use "
                             "tryGetConstant/tryGetEqConstant.");
  }
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_DNFTOEXPLICITVALUE_H
