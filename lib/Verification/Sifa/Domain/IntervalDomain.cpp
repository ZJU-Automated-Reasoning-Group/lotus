//===-- Verification/Sifa/Domain/IntervalDomain.cpp -----------------------===//
//
// Instruction-level block transfer for Sifa Interval domain.
// Applies sound over-approximating transfer for each LLVM instruction in a
// basic block so that post(Edge) models real program semantics.
// When alias analysis is set, Load/Store use region-based memory (IKOS/CLAM
// style) reusing lib/Alias.
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Domain/IntervalDomain.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Verification/Sifa/RegionMemory.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace lotus::sifa;

namespace {

Interval getInterval(const IntervalState &state, const llvm::Value *V);
bool isFunctionLocalValue(const llvm::Function *F, const llvm::Value *V);
IntervalState refineForTakenEdge(const Transition &t, IntervalState out);
IntervalState projectCallState(const Transition &t,
                               const IntervalState &callerState);
IntervalState mergeReturnState(const Transition &t,
                               const IntervalState &callerState,
                               const IntervalState &calleeSummary);

llvm::BasicBlock::const_iterator
segmentBegin(const llvm::BasicBlock *bb,
             const llvm::Instruction *segmentStart) {
  if (segmentStart) {
    return segmentStart->getIterator();
  }
  auto it = bb->begin();
  while (it != bb->end() && llvm::isa<llvm::PHINode>(&*it)) {
    ++it;
  }
  return it;
}

const llvm::Value *incomingValueForPredecessor(const llvm::PHINode &phi,
                                               const llvm::BasicBlock *pred) {
  if (!pred)
    return nullptr;
  const int index =
      phi.getBasicBlockIndex(const_cast<llvm::BasicBlock *>(pred));
  if (index < 0)
    return nullptr;
  return phi.getIncomingValue(static_cast<unsigned>(index));
}

IntervalState applyIncomingPhis(const Transition &t, IntervalState out) {
  if (out.isBottom())
    return out;
  if (!t.source || !t.target || !t.landsAtBlockEntry())
    return out;
  for (const llvm::Instruction &I : *t.target) {
    const auto *phi = llvm::dyn_cast<llvm::PHINode>(&I);
    if (!phi)
      break;
    out.set(phi, getInterval(out, incomingValueForPredecessor(*phi, t.source)));
  }
  return out;
}

/// Return the interval for \p V from \p state, or Interval::top() if unknown.
Interval getInterval(const IntervalState &state, const llvm::Value *V) {
  if (!V)
    return Interval::top();
  auto opt = state.get(V);
  if (opt.hasValue())
    return opt.getValue();
  if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(V)) {
    if (C->getBitWidth() > 64)
      return Interval::top();
    int64_t val = C->getSExtValue();
    return Interval::point(val);
  }
  return Interval::top();
}

bool isFunctionLocalValue(const llvm::Function *F, const llvm::Value *V) {
  if (!F || !V)
    return false;
  if (const auto *I = llvm::dyn_cast<llvm::Instruction>(V))
    return I->getFunction() == F;
  if (const auto *A = llvm::dyn_cast<llvm::Argument>(V))
    return A->getParent() == F;
  return false;
}

llvm::Optional<int64_t> decrementBound(const llvm::Optional<int64_t> &bound,
                                       bool &overflowed) {
  if (!bound.hasValue())
    return llvm::None;
  if (*bound == std::numeric_limits<int64_t>::min()) {
    overflowed = true;
    return llvm::None;
  }
  return *bound - 1;
}

llvm::Optional<int64_t> incrementBound(const llvm::Optional<int64_t> &bound,
                                       bool &overflowed) {
  if (!bound.hasValue())
    return llvm::None;
  if (*bound == std::numeric_limits<int64_t>::max()) {
    overflowed = true;
    return llvm::None;
  }
  return *bound + 1;
}

Interval intersectWithUpper(const Interval &input,
                            const llvm::Optional<int64_t> &upper) {
  if (input.isBottom())
    return input;
  Interval bound{llvm::None, upper, false};
  return input.intersect(bound);
}

Interval intersectWithLower(const Interval &input,
                            const llvm::Optional<int64_t> &lower) {
  if (input.isBottom())
    return input;
  Interval bound{lower, llvm::None, false};
  return input.intersect(bound);
}

bool isDefinitelyNonNegative(const Interval &interval) {
  return interval.lo.hasValue() && *interval.lo >= 0;
}

