//===-- Verification/Sifa/Domain/OctagonDomain.cpp ------------------------===//
//
// Instruction-level block transfer for Sifa Octagon domain.
// Applies sound over-approximating transfer: copy/constant/affine assignments
// update octagon constraints; non-linear ops and memory havoc the result.
// When alias analysis is set, Load/Store use region-based memory (IKOS/CLAM
// style).
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Domain/OctagonDomain.h"

#include "llvm/ADT/Optional.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Verification/Sifa/RegionMemory.h"

#include <limits>
#include <unordered_map>

using namespace lotus::sifa;

namespace {

OctagonState addVarUnconstrained(const OctagonState &s, const llvm::Value *v);
llvm::Optional<std::size_t> getVarIndex(const OctagonState &s,
                                        const llvm::Value *v);
Interval intervalForValue(const OctagonState &s, const llvm::Value *v);
OctagonState assignCopy(const OctagonState &s, const llvm::Value *res,
                        const llvm::Value *src);
OctagonState assignConstant(const OctagonState &s, const llvm::Value *res,
                            int64_t c);
OctagonState havocVar(const OctagonState &s, const llvm::Value *v);
llvm::Optional<int64_t> getConstant(const llvm::Value *V);
OctagonState assignInterval(const OctagonState &s, const llvm::Value *res,
                            const Interval &interval);
bool isFunctionLocalValue(const llvm::Function *F, const llvm::Value *v);
OctagonState refineForTakenEdge(const Transition &t, OctagonState out);
OctagonState projectCallState(const Transition &t,
                              const OctagonState &callerState);
OctagonState mergeReturnState(const Transition &t,
                              const OctagonState &callerState,
                              const OctagonState &calleeSummary);

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

OctagonState applyIncomingPhis(const Transition &t, OctagonState out) {
  if (out.isBottom())
    return out;
  if (!t.source || !t.target || !t.landsAtBlockEntry())
    return out;
  for (const llvm::Instruction &I : *t.target) {
    const auto *phi = llvm::dyn_cast<llvm::PHINode>(&I);
    if (!phi)
      break;
    const llvm::Value *incoming = incomingValueForPredecessor(*phi, t.source);
    out = addVarUnconstrained(out, phi);
    const Interval incomingInterval = intervalForValue(out, incoming);
    if (incomingInterval.isBottom()) {
      return OctagonState(true);
    }
    if (!incomingInterval.isTop()) {
      out = assignInterval(out, phi, incomingInterval);
    } else if (incoming && getVarIndex(out, incoming)) {
      out = assignCopy(out, phi, incoming);
    } else {
      out = havocVar(out, phi);
    }
  }
  return out;
}

/// Add variable \p v to state with no constraints (top for that variable).
OctagonState addVarUnconstrained(const OctagonState &s, const llvm::Value *v) {
  auto varToIndex = s.varToIndex();
  if (varToIndex.count(v))
    return s;
  const std::size_t n = varToIndex.size();
  std::unordered_map<const llvm::Value *, std::size_t> newVarToIndex(
      varToIndex);
  newVarToIndex[v] = n;

  OctagonMatrix newMat(n + 1);
  const OctagonMatrix &old = s.matrix();
  for (std::size_t i = 0; i < old.dim(); ++i)
    for (std::size_t j = 0; j < old.dim(); ++j) {
      auto c = old.get(i, j);
      if (c)
        newMat.set(i, j, *c);
    }
  return OctagonState(std::move(newVarToIndex), std::move(newMat),
                      s.isBottom());
}

/// Get index of \p v in state, or None if not present.
llvm::Optional<std::size_t> getVarIndex(const OctagonState &s,
                                        const llvm::Value *v) {
  auto it = s.varToIndex().find(v);
  if (it == s.varToIndex().end())
    return llvm::None;
  return it->second;
}

int64_t floorDiv(int64_t numerator, int64_t denominator) {
  int64_t quotient = numerator / denominator;
  int64_t remainder = numerator % denominator;
  if (remainder != 0 && ((remainder > 0) != (denominator > 0)))
    --quotient;
  return quotient;
}

Interval intervalForValue(const OctagonState &s, const llvm::Value *v) {
  if (!v)
    return Interval::top();
  if (const auto constant = getConstant(v))
    return Interval::point(*constant);
  auto index = getVarIndex(s, v);
  if (!index)
    return Interval::top();
  const OctagonMatrix closed = s.matrix().strongClosure();
  llvm::Optional<int64_t> lower;
  llvm::Optional<int64_t> upper;
  if (auto lo = closed.get(2 * *index, 2 * *index + 1))
    lower = -floorDiv(*lo, 2);
  if (auto hi = closed.get(2 * *index + 1, 2 * *index))
    upper = floorDiv(*hi, 2);
  if (lower && upper && *lower > *upper)
    return Interval::bottom();
  return Interval{lower, upper, false};
}

/// Assign res = src (both must be in state). Octagon: res - src ≤ 0 and src -
/// res ≤ 0.
OctagonState assignCopy(const OctagonState &s, const llvm::Value *res,
                        const llvm::Value *src) {
  auto ri = getVarIndex(s, res);
  auto si = getVarIndex(s, src);
  if (!ri || !si)
    return s;
  std::size_t r = *ri, srcIdx = *si;
  OctagonMatrix m = s.matrix();
  m.set(2 * r, 2 * srcIdx + 1, 0);
  m.set(2 * srcIdx, 2 * r + 1, 0);
  m = m.strongClosure();
  if (m.hasNegativeSelfLoop())
    return OctagonState(true);
  return OctagonState(s.varToIndex(), std::move(m), false);
}

/// Assign res = c (constant). Octagon: 2*res ≤ 2c and -2*res ≤ -2c.
OctagonState assignConstant(const OctagonState &s, const llvm::Value *res,
                            int64_t c) {
  auto ri = getVarIndex(s, res);
  if (!ri)
    return s;
  std::size_t r = *ri;
  int64_t twoC;
  if (__builtin_mul_overflow(c, 2, &twoC))
    return s;
  OctagonMatrix m = s.matrix();
  m.set(2 * r + 1, 2 * r, twoC);
  m.set(2 * r, 2 * r + 1, -twoC);
  m = m.strongClosure();
  if (m.hasNegativeSelfLoop())
    return OctagonState(true);
  return OctagonState(s.varToIndex(), std::move(m), false);
}

/// Assign res = src + k (affine). Octagon: res - src ≤ k and src - res ≤ -k.
OctagonState assignAffine(const OctagonState &s, const llvm::Value *res,
                          const llvm::Value *src, int64_t k) {
  auto ri = getVarIndex(s, res);
  auto si = getVarIndex(s, src);
  if (!ri || !si)
    return s;
  std::size_t r = *ri, srcIdx = *si;
  OctagonMatrix m = s.matrix();
  m.set(2 * r, 2 * srcIdx + 1, k);
  m.set(2 * srcIdx, 2 * r + 1, -k);
  m = m.strongClosure();
  if (m.hasNegativeSelfLoop())
    return OctagonState(true);
  return OctagonState(s.varToIndex(), std::move(m), false);
}

/// Havoc variable \p v: relax all constraints involving v (sound
/// over-approximation).
OctagonState havocVar(const OctagonState &s, const llvm::Value *v) {
  auto it = s.varToIndex().find(v);
  if (it == s.varToIndex().end())
    return s;
  OctagonMatrix m = s.matrix().relaxVar(it->second);
  if (m.hasNegativeSelfLoop())
    return OctagonState(true);
  return OctagonState(s.varToIndex(), std::move(m), false);
}

/// Get constant from \p V if ConstantInt (fits in 64 bits), else None.
llvm::Optional<int64_t> getConstant(const llvm::Value *V) {
  const auto *C = llvm::dyn_cast<llvm::ConstantInt>(V);
  if (!C || C->getBitWidth() > 64)
    return llvm::None;
  return C->getSExtValue();
}

bool isFunctionLocalValue(const llvm::Function *F, const llvm::Value *v) {
  if (!F || !v)
    return false;
  if (const auto *I = llvm::dyn_cast<llvm::Instruction>(v))
    return I->getFunction() == F;
  if (const auto *A = llvm::dyn_cast<llvm::Argument>(v))
    return A->getParent() == F;
  return false;
}

/// Constrain result variable to interval [lo, hi]. Point => assignConstant; top
/// => havoc.
OctagonState assignInterval(const OctagonState &s, const llvm::Value *res,
                            const Interval &interval) {
  auto ri = getVarIndex(s, res);
  if (!ri)
    return s;
  std::size_t r = *ri;
  if (interval.isBottom())
    return OctagonState(true);
  if (interval.isPoint() && interval.lo)
    return assignConstant(s, res, *interval.lo);
  if (interval.isTop())
    return havocVar(s, res);
  OctagonMatrix m = s.matrix();
  if (interval.hi) {
    int64_t twoHi;
    if (__builtin_mul_overflow(*interval.hi, 2, &twoHi))
      return havocVar(s, res);
    m.set(2 * r + 1, 2 * r, twoHi);
  }
  if (interval.lo) {
    int64_t twoLo;
    if (__builtin_mul_overflow(*interval.lo, 2, &twoLo))
      return havocVar(s, res);
    m.set(2 * r, 2 * r + 1, -twoLo);
  }
  m = m.strongClosure();
  if (m.hasNegativeSelfLoop())
    return OctagonState(true);
  return OctagonState(s.varToIndex(), std::move(m), false);
}

bool evaluatePredicate(llvm::CmpInst::Predicate predicate, int64_t lhs,
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
  case llvm::CmpInst::ICMP_ULT:
    return static_cast<uint64_t>(lhs) < static_cast<uint64_t>(rhs);
  case llvm::CmpInst::ICMP_ULE:
    return static_cast<uint64_t>(lhs) <= static_cast<uint64_t>(rhs);
  case llvm::CmpInst::ICMP_UGT:
    return static_cast<uint64_t>(lhs) > static_cast<uint64_t>(rhs);
  case llvm::CmpInst::ICMP_UGE:
    return static_cast<uint64_t>(lhs) >= static_cast<uint64_t>(rhs);
  default:
    return false;
  }
}

