/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Inter/Interval.h"

#include "Dataflow/NPA/LLVM/ForwardInterEngine.h"

#include <algorithm>
#include <array>
#include <limits>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace npa {

namespace {

bool apIntLess(const llvm::APInt &lhs, const llvm::APInt &rhs) {
  if (lhs.getBitWidth() != rhs.getBitWidth())
    return lhs.getBitWidth() < rhs.getBitWidth();
  return lhs.ult(rhs);
}

bool apIntEqual(const llvm::APInt &lhs, const llvm::APInt &rhs) {
  return lhs.getBitWidth() == rhs.getBitWidth() && lhs.eq(rhs);
}

using D = IntervalSummary;
using Exp = Exp0<D>;
using E = E0<D>;

bool isTrackedScalar(const llvm::Value *V) {
  auto *Ty = V ? V->getType() : nullptr;
  return Ty && Ty->isIntegerTy() && Ty->getIntegerBitWidth() <= 64;
}

unsigned getIntegerBitWidth(const llvm::Value *V) {
  auto *Ty = V ? V->getType() : nullptr;
  return Ty && Ty->isIntegerTy() ? Ty->getIntegerBitWidth() : 0;
}

bool getConstantAPInt(const llvm::Value *V, llvm::APInt &out) {
  auto *CI = llvm::dyn_cast_or_null<llvm::ConstantInt>(V);
  if (!CI || CI->getBitWidth() > 64)
    return false;
  out = CI->getValue();
  return true;
}

unsigned intervalBitWidth(const Interval &I) {
  if (I.hasLower)
    return I.lower.getBitWidth();
  if (I.hasUpper)
    return I.upper.getBitWidth();
  return 1;
}

Interval topInterval(unsigned bitWidth = 1,
                     IntervalOrdering ordering = IntervalOrdering::Signed) {
  return Interval::top(bitWidth, ordering);
}

Interval pointInterval(const llvm::APInt &value,
                       IntervalOrdering ordering = IntervalOrdering::Signed) {
  return Interval::point(value, ordering);
}

bool usesUnsignedOrder(IntervalOrdering ordering) {
  return ordering == IntervalOrdering::Unsigned;
}

bool orderedLess(const llvm::APInt &lhs, const llvm::APInt &rhs,
                 IntervalOrdering ordering) {
  return usesUnsignedOrder(ordering) ? lhs.ult(rhs) : lhs.slt(rhs);
}

bool orderedLessEqual(const llvm::APInt &lhs, const llvm::APInt &rhs,
                      IntervalOrdering ordering) {
  return usesUnsignedOrder(ordering) ? lhs.ule(rhs) : lhs.sle(rhs);
}

llvm::APInt orderedMin(const llvm::APInt &lhs, const llvm::APInt &rhs,
                       IntervalOrdering ordering) {
  return orderedLess(lhs, rhs, ordering) ? lhs : rhs;
}

llvm::APInt orderedMax(const llvm::APInt &lhs, const llvm::APInt &rhs,
                       IntervalOrdering ordering) {
  return orderedLess(rhs, lhs, ordering) ? lhs : rhs;
}

Interval joinIntervals(const Interval &lhs, const Interval &rhs) {
  if (lhs.bottom)
    return rhs;
  if (rhs.bottom)
    return lhs;
  if (lhs.isExact() && rhs.isExact() && apIntEqual(lhs.lower, rhs.lower))
    return lhs;
  if (lhs.ordering != rhs.ordering)
    return topInterval(std::max(intervalBitWidth(lhs), intervalBitWidth(rhs)));
  if (!lhs.hasLower || !lhs.hasUpper || !rhs.hasLower || !rhs.hasUpper)
    return topInterval(std::max(intervalBitWidth(lhs), intervalBitWidth(rhs)),
                       lhs.ordering);
  Interval out = topInterval(intervalBitWidth(lhs), lhs.ordering);
  out.hasLower = true;
  out.hasUpper = true;
  out.lower = orderedMin(lhs.lower, rhs.lower, lhs.ordering);
  out.upper = orderedMax(lhs.upper, rhs.upper, lhs.ordering);
  return out;
}

bool containsZero(const Interval &I) {
  if (I.bottom)
    return false;
  if (!I.hasLower || !I.hasUpper)
    return true;
  llvm::APInt zero(intervalBitWidth(I), 0);
  if (usesUnsignedOrder(I.ordering))
    return I.lower.isZero();
  return orderedLessEqual(I.lower, zero, I.ordering) &&
         orderedLessEqual(zero, I.upper, I.ordering);
}

bool isDefinitelyZero(const Interval &I) {
  return I.isExact() && I.lower.isZero();
}

bool isDefinitelyNonZero(const Interval &I) {
  if (!I.hasLower || !I.hasUpper)
    return false;
  llvm::APInt zero(intervalBitWidth(I), 0);
  if (usesUnsignedOrder(I.ordering))
    return orderedLess(zero, I.lower, I.ordering);
  return orderedLess(I.upper, zero, I.ordering) ||
         orderedLess(zero, I.lower, I.ordering);
}

Interval applyCastToConstant(unsigned opcode, unsigned destWidth,
                             const llvm::APInt &input) {
  llvm::APInt casted = input;
  switch (opcode) {
  case llvm::Instruction::SExt:
    casted = input.sext(destWidth);
    return pointInterval(casted, IntervalOrdering::Signed);
  case llvm::Instruction::ZExt:
    casted = input.zext(destWidth);
    return pointInterval(casted, IntervalOrdering::Unsigned);
  case llvm::Instruction::Trunc:
    casted = input.trunc(destWidth);
    return pointInterval(casted, IntervalOrdering::Signed);
  default:
    return topInterval(destWidth);
  }
}

Interval widenInterval(const Interval &oldI, const Interval &newI) {
  if (oldI.bottom)
    return newI;
  if (newI.bottom)
    return oldI;
  if (!oldI.hasLower || !oldI.hasUpper)
    return oldI;
  if (!newI.hasLower || !newI.hasUpper || oldI.ordering != newI.ordering)
    return topInterval(intervalBitWidth(oldI), oldI.ordering);

  Interval out = topInterval(intervalBitWidth(oldI), oldI.ordering);
  if (orderedLess(newI.lower, oldI.lower, oldI.ordering)) {
    out.hasLower = false;
  } else {
    out.hasLower = true;
    out.lower = newI.lower;
  }
  if (orderedLess(oldI.upper, newI.upper, oldI.ordering)) {
    out.hasUpper = false;
  } else {
    out.hasUpper = true;
    out.upper = newI.upper;
  }
  return out;
}

Interval refineDefaultSwitch(Interval current,
                             const std::vector<llvm::APInt> &excluded) {
  if (current.bottom || !current.hasLower || !current.hasUpper)
    return current;
  if (current.isExact()) {
    for (const auto &value : excluded)
      if (apIntEqual(current.lower, value))
        return Interval{true,
                        false,
                        false,
                        current.ordering,
                        llvm::APInt(intervalBitWidth(current), 0),
                        llvm::APInt(intervalBitWidth(current), 0)};
    return current;
  }

  llvm::APInt lower = current.lower;
  llvm::APInt upper = current.upper;
  bool changed = false;

  auto inRange = [&](const llvm::APInt &value) {
    return !orderedLess(value, lower, current.ordering) &&
           !orderedLess(upper, value, current.ordering);
  };

  std::vector<llvm::APInt> relevant;
  for (const auto &value : excluded)
    if (value.getBitWidth() == lower.getBitWidth() && inRange(value))
      relevant.push_back(value);

  auto orderCmp = [&](const llvm::APInt &lhs, const llvm::APInt &rhs) {
    return orderedLess(lhs, rhs, current.ordering);
  };
  std::sort(relevant.begin(), relevant.end(), orderCmp);
  relevant.erase(std::unique(relevant.begin(), relevant.end(), apIntEqual),
                 relevant.end());

  std::size_t prefix = 0;
  while (prefix < relevant.size() && apIntEqual(relevant[prefix], lower)) {
    lower = lower + llvm::APInt(lower.getBitWidth(), 1);
    changed = true;
    ++prefix;
  }

  std::size_t suffix = relevant.size();
  while (suffix > prefix && apIntEqual(relevant[suffix - 1], upper)) {
    upper = upper - llvm::APInt(upper.getBitWidth(), 1);
    changed = true;
    --suffix;
  }

  if (!changed)
    return current;
  if (orderedLess(upper, lower, current.ordering)) {
    Interval out = topInterval(intervalBitWidth(current), current.ordering);
    out.bottom = true;
    out.hasLower = false;
    out.hasUpper = false;
    return out;
  }

  current.lower = lower;
  current.upper = upper;
  return current;
}

std::vector<llvm::APInt>
constantCases(const std::vector<const llvm::Value *> &inputs) {
  std::vector<llvm::APInt> out;
  for (const llvm::Value *input : inputs) {
    llvm::APInt value(1, 0);
    if (getConstantAPInt(input, value))
      out.push_back(value);
  }
  return out;
}

} // namespace