bool evaluateUnsignedPredicate(llvm::CmpInst::Predicate predicate, uint64_t lhs,
                               uint64_t rhs) {
  switch (predicate) {
  case llvm::CmpInst::ICMP_EQ:
    return lhs == rhs;
  case llvm::CmpInst::ICMP_NE:
    return lhs != rhs;
  case llvm::CmpInst::ICMP_ULT:
    return lhs < rhs;
  case llvm::CmpInst::ICMP_ULE:
    return lhs <= rhs;
  case llvm::CmpInst::ICMP_UGT:
    return lhs > rhs;
  case llvm::CmpInst::ICMP_UGE:
    return lhs >= rhs;
  default:
    return false;
  }
}

bool evaluateSignedPredicate(llvm::CmpInst::Predicate predicate, int64_t lhs,
                             int64_t rhs) {
  switch (predicate) {
  case llvm::CmpInst::ICMP_EQ:
    return lhs == rhs;
  case llvm::CmpInst::ICMP_NE:
    return lhs != rhs;
  case llvm::CmpInst::ICMP_SLT:
    return lhs < rhs;
  case llvm::CmpInst::ICMP_SLE:
    return lhs <= rhs;
  case llvm::CmpInst::ICMP_SGT:
    return lhs > rhs;
  case llvm::CmpInst::ICMP_SGE:
    return lhs >= rhs;
  default:
    return false;
  }
}

IntervalState bottomState() {
  IntervalState s(true);
  return s;
}

IntervalState setOperandInterval(IntervalState state,
                                 const llvm::Value *operand,
                                 const Interval &interval) {
  if (!operand || llvm::isa<llvm::Constant>(operand))
    return state;
  if (interval.isBottom())
    return bottomState();
  state.set(operand, interval);
  return state;
}

IntervalState refineForPredicate(IntervalState out, const llvm::ICmpInst &cmp,
                                 bool truthy) {
  if (out.isBottom())
    return out;
  const llvm::Value *lhsValue = cmp.getOperand(0);
  const llvm::Value *rhsValue = cmp.getOperand(1);
  const Interval lhs = getInterval(out, lhsValue);
  const Interval rhs = getInterval(out, rhsValue);
  if (lhs.isBottom() || rhs.isBottom())
    return bottomState();

  llvm::CmpInst::Predicate predicate = cmp.getPredicate();
  if (!truthy)
    predicate = cmp.getInversePredicate();

  if (llvm::CmpInst::isUnsigned(predicate)) {
    if (!isDefinitelyNonNegative(lhs) || !isDefinitelyNonNegative(rhs))
      return out;
    switch (predicate) {
    case llvm::CmpInst::ICMP_ULT:
      predicate = llvm::CmpInst::ICMP_SLT;
      break;
    case llvm::CmpInst::ICMP_ULE:
      predicate = llvm::CmpInst::ICMP_SLE;
      break;
    case llvm::CmpInst::ICMP_UGT:
      predicate = llvm::CmpInst::ICMP_SGT;
      break;
    case llvm::CmpInst::ICMP_UGE:
      predicate = llvm::CmpInst::ICMP_SGE;
      break;
    default:
      break;
    }
  }

  if (lhs.isPoint() && rhs.isPoint()) {
    bool ok = false;
    if (llvm::CmpInst::isUnsigned(predicate)) {
      ok = evaluateUnsignedPredicate(predicate, static_cast<uint64_t>(*lhs.lo),
                                     static_cast<uint64_t>(*rhs.lo));
    } else {
      ok = evaluateSignedPredicate(predicate, *lhs.lo, *rhs.lo);
    }
    return ok ? out : bottomState();
  }

  Interval refinedL = lhs;
  Interval refinedR = rhs;

  switch (predicate) {
  case llvm::CmpInst::ICMP_EQ: {
    const Interval common = lhs.intersect(rhs);
    if (common.isBottom())
      return bottomState();
    refinedL = common;
    refinedR = common;
    break;
  }
  case llvm::CmpInst::ICMP_NE:
    if (lhs.isPoint() && rhs.isPoint() && *lhs.lo == *rhs.lo)
      return bottomState();
    return out;
  case llvm::CmpInst::ICMP_SLT: {
    bool lhsOverflow = false;
    bool rhsOverflow = false;
    const auto upper = decrementBound(rhs.hi, rhsOverflow);
    const auto lower = incrementBound(lhs.lo, lhsOverflow);
    if (rhsOverflow || lhsOverflow)
      return bottomState();
    refinedL = intersectWithUpper(lhs, upper);
    refinedR = intersectWithLower(rhs, lower);
    break;
  }
  case llvm::CmpInst::ICMP_SLE:
    refinedL = intersectWithUpper(lhs, rhs.hi);
    refinedR = intersectWithLower(rhs, lhs.lo);
    break;
  case llvm::CmpInst::ICMP_SGT: {
    bool lhsOverflow = false;
    bool rhsOverflow = false;
    const auto lower = incrementBound(rhs.lo, lhsOverflow);
    const auto upper = decrementBound(lhs.hi, rhsOverflow);
    if (lhsOverflow || rhsOverflow)
      return bottomState();
    refinedL = intersectWithLower(lhs, lower);
    refinedR = intersectWithUpper(rhs, upper);
    break;
  }
  case llvm::CmpInst::ICMP_SGE:
    refinedL = intersectWithLower(lhs, rhs.lo);
    refinedR = intersectWithUpper(rhs, lhs.hi);
    break;
  default:
    return out;
  }

  if (refinedL.isBottom() || refinedR.isBottom())
    return bottomState();
  out = setOperandInterval(std::move(out), lhsValue, refinedL);
  if (out.isBottom())
    return out;
  out = setOperandInterval(std::move(out), rhsValue, refinedR);
  return out;
}

