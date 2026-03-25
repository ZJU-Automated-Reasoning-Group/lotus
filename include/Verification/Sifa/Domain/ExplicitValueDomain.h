//===-- Verification/Sifa/Domain/ExplicitValueDomain.h --------------------===//
//
// Domain of explicit variable valuations (Ultimate
// ExplicitValueDomain-aligned).
//
// Ultimate's ExplicitValueDomain(SymbolicTools, maxDisjuncts) represents
// states as DNF of variable = constant; join limits disjuncts. In lotus we
// use a single map Value* -> optional constant (constant propagation style);
// join: same constant => keep, different => top (drop binding).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_EXPLICITVALUEDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_EXPLICITVALUEDOMAIN_H

#include "llvm/ADT/Optional.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"

#include "Verification/Sifa/BlockTransferPolicy.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/RegionMemory.h"

#include <unordered_map>

namespace lotus {
class AliasAnalysisWrapper;
namespace sifa {

/// Single explicit value: constant or top (no info). Ultimate
/// INonrelationalValue for constants.
struct ExplicitValue {
  llvm::Optional<int64_t> value;

  static ExplicitValue top() { return {llvm::None}; }
  static ExplicitValue constant(int64_t c) { return {c}; }
  bool isTop() const { return !value.hasValue(); }
  bool isBottom() const {
    return false;
  } // no explicit bottom in constant domain

  ExplicitValue join(const ExplicitValue &rhs) const {
    if (isTop() || rhs.isTop())
      return top();
    if (value.getValue() == rhs.value.getValue())
      return *this;
    return top();
  }
  ExplicitValue widen(const ExplicitValue &rhs) const { return join(rhs); }

  bool operator==(const ExplicitValue &rhs) const { return value == rhs.value; }
};

/// State: map Value* -> optional constant + optional region memory (when AA is
/// set).
struct ExplicitValueState {
  bool isBottom_ = false;
  std::unordered_map<const llvm::Value *, ExplicitValue> map_;
  std::unordered_map<const llvm::Value *, ExplicitValue> memory_;

  ExplicitValueState() = default;
  explicit ExplicitValueState(bool isBottom) : isBottom_(isBottom) {}

  bool isBottom() const { return isBottom_; }
  llvm::Optional<ExplicitValue> get(const llvm::Value *v) const {
    auto it = map_.find(v);
    if (it == map_.end())
      return llvm::Optional<ExplicitValue>(ExplicitValue::top());
    return it->second;
  }
  void set(const llvm::Value *v, ExplicitValue val) {
    if (val.isTop())
      map_.erase(v);
    else
      map_[v] = std::move(val);
  }
  llvm::Optional<ExplicitValue> getMemory(const llvm::Value *region) const {
    auto it = memory_.find(region);
    if (it == memory_.end())
      return llvm::Optional<ExplicitValue>(ExplicitValue::top());
    return it->second;
  }
  void setMemory(const llvm::Value *region, ExplicitValue val) {
    if (val.isTop())
      memory_.erase(region);
    else
      memory_[region] = std::move(val);
  }

  bool operator==(const ExplicitValueState &rhs) const {
    return isBottom_ == rhs.isBottom_ && map_ == rhs.map_ &&
           memory_ == rhs.memory_;
  }
};

/// Explicit value domain (constant propagation style). post(Edge): applies
/// block transfer (constant propagation). When BlockTransferPolicy marks a
/// block as block-wise, post(Edge) uses applyBlockWiseHavoc (no new constants).
class ExplicitValueDomain final
    : public AbstractDomain<Transition, ExplicitValueState> {
public:
  using State = ExplicitValueState;

  ExplicitValueDomain() = default;
  explicit ExplicitValueDomain(const BlockTransferPolicy *policy)
      : blockTransferPolicy_(policy) {}
  ExplicitValueDomain(const BlockTransferPolicy *policy,
                      lotus::AliasAnalysisWrapper *aliasAnalysis)
      : blockTransferPolicy_(policy), aliasAnalysis_(aliasAnalysis) {}

  void setBlockTransferPolicy(const BlockTransferPolicy *policy) {
    blockTransferPolicy_ = policy;
  }
  const BlockTransferPolicy *getBlockTransferPolicy() const {
    return blockTransferPolicy_;
  }
  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) {
    aliasAnalysis_ = aa;
  }
  lotus::AliasAnalysisWrapper *getAliasAnalysis() const {
    return aliasAnalysis_;
  }