OctagonState addDifferenceConstraint(const OctagonState &s,
                                     const llvm::Value *lhs,
                                     const llvm::Value *rhs, int64_t bound) {
  auto lhsIndex = getVarIndex(s, lhs);
  auto rhsIndex = getVarIndex(s, rhs);
  if (!lhsIndex || !rhsIndex)
    return s;
  OctagonMatrix matrix = s.matrix();
  matrix.set(2 * *lhsIndex, 2 * *rhsIndex + 1, bound);
  matrix = matrix.strongClosure();
  if (matrix.hasNegativeSelfLoop())
    return OctagonState(true);
  return OctagonState(s.varToIndex(), std::move(matrix), false);
}

OctagonState constrainToInterval(OctagonState state, const llvm::Value *value,
                                 const Interval &interval) {
  if (!value || llvm::isa<llvm::Constant>(value))
    return state;
  state = addVarUnconstrained(state, value);
  return assignInterval(state, value, interval);
}

OctagonState refineForPredicate(OctagonState out, const llvm::ICmpInst &cmp,
                                bool truthy) {
  const llvm::Value *lhsValue = cmp.getOperand(0);
  const llvm::Value *rhsValue = cmp.getOperand(1);
  const Interval lhs = intervalForValue(out, lhsValue);
  const Interval rhs = intervalForValue(out, rhsValue);
  if (lhs.isBottom() || rhs.isBottom())
    return OctagonState(true);

  llvm::CmpInst::Predicate predicate = cmp.getPredicate();
  if (!truthy)
    predicate = cmp.getInversePredicate();
  if (llvm::CmpInst::isUnsigned(predicate) &&
      ((!lhs.lo.hasValue() || *lhs.lo < 0) ||
       (!rhs.lo.hasValue() || *rhs.lo < 0))) {
    return out;
  }

  if (lhs.isPoint() && rhs.isPoint()) {
    return evaluatePredicate(predicate, *lhs.lo, *rhs.lo) ? out
                                                          : OctagonState(true);
  }

  switch (predicate) {
  case llvm::CmpInst::ICMP_EQ:
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(lhsValue))
      return constrainToInterval(std::move(out), rhsValue,
                                 Interval::point(constant->getSExtValue()));
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(rhsValue))
      return constrainToInterval(std::move(out), lhsValue,
                                 Interval::point(constant->getSExtValue()));
    out = addVarUnconstrained(out, lhsValue);
    out = addVarUnconstrained(out, rhsValue);
    out = addDifferenceConstraint(out, lhsValue, rhsValue, 0);
    return addDifferenceConstraint(out, rhsValue, lhsValue, 0);
  case llvm::CmpInst::ICMP_NE:
    if (lhs.isPoint() && rhs.isPoint() && lhs.lo == rhs.lo)
      return OctagonState(true);
    return out;
  case llvm::CmpInst::ICMP_SLT:
  case llvm::CmpInst::ICMP_ULT:
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(rhsValue)) {
      if (constant->getSExtValue() == std::numeric_limits<int64_t>::min())
        return OctagonState(true);
      return constrainToInterval(
          std::move(out), lhsValue,
          Interval{llvm::None, constant->getSExtValue() - 1, false});
    }
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(lhsValue)) {
      if (constant->getSExtValue() == std::numeric_limits<int64_t>::max())
        return OctagonState(true);
      return constrainToInterval(
          std::move(out), rhsValue,
          Interval{constant->getSExtValue() + 1, llvm::None, false});
    }
    out = addVarUnconstrained(out, lhsValue);
    out = addVarUnconstrained(out, rhsValue);
    return addDifferenceConstraint(out, lhsValue, rhsValue, -1);
  case llvm::CmpInst::ICMP_SLE:
  case llvm::CmpInst::ICMP_ULE:
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(rhsValue))
      return constrainToInterval(
          std::move(out), lhsValue,
          Interval{llvm::None, constant->getSExtValue(), false});
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(lhsValue))
      return constrainToInterval(
          std::move(out), rhsValue,
          Interval{constant->getSExtValue(), llvm::None, false});
    out = addVarUnconstrained(out, lhsValue);
    out = addVarUnconstrained(out, rhsValue);
    return addDifferenceConstraint(out, lhsValue, rhsValue, 0);
  case llvm::CmpInst::ICMP_SGT:
  case llvm::CmpInst::ICMP_UGT:
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(rhsValue)) {
      if (constant->getSExtValue() == std::numeric_limits<int64_t>::max())
        return OctagonState(true);
      return constrainToInterval(
          std::move(out), lhsValue,
          Interval{constant->getSExtValue() + 1, llvm::None, false});
    }
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(lhsValue)) {
      if (constant->getSExtValue() == std::numeric_limits<int64_t>::min())
        return OctagonState(true);
      return constrainToInterval(
          std::move(out), rhsValue,
          Interval{llvm::None, constant->getSExtValue() - 1, false});
    }
    out = addVarUnconstrained(out, lhsValue);
    out = addVarUnconstrained(out, rhsValue);
    return addDifferenceConstraint(out, rhsValue, lhsValue, -1);
  case llvm::CmpInst::ICMP_SGE:
  case llvm::CmpInst::ICMP_UGE:
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(rhsValue))
      return constrainToInterval(
          std::move(out), lhsValue,
          Interval{constant->getSExtValue(), llvm::None, false});
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(lhsValue))
      return constrainToInterval(
          std::move(out), rhsValue,
          Interval{llvm::None, constant->getSExtValue(), false});
    out = addVarUnconstrained(out, lhsValue);
    out = addVarUnconstrained(out, rhsValue);
    return addDifferenceConstraint(out, rhsValue, lhsValue, 0);
  default:
    return out;
  }
}