IntervalState refineForCondition(IntervalState out,
                                 const llvm::Value *condition, bool truthy) {
  if (out.isBottom() || !condition)
    return out;
  if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(condition)) {
    const bool isTrue = !constant->isZero();
    return isTrue == truthy ? out : bottomState();
  }
  if (const auto *cmp = llvm::dyn_cast<llvm::ICmpInst>(condition))
    return refineForPredicate(std::move(out), *cmp, truthy);

  const Interval refined =
      getInterval(out, condition).intersect(Interval::point(truthy ? 1 : 0));
  if (refined.isBottom())
    return bottomState();
  return setOperandInterval(std::move(out), condition, refined);
}

IntervalState refineForSwitch(const Transition &t, IntervalState out,
                              const llvm::SwitchInst &switchInst) {
  const Interval condition = getInterval(out, switchInst.getCondition());
  const llvm::BasicBlock *target = t.target;
  if (!target)
    return out;

  llvm::Optional<int64_t> matchedCase;
  bool targetIsCase = false;
  for (const auto &caseHandle : switchInst.cases()) {
    if (caseHandle.getCaseSuccessor() != target)
      continue;
    targetIsCase = true;
    if (caseHandle.getCaseValue()->getBitWidth() > 64)
      return out;
    matchedCase = caseHandle.getCaseValue()->getSExtValue();
    break;
  }

  if (targetIsCase) {
    if (!matchedCase.hasValue())
      return out;
    const Interval refined = condition.intersect(Interval::point(*matchedCase));
    if (refined.isBottom())
      return bottomState();
    return setOperandInterval(std::move(out), switchInst.getCondition(),
                              refined);
  }

  if (switchInst.getDefaultDest() != target)
    return out;
  if (condition.isPoint() && condition.lo.hasValue()) {
    for (const auto &caseHandle : switchInst.cases()) {
      if (caseHandle.getCaseValue()->getBitWidth() > 64)
        continue;
      if (*condition.lo == caseHandle.getCaseValue()->getSExtValue())
        return bottomState();
    }
  }
  return out;
}

IntervalState refineForTakenEdge(const Transition &t, IntervalState out) {
  if (out.isBottom() || !t.source)
    return out;
  const llvm::Instruction *terminator = t.source->getTerminator();
  const auto *branch = llvm::dyn_cast<llvm::BranchInst>(terminator);
  if (branch && branch->isConditional()) {
    if (branch->getSuccessor(0) == t.target)
      return refineForCondition(std::move(out), branch->getCondition(), true);
    if (branch->getSuccessor(1) == t.target)
      return refineForCondition(std::move(out), branch->getCondition(), false);
    return bottomState();
  }

  const auto *switchInst = llvm::dyn_cast<llvm::SwitchInst>(terminator);
  if (switchInst)
    return refineForSwitch(t, std::move(out), *switchInst);

  return out;
}

