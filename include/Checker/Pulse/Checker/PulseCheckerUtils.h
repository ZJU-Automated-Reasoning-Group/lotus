#ifndef CHECKER_PULSE_PULSECHECKERUTILS_H
#define CHECKER_PULSE_PULSECHECKERUTILS_H

#include "Checker/Pulse/Core/PulseFormula.h"

#include <llvm/IR/Instructions.h>

namespace llvm {
class Value;
} // namespace llvm

namespace pulse {
namespace detail {

/** True if v is a null pointer constant (null, or 0 cast to pointer). */
bool isNullPointerConstantValue(const llvm::Value *v);

/** Invert an ICmp predicate (e.g. EQ -> NE). */
llvm::ICmpInst::Predicate invertIcmpPred(llvm::ICmpInst::Predicate p);

/** Add path condition for integer comparison; returns false if UNSAT. */
bool applyIntegerIcmpConstraint(PulseFormula &formula,
                                llvm::ICmpInst::Predicate p, AbstractValue lhs,
                                AbstractValue rhs);

} // namespace detail
} // namespace pulse

#endif