namespace {

class IntervalAnalysis {
public:
  using FactType = IntervalState;
  using Engine = InterEngine<D, IntervalAnalysis>;

  FactType getEntryValue() const { return {true, {}}; }

  D::value_type getEdgeTransfer(const llvm::Instruction &term,
                                const llvm::BasicBlock &succ) const {
    if (auto *Branch = llvm::dyn_cast<llvm::BranchInst>(&term)) {
      if (!Branch->isConditional())
        return D::one();
      llvm::APInt condValue(1, 0);
      if (getConstantAPInt(Branch->getCondition(), condValue))
        return Branch->getSuccessor(condValue.isOne() ? 0 : 1) == &succ
                   ? D::one()
                   : D::zero();
      if (!isTrackedScalar(Branch->getCondition()))
        return D::one();
      return buildAssign(
          Branch->getCondition(),
          llvm::APInt(1, Branch->getSuccessor(0) == &succ ? 1 : 0),
          IntervalOrdering::Signed);
    }
    if (auto *Switch = llvm::dyn_cast<llvm::SwitchInst>(&term)) {
      llvm::APInt condValue(1, 0);
      if (getConstantAPInt(Switch->getCondition(), condValue)) {
        for (const auto &Case : Switch->cases())
          if (condValue.eq(Case.getCaseValue()->getValue()))
            return Case.getCaseSuccessor() == &succ ? D::one() : D::zero();
        return Switch->getDefaultDest() == &succ ? D::one() : D::zero();
      }
      if (!isTrackedScalar(Switch->getCondition()))
        return D::one();
      for (const auto &Case : Switch->cases()) {
        if (Case.getCaseSuccessor() == &succ)
          return buildAssign(Switch->getCondition(),
                             Case.getCaseValue()->getValue(),
                             IntervalOrdering::Signed);
      }
      if (Switch->getDefaultDest() == &succ && !Switch->cases().empty()) {
        IntervalOp op;
        op.kind = IntervalOp::Kind::AssumeNotCases;
        op.cond = Switch->getCondition();
        op.bitWidth = getIntegerBitWidth(Switch->getCondition());
        for (const auto &Case : Switch->cases())
          op.inputs.push_back(Case.getCaseValue());
        return D::singleton(op);
      }
    }
    return D::one();
  }