OctagonState refineForCondition(OctagonState out, const llvm::Value *condition,
                                bool truthy) {
  if (out.isBottom() || !condition)
    return out;
  if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(condition))
    return (!constant->isZero() == truthy) ? out : OctagonState(true);
  if (const auto *cmp = llvm::dyn_cast<llvm::ICmpInst>(condition))
    return refineForPredicate(std::move(out), *cmp, truthy);
  return constrainToInterval(std::move(out), condition,
                             Interval::point(truthy ? 1 : 0));
}

OctagonState refineForSwitch(const Transition &t, OctagonState out,
                             const llvm::SwitchInst &switchInst) {
  const Interval condition = intervalForValue(out, switchInst.getCondition());
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
    if (condition.isPoint() && condition.lo.hasValue() &&
        *condition.lo != *matchedCase)
      return OctagonState(true);
    return constrainToInterval(std::move(out), switchInst.getCondition(),
                               Interval::point(*matchedCase));
  }

  if (switchInst.getDefaultDest() != target)
    return out;
  if (condition.isPoint() && condition.lo.hasValue()) {
    for (const auto &caseHandle : switchInst.cases()) {
      if (caseHandle.getCaseValue()->getBitWidth() > 64)
        continue;
      if (*condition.lo == caseHandle.getCaseValue()->getSExtValue())
        return OctagonState(true);
    }
  }
  return out;
}