IntervalState projectCallState(const Transition &t,
                               const IntervalState &callerState) {
  if (callerState.isBottom())
    return callerState;
  if (!t.call || !t.callee)
    return callerState;

  IntervalState projected(false);
  const llvm::Function *caller = t.source ? t.source->getParent() : nullptr;
  for (const auto &kv : callerState.intervals()) {
    if (!isFunctionLocalValue(caller, kv.first))
      projected.set(kv.first, kv.second);
  }
  for (const auto &kv : callerState.memory())
    projected.setMemory(kv.first, kv.second);

  unsigned actualIndex = 0;
  for (const llvm::Argument &formal : t.callee->args()) {
    if (actualIndex >= t.call->arg_size())
      break;
    projected.set(&formal,
                  getInterval(callerState, t.call->getArgOperand(actualIndex)));
    ++actualIndex;
  }
  return projected;
}

Interval returnedInterval(const llvm::Function &callee,
                          const IntervalState &calleeSummary) {
  Interval result = Interval::bottom();
  bool sawReturn = false;
  for (const llvm::BasicBlock &bb : callee) {
    const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
    if (!ret || ret->getNumOperands() == 0)
      continue;
    const Interval value = getInterval(calleeSummary, ret->getReturnValue());
    result = sawReturn ? result.join(value) : value;
    sawReturn = true;
  }
  return sawReturn ? result : Interval::top();
}

IntervalState mergeReturnState(const Transition &t,
                               const IntervalState &callerState,
                               const IntervalState &calleeSummary) {
  if (callerState.isBottom() || calleeSummary.isBottom())
    return bottomState();
  IntervalState out = callerState;
  const llvm::Function *callee = t.callee;
  for (const auto &kv : calleeSummary.intervals()) {
    if (!isFunctionLocalValue(callee, kv.first))
      out.set(kv.first, kv.second);
  }
  for (const auto &kv : calleeSummary.memory())
    out.setMemory(kv.first, kv.second);
  if (t.call && !t.call->getType()->isVoidTy())
    out.set(t.call, returnedInterval(*callee, calleeSummary));
  return out;
}

/// Restrict interval to signed range of \p bits (e.g. i32 -> [-2^31, 2^31-1]).
Interval restrictToSigned(const Interval &i, unsigned bits) {
  if (i.isBottom())
    return i;
  if (bits >= 64)
    return i;
  int64_t minVal = -(1LL << (bits - 1));
  int64_t maxVal = (1LL << (bits - 1)) - 1;
  llvm::Optional<int64_t> lo = i.lo.hasValue()
                                   ? std::max(*i.lo, minVal)
                                   : llvm::Optional<int64_t>(minVal);
  llvm::Optional<int64_t> hi = i.hi.hasValue()
                                   ? std::min(*i.hi, maxVal)
                                   : llvm::Optional<int64_t>(maxVal);
  if (lo && hi && *lo > *hi)
    return Interval::bottom();
  return Interval{lo, hi, false};
}

/// Restrict interval to unsigned range [0, 2^bits - 1].
Interval restrictToUnsigned(const Interval &i, unsigned bits) {
  if (i.isBottom())
    return i;
  if (bits >= 64)
    return i;
  int64_t maxVal = (bits == 64) ? INT64_MAX : ((1LL << bits) - 1);
  llvm::Optional<int64_t> lo = i.lo.hasValue() ? std::max(*i.lo, int64_t{0})
                                               : llvm::Optional<int64_t>(0);
  llvm::Optional<int64_t> hi = i.hi.hasValue()
                                   ? std::min(*i.hi, maxVal)
                                   : llvm::Optional<int64_t>(maxVal);
  if (lo && hi && *lo > *hi)
    return Interval::bottom();
  return Interval{lo, hi, false};
}

unsigned getBitWidth(const llvm::Value *V) {
  if (!V)
    return 64;
  auto *Ty = V->getType();
  if (Ty->isIntegerTy())
    return Ty->getIntegerBitWidth();
  return 64;
}