  E buildBlockEntryExpr(llvm::BasicBlock &BB, E inExpr) {
    auto *FirstPhi = llvm::dyn_cast<llvm::PHINode>(BB.begin());
    if (!FirstPhi)
      return inExpr;

    E result = nullptr;
    for (auto *Pred : predecessors(&BB)) {
      D::value_type transfer = D::one();
      if (auto *PredTerm = Pred->getTerminator())
        transfer = D::extend(getEdgeTransfer(*PredTerm, BB), transfer);
      for (auto &Inst : BB) {
        auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Inst);
        if (!Phi)
          break;
        transfer = D::extend(
            buildAssign(Phi, Phi->getIncomingValueForBlock(Pred)), transfer);
      }
      E branch = Exp::seq(transfer, Exp::hole(Engine::getBlockSymbol(Pred)));
      result = result ? Exp::ndet(result, branch) : branch;
    }
    return result ? result : inExpr;
  }

  E getTransfer(llvm::Instruction &I, E currentPath) {
    if (llvm::isa<llvm::CallBase>(&I) || llvm::isa<llvm::PHINode>(&I) ||
        I.getType()->isVoidTy())
      return currentPath;

    IntervalOp op;
    if (!buildTransfer(I, op))
      return currentPath;
    return Exp::seq(D::singleton(op), currentPath);
  }