OctagonState refineForTakenEdge(const Transition &t, OctagonState out) {
  if (out.isBottom() || !t.source)
    return out;
  const llvm::Instruction *terminator = t.source->getTerminator();
  if (const auto *branch = llvm::dyn_cast<llvm::BranchInst>(terminator)) {
    if (!branch->isConditional())
      return out;
    if (branch->getSuccessor(0) == t.target)
      return refineForCondition(std::move(out), branch->getCondition(), true);
    if (branch->getSuccessor(1) == t.target)
      return refineForCondition(std::move(out), branch->getCondition(), false);
    return OctagonState(true);
  }
  if (const auto *switchInst = llvm::dyn_cast<llvm::SwitchInst>(terminator))
    return refineForSwitch(t, std::move(out), *switchInst);
  return out;
}

OctagonState projectCallState(const Transition &t,
                              const OctagonState &callerState) {
  if (callerState.isBottom() || !t.call || !t.callee)
    return callerState;
  OctagonState projected;
  const llvm::Function *caller = t.source ? t.source->getParent() : nullptr;
  for (const auto &kv : callerState.varToIndex()) {
    if (!isFunctionLocalValue(caller, kv.first))
      projected = constrainToInterval(std::move(projected), kv.first,
                                      intervalForValue(callerState, kv.first));
  }
  for (const auto &kv : callerState.memory())
    projected.setMemory(kv.first, kv.second);
  unsigned actualIndex = 0;
  for (const llvm::Argument &formal : t.callee->args()) {
    if (actualIndex >= t.call->arg_size())
      break;
    projected = constrainToInterval(
        std::move(projected), &formal,
        intervalForValue(callerState, t.call->getArgOperand(actualIndex)));
    ++actualIndex;
  }
  return projected;
}