/// Transfer for a single instruction: compute result interval from operands.
Interval transferInstruction(const llvm::Instruction &I,
                             const IntervalState &state) {
  if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(&I)) {
    if (C->getBitWidth() > 64)
      return Interval::top();
    return Interval::point(C->getSExtValue());
  }

  switch (I.getOpcode()) {
  case llvm::Instruction::Add: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    return restrictToSigned(L.add(R), getBitWidth(&I));
  }
  case llvm::Instruction::Sub: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    return restrictToSigned(L.subtract(R), getBitWidth(&I));
  }
  case llvm::Instruction::Mul: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    return restrictToSigned(L.multiply(R), getBitWidth(&I));
  }
  case llvm::Instruction::SDiv: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    return restrictToSigned(L.divide(R), getBitWidth(&I));
  }
  case llvm::Instruction::UDiv: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (R.containsZero())
      return restrictToUnsigned(Interval::top(), getBitWidth(&I));
    if (L.isBottom() || R.isBottom())
      return Interval::bottom();
    if (!L.lo || !L.hi || !R.lo || !R.hi)
      return restrictToUnsigned(Interval::top(), getBitWidth(&I));
    // UDiv treats both operands as unsigned. Negative signed values are invalid
    // inputs for unsigned division; return top conservatively.
    if (*L.lo < 0 || *R.lo < 0)
      return restrictToUnsigned(Interval::top(), getBitWidth(&I));
    uint64_t u00 = static_cast<uint64_t>(*L.lo) / static_cast<uint64_t>(*R.hi);
    uint64_t u10 = static_cast<uint64_t>(*L.hi) / static_cast<uint64_t>(*R.lo);
    int64_t lo = static_cast<int64_t>(u00);
    int64_t hi = static_cast<int64_t>(u10);
    return restrictToUnsigned(Interval{lo, hi, false}, getBitWidth(&I));
  }
  case llvm::Instruction::SRem: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (R.containsZero())
      return restrictToSigned(Interval::top(), getBitWidth(&I));
    if (!R.lo || !R.hi)
      return restrictToSigned(Interval::top(), getBitWidth(&I));
    int64_t rLo = std::abs(*R.lo), rHi = std::abs(*R.hi);
    int64_t bound = std::max(rLo, rHi) - 1;
    return restrictToSigned(Interval{-bound, bound, false}, getBitWidth(&I));
  }
  case llvm::Instruction::URem: {
    auto R = getInterval(state, I.getOperand(1));
    if (R.containsZero())
      return restrictToUnsigned(Interval::top(), getBitWidth(&I));
    unsigned w = getBitWidth(&I);
    int64_t maxVal = (w >= 64) ? INT64_MAX : ((1LL << w) - 1);
    return restrictToUnsigned(Interval{0, maxVal, false}, w);
  }
  case llvm::Instruction::Shl: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (L.isBottom() || R.isBottom())
      return Interval::bottom();
    if (!L.lo || !L.hi || !R.lo || !R.hi)
      return restrictToSigned(Interval::top(), getBitWidth(&I));
    // Shift amount must be in [0, 63]; negative or out-of-range => top.
    if (*R.lo < 0 || *R.hi > 63)
      return restrictToSigned(Interval::top(), getBitWidth(&I));
    // Use unsigned arithmetic to avoid signed-shift UB.
    unsigned w = getBitWidth(&I);
    int64_t shAmtLo = *R.lo, shAmtHi = *R.hi;
    // Compute all four corner products using unsigned shift, then re-interpret.
    auto ushl = [&](int64_t val, int64_t amt) -> int64_t {
      return static_cast<int64_t>(static_cast<uint64_t>(val)
                                  << static_cast<unsigned>(amt));
    };
    int64_t v00 = ushl(*L.lo, shAmtLo), v01 = ushl(*L.lo, shAmtHi);
    int64_t v10 = ushl(*L.hi, shAmtLo), v11 = ushl(*L.hi, shAmtHi);
    return restrictToSigned(Interval{std::min({v00, v01, v10, v11}),
                                     std::max({v00, v01, v10, v11}), false},
                            w);
  }
  case llvm::Instruction::LShr: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (L.isBottom() || R.isBottom())
      return Interval::bottom();
    if (!L.lo || !L.hi || !R.lo || !R.hi)
      return restrictToUnsigned(Interval::top(), getBitWidth(&I));
    // Shift amount must be in [0, 63].
    if (*R.lo < 0 || *R.hi > 63)
      return restrictToUnsigned(Interval::top(), getBitWidth(&I));
    unsigned w = getBitWidth(&I);
    // LShr treats the value as unsigned. If the interval straddles the sign
    // boundary (lo < 0 <= hi), the unsigned representation is non-contiguous
    // ([lo_unsigned, UINT_MAX] ∪ [0, hi_unsigned]), so we cannot represent the
    // result precisely. Return top conservatively.
    if (*L.lo < 0)
      return restrictToUnsigned(Interval::top(), w);
    // Both bounds are non-negative: safe to treat as unsigned.
    uint64_t uLo = static_cast<uint64_t>(*L.lo);
    uint64_t uHi = static_cast<uint64_t>(*L.hi);
    // Minimum shift gives maximum result; maximum shift gives minimum result.
    uint64_t shAmtLo = static_cast<uint64_t>(*R.lo);
    uint64_t shAmtHi = static_cast<uint64_t>(*R.hi);
    uint64_t rLo = uHi >> shAmtHi; // smallest: largest value >> largest shift
    uint64_t rHi = uLo >> shAmtLo; // largest: smallest value >> smallest shift
    // Correct ordering: uLo <= uHi and shAmtLo <= shAmtHi, so rLo <= rHi.
    if (rLo > rHi)
      std::swap(rLo, rHi);
    return restrictToUnsigned(
        Interval{static_cast<int64_t>(rLo), static_cast<int64_t>(rHi), false},
        w);
  }
  case llvm::Instruction::AShr: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (L.isBottom() || R.isBottom())
      return Interval::bottom();
    if (!L.lo || !L.hi || !R.lo || !R.hi)
      return restrictToSigned(Interval::top(), getBitWidth(&I));
    if (*R.hi < 0 || *R.lo > 63)
      return restrictToSigned(Interval::top(), getBitWidth(&I));
    int64_t shAmt = std::min(*R.lo, int64_t{63});
    int64_t vLo = *L.lo >> shAmt, vHi = *L.hi >> shAmt;
    return restrictToSigned(
        Interval{std::min(vLo, vHi), std::max(vLo, vHi), false},
        getBitWidth(&I));
  }
  case llvm::Instruction::And: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (L.isBottom() || R.isBottom())
      return Interval::bottom();
    unsigned w = getBitWidth(&I);
    if (w > 64)
      return Interval::top();
    int64_t maxVal = (w == 64) ? INT64_MAX : ((1LL << w) - 1);
    if (!L.lo || !L.hi || !R.lo || !R.hi)
      return restrictToUnsigned(Interval{0, maxVal, false}, w);
    int64_t lo = 0, hi = maxVal;
    if (*L.lo >= 0 && *L.hi >= 0 && *R.lo >= 0 && *R.hi >= 0) {
      uint64_t u = static_cast<uint64_t>(*L.hi) & static_cast<uint64_t>(*R.hi);
      hi = static_cast<int64_t>(std::min(u, static_cast<uint64_t>(maxVal)));
    }
    return restrictToUnsigned(Interval{lo, hi, false}, w);
  }
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (L.isBottom() || R.isBottom())
      return Interval::bottom();
    unsigned w = getBitWidth(&I);
    return restrictToUnsigned(Interval::top(), w);
  }
  case llvm::Instruction::Trunc: {
    auto Op = getInterval(state, I.getOperand(0));
    unsigned w = I.getType()->getIntegerBitWidth();
    return restrictToUnsigned(Op, w);
  }
  case llvm::Instruction::ZExt: {
    auto Op = getInterval(state, I.getOperand(0));
    unsigned wIn = I.getOperand(0)->getType()->getIntegerBitWidth();
    unsigned wOut = getBitWidth(&I);
    if (Op.isBottom())
      return Interval::bottom();
    if (!Op.lo || !Op.hi)
      return restrictToUnsigned(Interval::top(), wOut);
    if (*Op.lo >= 0 && *Op.hi >= 0)
      return restrictToUnsigned(Interval{*Op.lo, *Op.hi, false}, wOut);
    int64_t maxIn = (wIn >= 64) ? INT64_MAX : ((1LL << wIn) - 1);
    return restrictToUnsigned(Interval{0, maxIn, false}, wOut);
  }
  case llvm::Instruction::SExt: {
    auto Op = getInterval(state, I.getOperand(0));
    return restrictToSigned(Op, getBitWidth(&I));
  }
  case llvm::Instruction::PtrToInt:
  case llvm::Instruction::IntToPtr:
  case llvm::Instruction::BitCast:
    return restrictToSigned(getInterval(state, I.getOperand(0)),
                            getBitWidth(&I));

  case llvm::Instruction::ICmp: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (L.isBottom() || R.isBottom())
      return Interval::bottom();
    auto *Cmp = llvm::cast<llvm::CmpInst>(&I);
    bool isSigned = Cmp->isSigned();
    switch (Cmp->getPredicate()) {
    case llvm::CmpInst::ICMP_EQ:
      if (L.isPoint() && R.isPoint() && *L.lo == *R.lo)
        return Interval::point(1);
      if (!L.intersect(R).isBottom())
        return Interval{0, 1, false};
      return Interval::point(0);
    case llvm::CmpInst::ICMP_NE:
      if (L.isPoint() && R.isPoint() && *L.lo == *R.lo)
        return Interval::point(0);
      if (!L.intersect(R).isBottom())
        return Interval{0, 1, false};
      return Interval::point(1);
    case llvm::CmpInst::ICMP_SLT:
    case llvm::CmpInst::ICMP_ULT:
    case llvm::CmpInst::ICMP_SGT:
    case llvm::CmpInst::ICMP_UGT:
    case llvm::CmpInst::ICMP_SLE:
    case llvm::CmpInst::ICMP_ULE:
    case llvm::CmpInst::ICMP_SGE:
    case llvm::CmpInst::ICMP_UGE:
      return Interval{0, 1, false};
    default:
      return Interval::top();
    }
  }
  case llvm::Instruction::Select: {
    auto Cond = getInterval(state, I.getOperand(0));
    auto TrueVal = getInterval(state, I.getOperand(1));
    auto FalseVal = getInterval(state, I.getOperand(2));
    if (TrueVal.isBottom() && FalseVal.isBottom())
      return Interval::bottom();
    if (Cond.containsZero() && Cond.isPoint() && Cond.lo && *Cond.lo != 0)
      return TrueVal;
    if (Cond.containsZero() && Cond.isPoint() && Cond.lo && *Cond.lo == 0)
      return FalseVal;
    return TrueVal.join(FalseVal);
  }
  case llvm::Instruction::PHI: {
    auto *Phi = llvm::cast<llvm::PHINode>(&I);
    Interval acc = Interval::bottom();
    for (unsigned i = 0, e = Phi->getNumIncomingValues(); i < e; ++i) {
      Interval in = getInterval(state, Phi->getIncomingValue(i));
      acc = acc.isBottom() ? in : acc.join(in);
    }
    return acc.isBottom() ? Interval::top() : acc;
  }
  case llvm::Instruction::Alloca:
  case llvm::Instruction::Load:
  case llvm::Instruction::GetElementPtr:
  case llvm::Instruction::Call:
  case llvm::Instruction::Invoke:
  default:
    return Interval::top();
  }
}

} // namespace