  D::value_type getCallEntryTransfer(const llvm::CallBase &Call,
                                     const llvm::Function &Callee) {
    D::value_type transfer = D::one();
    const auto *ParamIt = Callee.arg_begin();
    for (unsigned i = 0; i < Call.arg_size() && ParamIt != Callee.arg_end();
         ++i, ++ParamIt) {
      if (!isTrackedScalar(&*ParamIt))
        continue;
      transfer =
          D::extend(buildAssign(&*ParamIt, Call.getArgOperand(i)), transfer);
    }
    return transfer;
  }

  D::value_type getCallReturnTransfer(const llvm::CallBase &Call,
                                      const llvm::Function &Callee) {
    if (Call.getType()->isVoidTy() || !isTrackedScalar(&Call))
      return D::one();

    IntervalOp op;
    op.dest = &Call;
    op.kind = IntervalOp::Kind::Phi;
    op.bitWidth = getIntegerBitWidth(&Call);
    for (const auto &BB : Callee) {
      auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator());
      if (!Ret)
        continue;
      if (const llvm::Value *RetVal = Ret->getReturnValue())
        op.inputs.push_back(RetVal);
    }
    if (op.inputs.empty())
      op.kind = IntervalOp::Kind::Forget;
    return D::singleton(op);
  }

  D::value_type getCallToReturnTransfer(const llvm::CallBase &Call) {
    if (Call.getType()->isVoidTy() || !isTrackedScalar(&Call))
      return D::one();
    IntervalOp op;
    op.kind = IntervalOp::Kind::Forget;
    op.dest = &Call;
    return D::singleton(op);
  }

  FactType applySummary(const D::value_type &summary, const FactType &fact,
                        bool *used_summary_overflow) const {
    if (!fact.reachable)
      return {false, {}};
    if (summary.overflow && used_summary_overflow)
      *used_summary_overflow = true;

    bool first = true;
    FactType joined;
    for (const auto &transformer : summary.transformers) {
      FactType current = fact;
      current.reachable = true;
      for (const auto &op : transformer)
        applyOp(current, op);
      if (!current.reachable)
        continue;
      if (first) {
        joined = std::move(current);
        first = false;
      } else {
        joined = joinFacts(joined, current);
      }
    }

    if (!summary.overflow)
      return first ? FactType{false, {}} : joined;

    FactType overflow = overflowFact(summary, fact);
    if (first)
      return overflow;
    return joinFacts(joined, overflow);
  }

  FactType applySummary(const D::value_type &summary,
                        const FactType &fact) const {
    return applySummary(summary, fact, nullptr);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    if (!lhs.reachable)
      return rhs;
    if (!rhs.reachable)
      return lhs;

    FactType out;
    out.reachable = true;
    for (const auto &entry : lhs.values) {
      auto It = rhs.values.find(entry.first);
      if (It == rhs.values.end())
        continue;
      Interval joined = joinIntervals(entry.second, It->second);
      if (!(joined == topInterval(intervalBitWidth(joined), joined.ordering)))
        out.values[entry.first] = joined;
    }
    return out;
  }

  FactType widenFacts(const FactType &oldFact, const FactType &newFact,
                      size_t updates, bool *used_fact_widening) const {
    if (updates < 2 || !oldFact.reachable)
      return newFact;
    if (!newFact.reachable)
      return oldFact;

    if (used_fact_widening)
      *used_fact_widening = true;
    FactType widened;
    widened.reachable = true;
    for (const auto &entry : newFact.values) {
      const llvm::Value *V = entry.first;
      const Interval &next = entry.second;
      auto OldIt = oldFact.values.find(V);
      if (OldIt == oldFact.values.end()) {
        widened.values[V] = next;
        continue;
      }
      widened.values[V] = widenInterval(OldIt->second, next);
    }
    return widened;
  }

  FactType widenFacts(const FactType &oldFact, const FactType &newFact,
                      size_t updates) const {
    return widenFacts(oldFact, newFact, updates, nullptr);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return lhs == rhs;
  }

  bool summaryIsApproximate(const D::value_type &summary) const {
    return summary.overflow;
  }