Interval returnedInterval(const llvm::Function &callee,
                          const OctagonState &calleeSummary) {
  Interval result = Interval::bottom();
  bool sawReturn = false;
  for (const llvm::BasicBlock &bb : callee) {
    const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
    if (!ret || ret->getNumOperands() == 0)
      continue;
    const Interval value =
        intervalForValue(calleeSummary, ret->getReturnValue());
    result = sawReturn ? result.join(value) : value;
    sawReturn = true;
  }
  return sawReturn ? result : Interval::top();
}

OctagonState mergeReturnState(const Transition &t,
                              const OctagonState &callerState,
                              const OctagonState &calleeSummary) {
  if (callerState.isBottom() || calleeSummary.isBottom())
    return OctagonState(true);
  OctagonState out = callerState;
  const llvm::Function *callee = t.callee;
  for (const auto &kv : calleeSummary.varToIndex()) {
    if (!isFunctionLocalValue(callee, kv.first))
      out = constrainToInterval(std::move(out), kv.first,
                                intervalForValue(calleeSummary, kv.first));
  }
  for (const auto &kv : calleeSummary.memory())
    out.setMemory(kv.first, kv.second);
  if (t.call && !t.call->getType()->isVoidTy())
    out = constrainToInterval(std::move(out), t.call,
                              returnedInterval(*callee, calleeSummary));
  return out;
}

} // namespace