  State top() const override { return State(false); }
  State bottom() const override { return State(true); }
  bool isBottom(const State &s) const override { return s.isBottom(); }

  bool leq(const State &a, const State &b) const override {
    if (a.isBottom())
      return true;
    if (b.isBottom())
      return false;
    for (const auto &kv : a.map_) {
      auto ob = b.get(kv.first);
      if (!ob.hasValue() || ob.getValue().isTop())
        continue;
      if (kv.second.isTop())
        return false;
      if (kv.second.value != ob.getValue().value)
        return false;
    }
    for (const auto &kv : a.memory_) {
      auto ob = b.getMemory(kv.first);
      if (!ob.hasValue() || ob.getValue().isTop())
        continue;
      if (kv.second.isTop())
        return false;
      if (kv.second.value != ob.getValue().value)
        return false;
    }
    return true;
  }
  State join(const State &a, const State &b) const override {
    if (a.isBottom())
      return b;
    if (b.isBottom())
      return a;
    State r;
    for (const auto &kv : a.map_) {
      auto ob = b.get(kv.first);
      ExplicitValue j = kv.second;
      if (ob.hasValue())
        j = j.join(ob.getValue());
      if (!j.isTop())
        r.set(kv.first, j);
    }
    for (const auto &kv : b.map_) {
      if (r.map_.find(kv.first) != r.map_.end())
        continue;
      auto oa = a.get(kv.first);
      ExplicitValue j = kv.second;
      if (oa.hasValue())
        j = j.join(oa.getValue());
      if (!j.isTop())
        r.set(kv.first, j);
    }
    for (const auto &kv : a.memory_) {
      auto ob = b.getMemory(kv.first);
      ExplicitValue j = kv.second;
      if (ob.hasValue())
        j = j.join(ob.getValue());
      if (!j.isTop())
        r.setMemory(kv.first, j);
    }
    for (const auto &kv : b.memory_) {
      if (r.memory_.find(kv.first) != r.memory_.end())
        continue;
      auto oa = a.getMemory(kv.first);
      ExplicitValue j = kv.second;
      if (oa.hasValue())
        j = j.join(oa.getValue());
      if (!j.isTop())
        r.setMemory(kv.first, j);
    }
    return r;
  }
  State widen(const State &prev, const State &next) const override {
    return join(prev, next);
  }