private:
  FactType overflowFact(const D::value_type &summary,
                        const FactType &fact) const {
    FactType out;
    out.reachable = fact.reachable;
    for (const auto &entry : fact.values)
      if (!summary.may_write.count(entry.first))
        out.values.insert(entry);
    return out;
  }

  D::value_type
  buildAssign(const llvm::Value *dest, const llvm::APInt &value,
              IntervalOrdering ordering = IntervalOrdering::Signed) const {
    IntervalOp op;
    op.dest = dest;
    op.kind = IntervalOp::Kind::AssignConst;
    op.bitWidth = getIntegerBitWidth(dest);
    op.ordering = ordering;
    op.constant = value;
    return D::singleton(op);
  }

  D::value_type buildAssign(const llvm::Value *dest,
                            const llvm::Value *src) const {
    IntervalOp op;
    op.dest = dest;
    op.bitWidth = getIntegerBitWidth(dest);
    llvm::APInt value(1, 0);
    if (getConstantAPInt(src, value)) {
      op.kind = IntervalOp::Kind::AssignConst;
      op.constant = value;
    } else if (isTrackedScalar(src)) {
      op.kind = IntervalOp::Kind::Copy;
      op.lhs = src;
    } else {
      op.kind = IntervalOp::Kind::Forget;
    }
    return D::singleton(op);
  }

  bool buildTransfer(llvm::Instruction &I, IntervalOp &op) const {
    op.dest = &I;
    if (!isTrackedScalar(&I)) {
      op.kind = IntervalOp::Kind::Forget;
      return true;
    }

    if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
      op.kind = IntervalOp::Kind::Phi;
      op.bitWidth = getIntegerBitWidth(&I);
      for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i)
        op.inputs.push_back(Phi->getIncomingValue(i));
      return true;
    }

    if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(&I)) {
      op.kind = IntervalOp::Kind::Select;
      op.bitWidth = getIntegerBitWidth(&I);
      op.cond = Select->getCondition();
      op.lhs = Select->getTrueValue();
      op.rhs = Select->getFalseValue();
      return true;
    }

    if (auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(&I)) {
      op.kind = IntervalOp::Kind::Compare;
      op.bitWidth = getIntegerBitWidth(Cmp->getOperand(0));
      op.lhs = Cmp->getOperand(0);
      op.rhs = Cmp->getOperand(1);
      op.opcode = static_cast<unsigned>(Cmp->getPredicate());
      return true;
    }

    if (auto *BinOp = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
      op.kind = IntervalOp::Kind::Binary;
      op.bitWidth = getIntegerBitWidth(&I);
      op.lhs = BinOp->getOperand(0);
      op.rhs = BinOp->getOperand(1);
      op.opcode = BinOp->getOpcode();
      return true;
    }

    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
      op.kind = IntervalOp::Kind::Cast;
      op.lhs = Cast->getOperand(0);
      op.opcode = Cast->getOpcode();
      op.bitWidth = getIntegerBitWidth(&I);
      op.sourceBitWidth = getIntegerBitWidth(Cast->getOperand(0));
      if (!isTrackedScalar(Cast->getOperand(0)))
        op.kind = IntervalOp::Kind::Forget;
      return true;
    }

    llvm::APInt value(1, 0);
    if (getConstantAPInt(&I, value)) {
      op.kind = IntervalOp::Kind::AssignConst;
      op.constant = value;
      return true;
    }

    op.kind = IntervalOp::Kind::Forget;
    return true;
  }

  Interval readValue(const FactType &state, const llvm::Value *V) const {
    llvm::APInt value(1, 0);
    if (getConstantAPInt(V, value))
      return pointInterval(value);
    if (!isTrackedScalar(V))
      return topInterval();
    auto It = state.values.find(V);
    if (It == state.values.end())
      return topInterval(getIntegerBitWidth(V));
    return It->second;
  }

  void writeValue(FactType &state, const llvm::Value *dest,
                  const Interval &value) const {
    if (!isTrackedScalar(dest))
      return;
    if (value == topInterval(intervalBitWidth(value), value.ordering))
      state.values.erase(dest);
    else
      state.values[dest] = value;
  }

  Interval evalBinary(unsigned opcode, unsigned bitWidth, Interval lhs,
                      Interval rhs) const {
    if (!lhs.hasLower || !lhs.hasUpper || !rhs.hasLower || !rhs.hasUpper)
      return topInterval(bitWidth);
    if (!lhs.isExact() && !rhs.isExact() && lhs.ordering != rhs.ordering)
      return topInterval(bitWidth);

    IntervalOrdering ordering = IntervalOrdering::Signed;
    if (opcode == llvm::Instruction::UDiv)
      ordering = IntervalOrdering::Unsigned;
    else if (!lhs.isExact())
      ordering = lhs.ordering;
    else if (!rhs.isExact())
      ordering = rhs.ordering;
    auto endpoints = [&](const Interval &I) {
      return std::array<llvm::APInt, 2>{I.lower, I.upper};
    };

    auto chooseBounds = [&](const std::vector<llvm::APInt> &values) {
      Interval out = topInterval(bitWidth, ordering);
      out.hasLower = true;
      out.hasUpper = true;
      out.lower = values.front();
      out.upper = values.front();
      for (const auto &value : values) {
        out.lower = orderedMin(out.lower, value, ordering);
        out.upper = orderedMax(out.upper, value, ordering);
      }
      return out;
    };

    switch (opcode) {
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::Mul: {
      std::vector<llvm::APInt> values;
      for (const auto &l : endpoints(lhs)) {
        for (const auto &r : endpoints(rhs)) {
          llvm::APInt result(bitWidth, 0);
          if (opcode == llvm::Instruction::Add)
            result = l + r;
          else if (opcode == llvm::Instruction::Sub)
            result = l - r;
          else
            result = l * r;
          values.push_back(result);
        }
      }
      return chooseBounds(values);
    }
    case llvm::Instruction::SDiv: {
      if (containsZero(rhs))
        return topInterval(bitWidth, IntervalOrdering::Signed);
      std::vector<llvm::APInt> values;
      for (const auto &l : endpoints(lhs))
        for (const auto &r : endpoints(rhs)) {
          if (r.isZero())
            return topInterval(bitWidth, IntervalOrdering::Signed);
          values.push_back(l.sdiv(r));
        }
      return chooseBounds(values);
    }
    case llvm::Instruction::UDiv: {
      if (containsZero(rhs))
        return topInterval(bitWidth, IntervalOrdering::Unsigned);
      std::vector<llvm::APInt> values;
      for (const auto &l : endpoints(lhs))
        for (const auto &r : endpoints(rhs)) {
          if (r.isZero())
            return topInterval(bitWidth, IntervalOrdering::Unsigned);
          values.push_back(l.udiv(r));
        }
      return chooseBounds(values);
    }
    default:
      return topInterval(bitWidth);
    }
  }

  Interval evalCompare(unsigned predicate, unsigned, Interval lhs,
                       Interval rhs) const {
    if (lhs.isExact() && rhs.isExact()) {
      bool result = llvm::ICmpInst::compare(
          lhs.lower, rhs.lower,
          static_cast<llvm::CmpInst::Predicate>(predicate));
      return pointInterval(llvm::APInt(1, result ? 1 : 0));
    }

    if (lhs.hasLower && lhs.hasUpper && rhs.hasLower && rhs.hasUpper &&
        lhs.ordering == rhs.ordering) {
      switch (static_cast<llvm::CmpInst::Predicate>(predicate)) {
      case llvm::CmpInst::ICMP_EQ:
        if (orderedLess(lhs.upper, rhs.lower, lhs.ordering) ||
            orderedLess(rhs.upper, lhs.lower, lhs.ordering))
          return pointInterval(llvm::APInt(1, 0));
        break;
      case llvm::CmpInst::ICMP_NE:
        if (orderedLess(lhs.upper, rhs.lower, lhs.ordering) ||
            orderedLess(rhs.upper, lhs.lower, lhs.ordering))
          return pointInterval(llvm::APInt(1, 1));
        break;
      case llvm::CmpInst::ICMP_SLT:
        if (lhs.ordering == IntervalOrdering::Signed) {
          if (orderedLess(lhs.upper, rhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 1));
          if (orderedLessEqual(rhs.upper, lhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 0));
        }
        break;
      case llvm::CmpInst::ICMP_SLE:
        if (lhs.ordering == IntervalOrdering::Signed) {
          if (orderedLessEqual(lhs.upper, rhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 1));
          if (orderedLess(rhs.upper, lhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 0));
        }
        break;
      case llvm::CmpInst::ICMP_SGT:
        if (lhs.ordering == IntervalOrdering::Signed) {
          if (orderedLess(rhs.upper, lhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 1));
          if (orderedLessEqual(lhs.upper, rhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 0));
        }
        break;
      case llvm::CmpInst::ICMP_SGE:
        if (lhs.ordering == IntervalOrdering::Signed) {
          if (orderedLessEqual(rhs.upper, lhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 1));
          if (orderedLess(lhs.upper, rhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 0));
        }
        break;
      case llvm::CmpInst::ICMP_ULT:
        if (lhs.ordering == IntervalOrdering::Unsigned) {
          if (orderedLess(lhs.upper, rhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 1));
          if (orderedLessEqual(rhs.upper, lhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 0));
        }
        break;
      case llvm::CmpInst::ICMP_ULE:
        if (lhs.ordering == IntervalOrdering::Unsigned) {
          if (orderedLessEqual(lhs.upper, rhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 1));
          if (orderedLess(rhs.upper, lhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 0));
        }
        break;
      case llvm::CmpInst::ICMP_UGT:
        if (lhs.ordering == IntervalOrdering::Unsigned) {
          if (orderedLess(rhs.upper, lhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 1));
          if (orderedLessEqual(lhs.upper, rhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 0));
        }
        break;
      case llvm::CmpInst::ICMP_UGE:
        if (lhs.ordering == IntervalOrdering::Unsigned) {
          if (orderedLessEqual(rhs.upper, lhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 1));
          if (orderedLess(lhs.upper, rhs.lower, lhs.ordering))
            return pointInterval(llvm::APInt(1, 0));
        }
        break;
      default:
        break;
      }
    }

    Interval out = topInterval(1);
    out.hasLower = true;
    out.hasUpper = true;
    out.lower = llvm::APInt(1, 0);
    out.upper = llvm::APInt(1, 1);
    return out;
  }

  Interval evalCast(const FactType &state, const IntervalOp &op) const {
    Interval src = readValue(state, op.lhs);
    if (!src.hasLower || !src.hasUpper)
      return topInterval(op.bitWidth);
    if (src.isExact())
      return applyCastToConstant(op.opcode, op.bitWidth, src.lower);

    switch (op.opcode) {
    case llvm::Instruction::SExt:
      if (src.ordering != IntervalOrdering::Signed)
        return topInterval(op.bitWidth);
      return Interval{false,
                      true,
                      true,
                      IntervalOrdering::Signed,
                      src.lower.sext(op.bitWidth),
                      src.upper.sext(op.bitWidth)};
    case llvm::Instruction::ZExt:
      if (src.ordering != IntervalOrdering::Unsigned)
        return topInterval(op.bitWidth, IntervalOrdering::Unsigned);
      return Interval{false,
                      true,
                      true,
                      IntervalOrdering::Unsigned,
                      src.lower.zext(op.bitWidth),
                      src.upper.zext(op.bitWidth)};
    case llvm::Instruction::Trunc:
      return topInterval(op.bitWidth);
    default:
      return topInterval(op.bitWidth);
    }
  }

  void applyOp(FactType &state, const IntervalOp &op) const {
    switch (op.kind) {
    case IntervalOp::Kind::AssignConst:
      writeValue(state, op.dest, pointInterval(op.constant, op.ordering));
      return;
    case IntervalOp::Kind::Copy:
      writeValue(state, op.dest, readValue(state, op.lhs));
      return;
    case IntervalOp::Kind::Cast:
      writeValue(state, op.dest, evalCast(state, op));
      return;
    case IntervalOp::Kind::Binary:
      writeValue(state, op.dest,
                 evalBinary(op.opcode, op.bitWidth, readValue(state, op.lhs),
                            readValue(state, op.rhs)));
      return;
    case IntervalOp::Kind::Compare:
      writeValue(state, op.dest,
                 evalCompare(op.opcode, op.bitWidth, readValue(state, op.lhs),
                             readValue(state, op.rhs)));
      return;
    case IntervalOp::Kind::AssumeNotCases: {
      Interval cond = readValue(state, op.cond);
      Interval refined = refineDefaultSwitch(cond, constantCases(op.inputs));
      if (refined.bottom) {
        state.reachable = false;
        state.values.clear();
        return;
      }
      writeValue(state, op.cond, refined);
      return;
    }
    case IntervalOp::Kind::Select: {
      Interval cond = readValue(state, op.cond);
      if (isDefinitelyZero(cond) || isDefinitelyNonZero(cond)) {
        writeValue(
            state, op.dest,
            readValue(state, isDefinitelyNonZero(cond) ? op.lhs : op.rhs));
      } else {
        writeValue(
            state, op.dest,
            joinIntervals(readValue(state, op.lhs), readValue(state, op.rhs)));
      }
      return;
    }
    case IntervalOp::Kind::Phi: {
      bool first = true;
      Interval joined = topInterval(op.bitWidth);
      for (const llvm::Value *Input : op.inputs) {
        Interval current = readValue(state, Input);
        if (first) {
          joined = current;
          first = false;
        } else {
          joined = joinIntervals(joined, current);
        }
      }
      writeValue(state, op.dest, first ? topInterval(op.bitWidth) : joined);
      return;
    }
    case IntervalOp::Kind::Forget:
      state.values.erase(op.dest);
      return;
    }
  }
};

} // namespace

InterIntervalAnalysis::Result
InterIntervalAnalysis::run(llvm::Module &M, bool verbose,
                           LinearStrategy linearStrategy,
                           IndirectCallResolutionMode callResolutionMode) {
  IntervalAnalysis analysis;
  auto engineResult = InterEngine<IntervalSummary, IntervalAnalysis>::run(
      M, analysis, verbose, linearStrategy, callResolutionMode);

  Result result;
  result.status = engineResult.status;
  result.summaries.insert(engineResult.summaries.begin(),
                          engineResult.summaries.end());
  result.blockFacts.insert(engineResult.blockEntryFacts.begin(),
                           engineResult.blockEntryFacts.end());
  return result;
}

} // namespace npa
