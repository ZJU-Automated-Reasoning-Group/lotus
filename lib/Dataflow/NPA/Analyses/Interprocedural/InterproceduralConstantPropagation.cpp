/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralConstantPropagation.h"

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"

#include <algorithm>

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

using D = ConstantPropagationDomain;
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

ConstantPropagationValue topValue() { return {}; }

ConstantPropagationValue constValue(const llvm::APInt &value) {
  ConstantPropagationValue out;
  out.tag = ConstantPropagationTag::Const;
  out.constant = value;
  return out;
}

} // namespace

bool ConstantPropagationOp::operator<(
    const ConstantPropagationOp &other) const {
  if (kind != other.kind)
    return kind < other.kind;
  if (dest != other.dest)
    return dest < other.dest;
  if (lhs != other.lhs)
    return lhs < other.lhs;
  if (rhs != other.rhs)
    return rhs < other.rhs;
  if (cond != other.cond)
    return cond < other.cond;
  if (opcode != other.opcode)
    return opcode < other.opcode;
  if (bitWidth != other.bitWidth)
    return bitWidth < other.bitWidth;
  if (sourceBitWidth != other.sourceBitWidth)
    return sourceBitWidth < other.sourceBitWidth;
  if (!apIntEqual(constant, other.constant))
    return apIntLess(constant, other.constant);
  return inputs < other.inputs;
}

bool ConstantPropagationOp::operator==(
    const ConstantPropagationOp &other) const {
  return kind == other.kind && dest == other.dest && lhs == other.lhs &&
         rhs == other.rhs && cond == other.cond && opcode == other.opcode &&
         bitWidth == other.bitWidth && sourceBitWidth == other.sourceBitWidth &&
         apIntEqual(constant, other.constant) && inputs == other.inputs;
}