  /// Block-wise fast path: do not track constants for this block (sound, less
  /// precise).
  State applyBlockWiseHavoc(llvm::BasicBlock *bb, const State &in) const {
    (void)bb;
    return in; // Leave state unchanged; values defined in bb are unknown (top).
  }
  State post(const Transition &t, const State &in) const override {
    if (in.isBottom())
      return in;
    if (t.kind != TransitionKind::Edge || !t.source)
      return in;
    auto begin =
        t.segmentStart ? t.segmentStart->getIterator() : t.source->begin();
    while (begin != t.source->end() && llvm::isa<llvm::PHINode>(*begin))
      ++begin;
    if (blockTransferPolicy_ && blockTransferPolicy_->useBlockWise(t.source)) {
      State out = in;
      for (auto it = begin; it != t.source->end(); ++it) {
        const llvm::Instruction &I = *it;
        if (&I == t.stopBefore || I.isTerminator())
          break;
        if (I.getType()->isVoidTy())
          continue;
        if (I.getType()->isIntegerTy() || I.getType()->isPointerTy())
          out.set(&I, ExplicitValue::top());
      }
      out = refineForTakenEdge(t, std::move(out));
      return applyIncomingPhis(t, std::move(out));
    }
    State out = in;
    lotus::AliasAnalysisWrapper *AA = getAliasAnalysis();
    std::vector<const llvm::Value *> regions;
    std::vector<const llvm::Value *> resolved;
    if (AA)
      regions = getRegionsForFunction(*t.source->getParent());
    for (auto it = begin; it != t.source->end(); ++it) {
      const llvm::Instruction &I = *it;
      if (&I == t.stopBefore || I.isTerminator())
        break;
      if (I.getType()->isVoidTy()) {
        if (AA && llvm::isa<llvm::StoreInst>(&I)) {
          auto *SI = llvm::cast<llvm::StoreInst>(&I);
          const llvm::Value *ptr = SI->getPointerOperand();
          ExplicitValue val = getConst(out, SI->getValueOperand());
          resolvePointerToRegions(AA, ptr, regions, resolved);
          for (const llvm::Value *r : resolved) {
            auto cur = out.getMemory(r);
            ExplicitValue j = cur ? cur->join(val) : val;
            if (!j.isTop())
              out.setMemory(r, j);
          }
        }
        continue;
      }
      if (I.getType()->isIntegerTy() || I.getType()->isPointerTy()) {
        ExplicitValue res;
        if (AA && llvm::isa<llvm::AllocaInst>(&I)) {
          out.setMemory(&I, ExplicitValue::top());
          res = ExplicitValue::top();
        } else if (AA && llvm::isa<llvm::LoadInst>(&I) &&
                   I.getType()->isIntegerTy()) {
          const llvm::Value *ptr =
              llvm::cast<llvm::LoadInst>(&I)->getPointerOperand();
          resolvePointerToRegions(AA, ptr, regions, resolved);
          res = ExplicitValue::top();
          for (const llvm::Value *r : resolved) {
            auto cur = out.getMemory(r);
            ExplicitValue ir = cur ? *cur : ExplicitValue::top();
            res = res.join(ir);
          }
        } else if (AA && llvm::isa<llvm::GetElementPtrInst>(&I)) {
          res = ExplicitValue::top();
        } else {
          res = transferInstruction(I, out);
        }
        if (!res.isTop())
          out.set(&I, res);
      }
    }
    out = refineForTakenEdge(t, std::move(out));
    return applyIncomingPhis(t, std::move(out));
  }
  State postCall(const Transition &t, const State &callerState) const override {
    if (callerState.isBottom() || !t.call || !t.callee)
      return callerState;
    State projected(false);
    const llvm::Function *caller = t.source ? t.source->getParent() : nullptr;
    for (const auto &kv : callerState.map_) {
      if (!isFunctionLocalValue(caller, kv.first))
        projected.set(kv.first, kv.second);
    }
    for (const auto &kv : callerState.memory_)
      projected.setMemory(kv.first, kv.second);
    unsigned actualIndex = 0;
    for (const llvm::Argument &formal : t.callee->args()) {
      if (actualIndex >= t.call->arg_size())
        break;
      projected.set(&formal,
                    getConst(callerState, t.call->getArgOperand(actualIndex)));
      ++actualIndex;
    }
    return projected;
  }
  State postCall(const State &callerState) const override {
    return callerState;
  }
  State postReturn(const Transition &t, const State &callerState,
                   const State &calleeSummary) const override {
    if (callerState.isBottom() || calleeSummary.isBottom())
      return bottom();
    State out = callerState;
    const llvm::Function *callee = t.callee;
    for (const auto &kv : calleeSummary.map_) {
      if (!isFunctionLocalValue(callee, kv.first))
        out.set(kv.first, kv.second);
    }
    for (const auto &kv : calleeSummary.memory_)
      out.setMemory(kv.first, kv.second);
    if (t.call && !t.call->getType()->isVoidTy())
      out.set(t.call, returnedValue(*callee, calleeSummary));
    return out;
  }
  State postReturn(const State &callerState,
                   const State &calleeSummary) const override {
    return join(callerState, calleeSummary);
  }

private:
  static bool isFunctionLocalValue(const llvm::Function *F,
                                   const llvm::Value *V) {
    if (!F || !V)
      return false;
    if (const auto *I = llvm::dyn_cast<llvm::Instruction>(V))
      return I->getFunction() == F;
    if (const auto *A = llvm::dyn_cast<llvm::Argument>(V))
      return A->getParent() == F;
    return false;
  }

  static const llvm::Value *
  incomingValueForPredecessor(const llvm::PHINode &phi,
                              const llvm::BasicBlock *pred) {
    if (!pred)
      return nullptr;
    const int index =
        phi.getBasicBlockIndex(const_cast<llvm::BasicBlock *>(pred));
    if (index < 0)
      return nullptr;
    return phi.getIncomingValue(static_cast<unsigned>(index));
  }