IntervalState
IntervalDomain::applyBlockTransfer(llvm::BasicBlock *bb,
                                   const IntervalState &in) const {
  return applyBlockTransfer(bb, in, nullptr, nullptr);
}

IntervalState
IntervalDomain::applyBlockTransfer(llvm::BasicBlock *bb,
                                   const IntervalState &in,
                                   const llvm::Instruction *segmentStart,
                                   const llvm::Instruction *stopBefore) const {
  if (in.isBottom())
    return in;
  IntervalState out(false);
  for (const auto &kv : in.intervals())
    out.set(kv.first, kv.second);
  for (const auto &kv : in.memory())
    out.setMemory(kv.first, kv.second);

  lotus::AliasAnalysisWrapper *AA = getAliasAnalysis();
  const llvm::Function &F = *bb->getParent();
  std::vector<const llvm::Value *> regions;
  std::vector<const llvm::Value *> resolved;
  if (AA)
    regions = getRegionsForFunction(F);

  for (llvm::BasicBlock::const_iterator it = segmentBegin(bb, segmentStart),
                                        end = bb->end();
       it != end; ++it) {
    const llvm::Instruction &I = *it;
    if (&I == stopBefore || I.isTerminator())
      break;
    if (I.getType()->isVoidTy()) {
      if (AA && llvm::isa<llvm::StoreInst>(&I)) {
        auto *SI = llvm::cast<llvm::StoreInst>(&I);
        const llvm::Value *ptr = SI->getPointerOperand();
        Interval val = getInterval(out, SI->getValueOperand());
        resolvePointerToRegions(AA, ptr, regions, resolved);
        for (const llvm::Value *r : resolved) {
          auto cur = out.getMemory(r);
          out.setMemory(r, cur ? cur->join(val) : val);
        }
      }
      continue;
    }
    if (I.getType()->isIntegerTy() || I.getType()->isPointerTy()) {
      Interval res;
      if (AA && llvm::isa<llvm::AllocaInst>(&I)) {
        out.setMemory(&I, Interval::top());
        res = Interval::top();
      } else if (AA && llvm::isa<llvm::LoadInst>(&I) &&
                 I.getType()->isIntegerTy()) {
        const llvm::Value *ptr =
            llvm::cast<llvm::LoadInst>(&I)->getPointerOperand();
        resolvePointerToRegions(AA, ptr, regions, resolved);
        res = Interval::bottom();
        for (const llvm::Value *r : resolved) {
          auto cur = out.getMemory(r);
          Interval ir = cur ? *cur : Interval::top();
          res = res.isBottom() ? ir : res.join(ir);
        }
        if (res.isBottom())
          res = Interval::top();
      } else if (AA && llvm::isa<llvm::GetElementPtrInst>(&I)) {
        res = Interval::top();
      } else {
        res = transferInstruction(I, out);
      }
      out.set(&I, std::move(res));
    }
  }
  return out;
}