namespace {

class ConstantPropagationAnalysis {
public:
  using FactType = ConstantPropagationState;
  using Engine = InterproceduralEngine<D, ConstantPropagationAnalysis>;

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
          llvm::APInt(1, Branch->getSuccessor(0) == &succ ? 1 : 0));
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
                             Case.getCaseValue()->getValue());
      }
      if (Switch->getDefaultDest() == &succ && !Switch->cases().empty()) {
        ConstantPropagationOp op;
        op.kind = ConstantPropagationOp::Kind::AssumeNotCases;
        op.cond = Switch->getCondition();
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

    ConstantPropagationOp op;
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

    ConstantPropagationOp op;
    op.dest = &Call;
    op.kind = ConstantPropagationOp::Kind::Phi;
    for (const auto &BB : Callee) {
      auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator());
      if (!Ret)
        continue;
      if (const llvm::Value *RetVal = Ret->getReturnValue())
        op.inputs.push_back(RetVal);
    }
    if (op.inputs.empty())
      op.kind = ConstantPropagationOp::Kind::Forget;
    return D::singleton(op);
  }

  D::value_type getCallToReturnTransfer(const llvm::CallBase &Call) {
    if (Call.getType()->isVoidTy() || !isTrackedScalar(&Call))
      return D::one();
    ConstantPropagationOp op;
    op.kind = ConstantPropagationOp::Kind::Forget;
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
      if (entry.second == It->second)
        out.values[entry.first] = entry.second;
    }
    return out;
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

  D::value_type buildAssign(const llvm::Value *dest,
                            const llvm::APInt &value) const {
    ConstantPropagationOp op;
    op.dest = dest;
    op.kind = ConstantPropagationOp::Kind::AssignConst;
    op.bitWidth = getIntegerBitWidth(dest);
    op.constant = value;
    return D::singleton(op);
  }

  D::value_type buildAssign(const llvm::Value *dest,
                            const llvm::Value *src) const {
    ConstantPropagationOp op;
    op.dest = dest;
    op.bitWidth = getIntegerBitWidth(dest);
    llvm::APInt value(1, 0);
    if (getConstantAPInt(src, value)) {
      op.kind = ConstantPropagationOp::Kind::AssignConst;
      op.constant = value;
    } else if (isTrackedScalar(src)) {
      op.kind = ConstantPropagationOp::Kind::Copy;
      op.lhs = src;
    } else {
      op.kind = ConstantPropagationOp::Kind::Forget;
    }
    return D::singleton(op);
  }

  bool buildTransfer(llvm::Instruction &I, ConstantPropagationOp &op) const {
    op.dest = &I;
    op.bitWidth = getIntegerBitWidth(&I);
    if (!isTrackedScalar(&I)) {
      op.kind = ConstantPropagationOp::Kind::Forget;
      return true;
    }

    if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
      op.kind = ConstantPropagationOp::Kind::Phi;
      for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i)
        op.inputs.push_back(Phi->getIncomingValue(i));
      return true;
    }

    if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(&I)) {
      op.kind = ConstantPropagationOp::Kind::Select;
      op.cond = Select->getCondition();
      op.lhs = Select->getTrueValue();
      op.rhs = Select->getFalseValue();
      return true;
    }

    if (auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(&I)) {
      op.kind = ConstantPropagationOp::Kind::Compare;
      op.bitWidth = getIntegerBitWidth(Cmp->getOperand(0));
      op.lhs = Cmp->getOperand(0);
      op.rhs = Cmp->getOperand(1);
      op.opcode = static_cast<unsigned>(Cmp->getPredicate());
      return true;
    }

    if (auto *BinOp = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
      op.kind = ConstantPropagationOp::Kind::Binary;
      op.lhs = BinOp->getOperand(0);
      op.rhs = BinOp->getOperand(1);
      op.opcode = BinOp->getOpcode();
      return true;
    }

    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
      op.kind = ConstantPropagationOp::Kind::Cast;
      op.lhs = Cast->getOperand(0);
      op.opcode = Cast->getOpcode();
      op.sourceBitWidth = getIntegerBitWidth(Cast->getOperand(0));
      if (!isTrackedScalar(Cast->getOperand(0)))
        op.kind = ConstantPropagationOp::Kind::Forget;
      return true;
    }

    if (auto *Freeze = llvm::dyn_cast<llvm::FreezeInst>(&I)) {
      if (isTrackedScalar(Freeze->getOperand(0))) {
        op.kind = ConstantPropagationOp::Kind::Copy;
        op.lhs = Freeze->getOperand(0);
      } else {
        op.kind = ConstantPropagationOp::Kind::Forget;
      }
      return true;
    }

    llvm::APInt value(1, 0);
    if (getConstantAPInt(&I, value)) {
      op.kind = ConstantPropagationOp::Kind::AssignConst;
      op.constant = value;
      return true;
    }

    op.kind = ConstantPropagationOp::Kind::Forget;
    return true;
  }

  ConstantPropagationValue readValue(const FactType &state,
                                     const llvm::Value *V) const {
    llvm::APInt value(1, 0);
    if (getConstantAPInt(V, value))
      return constValue(value);
    if (!isTrackedScalar(V))
      return topValue();
    auto It = state.values.find(V);
    if (It == state.values.end())
      return topValue();
    return It->second;
  }

  void writeValue(FactType &state, const llvm::Value *dest,
                  ConstantPropagationValue value) const {
    if (!isTrackedScalar(dest))
      return;
    if (value.isConstant())
      state.values[dest] = std::move(value);
    else
      state.values.erase(dest);
  }

  ConstantPropagationValue evalBinary(unsigned opcode, unsigned,
                                      ConstantPropagationValue lhs,
                                      ConstantPropagationValue rhs) const {
    if (!lhs.isConstant() || !rhs.isConstant())
      return topValue();

    llvm::APInt result(lhs.constant.getBitWidth(), 0);
    switch (opcode) {
    case llvm::Instruction::Add:
      result = lhs.constant + rhs.constant;
      break;
    case llvm::Instruction::Sub:
      result = lhs.constant - rhs.constant;
      break;
    case llvm::Instruction::Mul:
      result = lhs.constant * rhs.constant;
      break;
    case llvm::Instruction::SDiv:
      if (rhs.constant.isZero())
        return topValue();
      result = lhs.constant.sdiv(rhs.constant);
      break;
    case llvm::Instruction::UDiv:
      if (rhs.constant.isZero())
        return topValue();
      result = lhs.constant.udiv(rhs.constant);
      break;
    case llvm::Instruction::SRem:
      if (rhs.constant.isZero())
        return topValue();
      result = lhs.constant.srem(rhs.constant);
      break;
    case llvm::Instruction::URem:
      if (rhs.constant.isZero())
        return topValue();
      result = lhs.constant.urem(rhs.constant);
      break;
    case llvm::Instruction::And:
      result = lhs.constant & rhs.constant;
      break;
    case llvm::Instruction::Or:
      result = lhs.constant | rhs.constant;
      break;
    case llvm::Instruction::Xor:
      result = lhs.constant ^ rhs.constant;
      break;
    case llvm::Instruction::Shl:
      if (rhs.constant.uge(lhs.constant.getBitWidth()))
        return topValue();
      result = lhs.constant.shl(rhs.constant.getLimitedValue());
      break;
    case llvm::Instruction::LShr:
      if (rhs.constant.uge(lhs.constant.getBitWidth()))
        return topValue();
      result = lhs.constant.lshr(rhs.constant.getLimitedValue());
      break;
    case llvm::Instruction::AShr:
      if (rhs.constant.uge(lhs.constant.getBitWidth()))
        return topValue();
      result = lhs.constant.ashr(rhs.constant.getLimitedValue());
      break;
    default:
      return topValue();
    }
    return constValue(result);
  }

  ConstantPropagationValue evalCompare(unsigned predicate, unsigned,
                                       ConstantPropagationValue lhs,
                                       ConstantPropagationValue rhs) const {
    if (!lhs.isConstant() || !rhs.isConstant())
      return topValue();
    bool result = llvm::ICmpInst::compare(
        lhs.constant, rhs.constant,
        static_cast<llvm::CmpInst::Predicate>(predicate));
    return constValue(llvm::APInt(1, result ? 1 : 0));
  }

  ConstantPropagationValue evalCast(const FactType &state,
                                    const ConstantPropagationOp &op) const {
    ConstantPropagationValue src = readValue(state, op.lhs);
    if (!src.isConstant())
      return topValue();

    llvm::APInt casted = src.constant;
    switch (op.opcode) {
    case llvm::Instruction::SExt:
      casted = casted.sext(op.bitWidth);
      break;
    case llvm::Instruction::ZExt:
      casted = casted.zext(op.bitWidth);
      break;
    case llvm::Instruction::Trunc:
      casted = casted.trunc(op.bitWidth);
      break;
    default:
      return topValue();
    }
    return constValue(casted);
  }

  ConstantPropagationValue joinValues(ConstantPropagationValue lhs,
                                      ConstantPropagationValue rhs) const {
    if (lhs.isConstant() && rhs.isConstant() &&
        apIntEqual(lhs.constant, rhs.constant))
      return lhs;
    return topValue();
  }

  void applyOp(FactType &state, const ConstantPropagationOp &op) const {
    switch (op.kind) {
    case ConstantPropagationOp::Kind::AssignConst:
      writeValue(state, op.dest, constValue(op.constant));
      return;
    case ConstantPropagationOp::Kind::Copy:
      writeValue(state, op.dest, readValue(state, op.lhs));
      return;
    case ConstantPropagationOp::Kind::Binary:
      writeValue(state, op.dest,
                 evalBinary(op.opcode, op.bitWidth, readValue(state, op.lhs),
                            readValue(state, op.rhs)));
      return;
    case ConstantPropagationOp::Kind::Cast:
      writeValue(state, op.dest, evalCast(state, op));
      return;
    case ConstantPropagationOp::Kind::Compare:
      writeValue(state, op.dest,
                 evalCompare(op.opcode, op.bitWidth, readValue(state, op.lhs),
                             readValue(state, op.rhs)));
      return;
    case ConstantPropagationOp::Kind::AssumeNotCases: {
      ConstantPropagationValue cond = readValue(state, op.cond);
      if (!cond.isConstant())
        return;
      for (const llvm::Value *Input : op.inputs) {
        llvm::APInt caseValue(1, 0);
        if (!getConstantAPInt(Input, caseValue))
          continue;
        if (apIntEqual(cond.constant, caseValue)) {
          state.reachable = false;
          state.values.clear();
          return;
        }
      }
      return;
    }
    case ConstantPropagationOp::Kind::Phi: {
      ConstantPropagationValue joined = topValue();
      bool first = true;
      for (const llvm::Value *Input : op.inputs) {
        auto value = readValue(state, Input);
        if (first) {
          joined = value;
          first = false;
        } else {
          joined = joinValues(joined, value);
        }
      }
      writeValue(state, op.dest, first ? topValue() : joined);
      return;
    }
    case ConstantPropagationOp::Kind::Select: {
      ConstantPropagationValue cond = readValue(state, op.cond);
      if (cond.isConstant()) {
        writeValue(state, op.dest,
                   readValue(state, cond.constant.isZero() ? op.rhs : op.lhs));
      } else {
        writeValue(
            state, op.dest,
            joinValues(readValue(state, op.lhs), readValue(state, op.rhs)));
      }
      return;
    }
    case ConstantPropagationOp::Kind::Forget:
      state.values.erase(op.dest);
      return;
    }
  }
};

} // namespace

InterproceduralConstantPropagation::Result
InterproceduralConstantPropagation::run(
    llvm::Module &M, bool verbose, LinearStrategy linearStrategy,
    IndirectCallResolutionMode callResolutionMode) {
  ConstantPropagationAnalysis analysis;
  auto engineResult = InterproceduralEngine<
      ConstantPropagationDomain,
      ConstantPropagationAnalysis>::run(M, analysis, verbose, linearStrategy,
                                        callResolutionMode);

  Result result;
  result.status = engineResult.status;
  result.summaries.insert(engineResult.summaries.begin(),
                          engineResult.summaries.end());
  result.blockFacts.insert(engineResult.blockEntryFacts.begin(),
                           engineResult.blockEntryFacts.end());
  return result;
}

} // namespace npa