  static State applyIncomingPhis(const Transition &t, State out) {
    if (out.isBottom())
      return out;
    if (!t.source || !t.target || !t.landsAtBlockEntry())
      return out;
    for (const llvm::Instruction &I : *t.target) {
      const auto *phi = llvm::dyn_cast<llvm::PHINode>(&I);
      if (!phi)
        break;
      out.set(phi, getConst(out, incomingValueForPredecessor(*phi, t.source)));
    }
    return out;
  }

  static ExplicitValue getConst(const State &s, const llvm::Value *V) {
    if (s.isBottom())
      return ExplicitValue::top();
    if (!V)
      return ExplicitValue::top();
    if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(V)) {
      if (C->getBitWidth() > 64)
        return ExplicitValue::top();
      return ExplicitValue::constant(C->getSExtValue());
    }
    auto it = s.map_.find(V);
    if (it != s.map_.end() && !it->second.isTop())
      return it->second;
    return ExplicitValue::top();
  }
  static State bottomState() { return State(true); }

  static bool evaluatePredicate(llvm::CmpInst::Predicate predicate, int64_t lhs,
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

  static State setValue(State out, const llvm::Value *value,
                        ExplicitValue explicitValue) {
    if (!value || llvm::isa<llvm::Constant>(value))
      return out;
    if (out.isBottom())
      return out;
    out.set(value, explicitValue);
    return out;
  }

  static State refineForPredicate(State out, const llvm::ICmpInst &cmp,
                                  bool truthy) {
    llvm::CmpInst::Predicate predicate = cmp.getPredicate();
    if (!truthy)
      predicate = cmp.getInversePredicate();

    const llvm::Value *lhsValue = cmp.getOperand(0);
    const llvm::Value *rhsValue = cmp.getOperand(1);
    const ExplicitValue lhs = getConst(out, lhsValue);
    const ExplicitValue rhs = getConst(out, rhsValue);
    if (!lhs.isTop() && !rhs.isTop()) {
      return evaluatePredicate(predicate, *lhs.value, *rhs.value)
                 ? out
                 : bottomState();
    }

    switch (predicate) {
    case llvm::CmpInst::ICMP_EQ:
      if (!lhs.isTop())
        out = setValue(std::move(out), rhsValue, lhs);
      else if (!rhs.isTop())
        out = setValue(std::move(out), lhsValue, rhs);
      break;
    case llvm::CmpInst::ICMP_NE:
      if (!lhs.isTop() && !rhs.isTop() && lhs.value == rhs.value)
        return bottomState();
      break;
    default:
      break;
    }
    return out;
  }

  static State refineForCondition(State out, const llvm::Value *condition,
                                  bool truthy) {
    if (out.isBottom() || !condition)
      return out;
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(condition)) {
      return (!constant->isZero() == truthy) ? out : bottomState();
    }
    if (const auto *cmp = llvm::dyn_cast<llvm::ICmpInst>(condition))
      return refineForPredicate(std::move(out), *cmp, truthy);

    const ExplicitValue current = getConst(out, condition);
    if (!current.isTop())
      return (*current.value != 0) == truthy ? out : bottomState();
    return setValue(std::move(out), condition,
                    ExplicitValue::constant(truthy ? 1 : 0));
  }

  static State refineForSwitch(const Transition &t, State out,
                               const llvm::SwitchInst &switchInst) {
    const ExplicitValue current = getConst(out, switchInst.getCondition());
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
      if (!current.isTop() && *current.value != *matchedCase)
        return bottomState();
      return setValue(std::move(out), switchInst.getCondition(),
                      ExplicitValue::constant(*matchedCase));
    }