OctagonState OctagonDomain::applyBlockTransfer(llvm::BasicBlock *bb,
                                               const OctagonState &in) const {
  return applyBlockTransfer(bb, in, nullptr, nullptr);
}

OctagonState
OctagonDomain::applyBlockTransfer(llvm::BasicBlock *bb, const OctagonState &in,
                                  const llvm::Instruction *segmentStart,
                                  const llvm::Instruction *stopBefore) const {
  if (in.isBottom())
    return in;
  OctagonState out = in;

  lotus::AliasAnalysisWrapper *AA = getAliasAnalysis();
  std::vector<const llvm::Value *> regions;
  std::vector<const llvm::Value *> resolved;
  if (AA)
    regions = getRegionsForFunction(*bb->getParent());

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
        Interval val = Interval::top(); // Conservative: don't extract interval
                                        // from octagon yet
        resolvePointerToRegions(AA, ptr, regions, resolved);
        for (const llvm::Value *r : resolved) {
          auto cur = out.getMemory(r);
          out.setMemory(r, cur ? cur->join(val) : val);
        }
      }
      continue;
    }
    if (!I.getType()->isIntegerTy() && !I.getType()->isPointerTy())
      continue;

    const llvm::Value *res = &I;
    if (AA && llvm::isa<llvm::AllocaInst>(&I)) {
      out = addVarUnconstrained(out, res);
      out.setMemory(&I, Interval::top());
      continue;
    }
    if (AA && llvm::isa<llvm::LoadInst>(&I) && I.getType()->isIntegerTy()) {
      const llvm::Value *ptr =
          llvm::cast<llvm::LoadInst>(&I)->getPointerOperand();
      resolvePointerToRegions(AA, ptr, regions, resolved);
      Interval loadVal = Interval::bottom();
      for (const llvm::Value *r : resolved) {
        auto cur = out.getMemory(r);
        Interval ir = cur ? *cur : Interval::top();
        loadVal = loadVal.isBottom() ? ir : loadVal.join(ir);
      }
      if (loadVal.isBottom())
        loadVal = Interval::top();
      out = addVarUnconstrained(out, res);
      out = assignInterval(out, res, loadVal);
      continue;
    }
    if (AA && llvm::isa<llvm::GetElementPtrInst>(&I)) {
      out = addVarUnconstrained(out, res);
      out = havocVar(out, res);
      continue;
    }
    switch (I.getOpcode()) {
    case llvm::Instruction::PHI:
      out = addVarUnconstrained(out, res);
      out = havocVar(out, res);
      break;
    case llvm::Instruction::Select: {
      out = addVarUnconstrained(out, res);
      out = havocVar(out, res);
      break;
    }
    case llvm::Instruction::Add: {
      auto c0 = getConstant(I.getOperand(0));
      auto c1 = getConstant(I.getOperand(1));
      out = addVarUnconstrained(out, res);
      if (c0 && c1) {
        int64_t sum;
        if (__builtin_add_overflow(*c0, *c1, &sum))
          out = havocVar(out, res);
        else
          out = assignConstant(out, res, sum);
      } else if (c1 && *c1 == 0) {
        out = assignCopy(out, res, I.getOperand(0));
      } else if (c1) {
        out = assignAffine(out, res, I.getOperand(0), *c1);
      } else if (c0 && *c0 == 0) {
        out = assignCopy(out, res, I.getOperand(1));
      } else if (c0) {
        out = assignAffine(out, res, I.getOperand(1), *c0);
      } else {
        out = havocVar(out, res);
      }
      break;
    }
    case llvm::Instruction::Sub: {
      auto c0 = getConstant(I.getOperand(0));
      auto c1 = getConstant(I.getOperand(1));
      out = addVarUnconstrained(out, res);
      if (c0 && c1) {
        int64_t diff;
        if (__builtin_sub_overflow(*c0, *c1, &diff))
          out = havocVar(out, res);
        else
          out = assignConstant(out, res, diff);
      } else if (c1 && *c1 == 0) {
        out = assignCopy(out, res, I.getOperand(0));
      } else if (c1) {
        out = assignAffine(out, res, I.getOperand(0), -(*c1));
      } else if (c0) {
        out = havocVar(out, res);
      } else {
        out = havocVar(out, res);
      }
      break;
    }
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
    case llvm::Instruction::PtrToInt:
    case llvm::Instruction::IntToPtr:
    case llvm::Instruction::BitCast: {
      out = addVarUnconstrained(out, res);
      if (auto c = getConstant(I.getOperand(0)))
        out = assignConstant(out, res, *c);
      else
        out = assignCopy(out, res, I.getOperand(0));
      break;
    }
    case llvm::Instruction::ICmp:
      out = addVarUnconstrained(out, res);
      out = havocVar(out, res);
      break;
    default:
      out = addVarUnconstrained(out, res);
      out = havocVar(out, res);
      break;
    }
  }
  return out;
}