IntervalState
IntervalDomain::applyBlockWiseHavoc(llvm::BasicBlock *bb,
                                    const IntervalState &in) const {
  return applyBlockWiseHavoc(bb, in, nullptr, nullptr);
}

IntervalState
IntervalDomain::applyBlockWiseHavoc(llvm::BasicBlock *bb,
                                    const IntervalState &in,
                                    const llvm::Instruction *segmentStart,
                                    const llvm::Instruction *stopBefore) const {
  if (in.isBottom())
    return in;
  IntervalState out(false);
  for (const auto &kv : in.intervals())
    out.set(kv.first, kv.second);
  for (const auto &kv : in.memory())
    out.setMemory(kv.first, kv.second);
  for (llvm::BasicBlock::const_iterator it = segmentBegin(bb, segmentStart),
                                        end = bb->end();
       it != end; ++it) {
    const llvm::Instruction &I = *it;
    if (&I == stopBefore || I.isTerminator())
      break;
    if (I.getType()->isVoidTy())
      continue;
    if (I.getType()->isIntegerTy() || I.getType()->isPointerTy())
      out.set(&I, Interval::top());
  }
  return out;
}

IntervalState IntervalDomain::post(const Transition &t,
                                   const IntervalState &in) const {
  if (in.isBottom())
    return in;
  if (t.kind == TransitionKind::Marker)
    return in;
  if (t.kind == TransitionKind::EnterCall)
    return in;
  if (t.kind == TransitionKind::ReturnSummary)
    return in;
  if (t.kind != TransitionKind::Edge || !t.source)
    return in;
  IntervalState out =
      blockTransferPolicy_ && blockTransferPolicy_->useBlockWise(t.source)
          ? applyBlockWiseHavoc(t.source, in, t.segmentStart, t.stopBefore)
          : applyBlockTransfer(t.source, in, t.segmentStart, t.stopBefore);
  out = refineForTakenEdge(t, std::move(out));
  return applyIncomingPhis(t, std::move(out));
}