    if (switchInst.getDefaultDest() != target)
      return out;
    if (!current.isTop()) {
      for (const auto &caseHandle : switchInst.cases()) {
        if (caseHandle.getCaseValue()->getBitWidth() > 64)
          continue;
        if (*current.value == caseHandle.getCaseValue()->getSExtValue())
          return bottomState();
      }
    }
    return out;
  }

  static State refineForTakenEdge(const Transition &t, State out) {
    if (out.isBottom() || !t.source)
      return out;
    const llvm::Instruction *terminator = t.source->getTerminator();
    if (const auto *branch = llvm::dyn_cast<llvm::BranchInst>(terminator)) {
      if (!branch->isConditional())
        return out;
      if (branch->getSuccessor(0) == t.target)
        return refineForCondition(std::move(out), branch->getCondition(), true);
      if (branch->getSuccessor(1) == t.target)
        return refineForCondition(std::move(out), branch->getCondition(),
                                  false);
      return bottomState();
    }
    if (const auto *switchInst = llvm::dyn_cast<llvm::SwitchInst>(terminator))
      return refineForSwitch(t, std::move(out), *switchInst);
    return out;
  }

  static ExplicitValue returnedValue(const llvm::Function &callee,
                                     const State &calleeSummary) {
    ExplicitValue result = ExplicitValue::top();
    bool sawReturn = false;
    for (const llvm::BasicBlock &bb : callee) {
      const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
      if (!ret || ret->getNumOperands() == 0)
        continue;
      const ExplicitValue value =
          getConst(calleeSummary, ret->getReturnValue());
      result = sawReturn ? result.join(value) : value;
      sawReturn = true;
    }
    return sawReturn ? result : ExplicitValue::top();
  }

  static ExplicitValue transferInstruction(const llvm::Instruction &I,
                                           const State &state) {
    if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(&I)) {
      if (C->getBitWidth() > 64)
        return ExplicitValue::top();
      return ExplicitValue::constant(C->getSExtValue());
    }
    switch (I.getOpcode()) {
    case llvm::Instruction::Add: {
      auto L = getConst(state, I.getOperand(0));
      auto R = getConst(state, I.getOperand(1));
      if (!L.isTop() && !R.isTop())
        return ExplicitValue::constant(*L.value + *R.value);
      return ExplicitValue::top();
    }
    case llvm::Instruction::Sub: {
      auto L = getConst(state, I.getOperand(0));
      auto R = getConst(state, I.getOperand(1));
      if (!L.isTop() && !R.isTop())
        return ExplicitValue::constant(*L.value - *R.value);
      return ExplicitValue::top();
    }
    case llvm::Instruction::Mul: {
      auto L = getConst(state, I.getOperand(0));
      auto R = getConst(state, I.getOperand(1));
      if (!L.isTop() && !R.isTop())
        return ExplicitValue::constant(*L.value * *R.value);
      return ExplicitValue::top();
    }
    case llvm::Instruction::SDiv:
    case llvm::Instruction::UDiv: {
      auto L = getConst(state, I.getOperand(0));
      auto R = getConst(state, I.getOperand(1));
      if (!L.isTop() && !R.isTop() && *R.value != 0)
        return ExplicitValue::constant(*L.value / *R.value);
      return ExplicitValue::top();
    }
    case llvm::Instruction::SRem:
    case llvm::Instruction::URem: {
      auto L = getConst(state, I.getOperand(0));
      auto R = getConst(state, I.getOperand(1));
      if (!L.isTop() && !R.isTop() && *R.value != 0)
        return ExplicitValue::constant(*L.value % *R.value);
      return ExplicitValue::top();
    }
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
    case llvm::Instruction::PtrToInt:
    case llvm::Instruction::IntToPtr:
    case llvm::Instruction::BitCast:
      return getConst(state, I.getOperand(0));
    case llvm::Instruction::PHI: {
      auto *Phi = llvm::cast<llvm::PHINode>(&I);
      ExplicitValue acc = getConst(state, Phi->getIncomingValue(0));
      for (unsigned i = 1, e = Phi->getNumIncomingValues(); i < e; ++i)
        acc = acc.join(getConst(state, Phi->getIncomingValue(i)));
      return acc;
    }
    case llvm::Instruction::Select: {
      auto T = getConst(state, I.getOperand(1));
      auto F = getConst(state, I.getOperand(2));
      return T.join(F);
    }
    default:
      return ExplicitValue::top();
    }
  }

private:
  const BlockTransferPolicy *blockTransferPolicy_ = nullptr;
  lotus::AliasAnalysisWrapper *aliasAnalysis_ = nullptr;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_EXPLICITVALUEDOMAIN_H