OctagonState OctagonDomain::applyBlockWiseHavoc(llvm::BasicBlock *bb,
                                                const OctagonState &in) const {
  return applyBlockWiseHavoc(bb, in, nullptr, nullptr);
}

OctagonState
OctagonDomain::applyBlockWiseHavoc(llvm::BasicBlock *bb, const OctagonState &in,
                                   const llvm::Instruction *segmentStart,
                                   const llvm::Instruction *stopBefore) const {
  if (in.isBottom())
    return in;
  OctagonState out = in;
  // memory_ already copied from in
  for (llvm::BasicBlock::const_iterator it = segmentBegin(bb, segmentStart),
                                        end = bb->end();
       it != end; ++it) {
    const llvm::Instruction &I = *it;
    if (&I == stopBefore || I.isTerminator())
      break;
    if (I.getType()->isVoidTy())
      continue;
    if (I.getType()->isIntegerTy() || I.getType()->isPointerTy())
      out = addVarUnconstrained(out, &I);
  }
  return out;
}

OctagonState OctagonDomain::post(const Transition &t,
                                 const OctagonState &in) const {
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
  OctagonState out =
      blockTransferPolicy_ && blockTransferPolicy_->useBlockWise(t.source)
          ? applyBlockWiseHavoc(t.source, in, t.segmentStart, t.stopBefore)
          : applyBlockTransfer(t.source, in, t.segmentStart, t.stopBefore);
  out = refineForTakenEdge(t, std::move(out));
  return applyIncomingPhis(t, std::move(out));
}

OctagonState OctagonDomain::postCall(const Transition &t,
                                     const OctagonState &callerState) const {
  return projectCallState(t, callerState);
}

OctagonState
OctagonDomain::postReturn(const Transition &t, const OctagonState &callerState,
                          const OctagonState &calleeSummary) const {
  return mergeReturnState(t, callerState, calleeSummary);
}

void OctagonState::print(llvm::raw_ostream &out) const {
  if (isBottom_) {
    out << "  (bottom)\n";
    return;
  }
  out << "  variables: " << varToIndex_.size();
  if (!memory_.empty())
    out << ", memory regions: " << memory_.size();
  out << "\n";
}