IntervalState IntervalDomain::postCall(const Transition &t,
                                       const IntervalState &callerState) const {
  return projectCallState(t, callerState);
}

IntervalState
IntervalDomain::postReturn(const Transition &t,
                           const IntervalState &callerState,
                           const IntervalState &calleeSummary) const {
  return mergeReturnState(t, callerState, calleeSummary);
}

void IntervalState::print(llvm::raw_ostream &out) const {
  if (isBottom_) {
    out << "  (bottom)\n";
    return;
  }
  for (const auto &kv : intervals_) {
    const llvm::Value *V = kv.first;
    std::string name = V->getName().str();
    if (name.empty()) {
      llvm::raw_string_ostream os(name);
      V->print(os);
      if (name.size() > 40)
        name = name.substr(0, 37) + "...";
    }
    const Interval &i = kv.second;
    if (i.isBottom())
      out << "  " << name << " : bottom\n";
    else if (i.isTop())
      out << "  " << name << " : [-inf, +inf]\n";
    else {
      out << "  " << name << " : [";
      if (i.lo.hasValue())
        out << *i.lo;
      else
        out << "-inf";
      out << ", ";
      if (i.hi.hasValue())
        out << *i.hi;
      else
        out << "+inf";
      out << "]\n";
    }
  }
  for (const auto &kv : memory_) {
    const llvm::Value *R = kv.first;
    std::string regName = R->getName().str();
    if (regName.empty()) {
      llvm::raw_string_ostream os(regName);
      R->print(os);
      if (regName.size() > 40)
        regName = regName.substr(0, 37) + "...";
    }
    const Interval &i = kv.second;
    out << "  mem(" << regName << ") : ";
    if (i.isBottom())
      out << "bottom\n";
    else if (i.isTop())
      out << "[-inf, +inf]\n";
    else
      out << "[" << (i.lo.hasValue() ? std::to_string(*i.lo) : "-inf") << ", "
          << (i.hi.hasValue() ? std::to_string(*i.hi) : "+inf") << "]\n";
  }
}
