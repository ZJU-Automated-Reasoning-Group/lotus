#include "Dataflow/Mono/Analyses/Inter/ConstantPropagation.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Solver/InterSolver.h"

#include <algorithm>
#include <memory>

using namespace llvm;

namespace mono {
namespace {

ConstantPropagationValue makeTop() {
  return ConstantPropagationValue{ConstantPropagationTag::Top, 0};
}

ConstantPropagationValue makeBottom() {
  return ConstantPropagationValue{ConstantPropagationTag::Bottom, 0};
}

ConstantPropagationValue makeConst(int64_t Value) {
  return ConstantPropagationValue{ConstantPropagationTag::Const, Value};
}

bool isBottom(const ConstantPropagationValue &V) {
  return V.Tag == ConstantPropagationTag::Bottom;
}

bool isConst(const ConstantPropagationValue &V) {
  return V.Tag == ConstantPropagationTag::Const;
}

bool equalValue(const ConstantPropagationValue &Lhs,
                const ConstantPropagationValue &Rhs) {
  return Lhs.Tag == Rhs.Tag && Lhs.ConstValue == Rhs.ConstValue;
}

ConstantPropagationValue resolveValue(const ConstantPropagationMap &In,
                                      const Value *V) {
  if (auto *CI = dyn_cast<ConstantInt>(V)) {
    return makeConst(CI->getSExtValue());
  }

  auto It = In.find(V);
  if (It != In.end()) {
    return It->second;
  }

  return makeTop();
}

std::vector<Function *>
resolveIndirectCalleesWithAA(Instruction *CallSite,
                             lotus::AliasAnalysisWrapper *AA) {
  std::vector<Function *> Callees;
  auto *Call = dyn_cast_or_null<CallBase>(CallSite);
  if (Call == nullptr || AA == nullptr || !AA->isInitialized()) {
    return Callees;
  }

  std::vector<const Function *> Targets;
  AA->getIndirectCallTargets(Call, Targets);
  for (const auto *Target : Targets) {
    if (Target == nullptr) {
      continue;
    }
    auto *MutableTarget = const_cast<Function *>(Target);
    if (std::find(Callees.begin(), Callees.end(), MutableTarget) ==
        Callees.end()) {
      Callees.push_back(MutableTarget);
    }
  }
  return Callees;
}

ConstantPropagationValue evalBinaryOp(unsigned Opcode,
                                      const ConstantPropagationValue &Lhs,
                                      const ConstantPropagationValue &Rhs) {
  // Bottom (unreachable) propagates as Bottom; Top (unknown) yields
  // Top, not Bottom.  The old code returned Bottom for any non-Const input,
  // which incorrectly treated unknown values as unreachable.
  if (isBottom(Lhs) || isBottom(Rhs)) {
    return makeBottom();
  }
  if (!isConst(Lhs) || !isConst(Rhs)) {
    return makeTop();
  }

  auto LV = Lhs.ConstValue;
  auto RV = Rhs.ConstValue;
  switch (Opcode) {
  case Instruction::Add:
    return makeConst(LV + RV);
  case Instruction::Sub:
    return makeConst(LV - RV);
  case Instruction::Mul:
    return makeConst(LV * RV);
  case Instruction::SDiv:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(LV / RV);
  case Instruction::UDiv:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(static_cast<uint64_t>(LV) / static_cast<uint64_t>(RV));
  case Instruction::SRem:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(LV % RV);
  case Instruction::URem:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(static_cast<uint64_t>(LV) % static_cast<uint64_t>(RV));
  default:
    // Unknown opcode: result is unknown (Top), not unreachable (Bottom).
    return makeTop();
  }
}

class InterMonoConstantPropagation
    : public InterMonoProblem<ConstantPropagationAnalysisTypes> {
public:
  explicit InterMonoConstantPropagation(Function *Entry,
                                        lotus::AliasAnalysisWrapper *AA)
      : InterMonoProblem<ConstantPropagationAnalysisTypes>(
            std::vector<Function *>{Entry}, AA),
        AA(AA) {}

  ConstantPropagationMap normalFlow(Instruction *Inst,
                                    const ConstantPropagationMap &In) override {
    if (getAbstractDomain().isBottom(In))
      return In;
    ConstantPropagationMap Out = In;

    if (const auto *Alloca = dyn_cast<AllocaInst>(Inst)) {
      if (Alloca->getAllocatedType()->isIntegerTy()) {
        Out[Alloca] = makeTop();
      }
      return Out;
    }

    if (const auto *Store = dyn_cast<StoreInst>(Inst)) {
      auto *Ptr = Store->getPointerOperand();
      if (!Store->getValueOperand()->getType()->isIntegerTy()) {
        return Out;
      }

      auto Val = resolveValue(In, Store->getValueOperand());
      if (!isBottom(Val)) {
        Out[Ptr] = Val;
        writeAliases(Ptr, Val, Out);
      }
      return Out;
    }

    if (const auto *Load = dyn_cast<LoadInst>(Inst)) {
      auto It = In.find(Load->getPointerOperand());
      if (It != In.end()) {
        Out[Load] = It->second;
      } else {
        Out[Load] = resolveAliasValue(Load->getPointerOperand(), In);
      }
      return Out;
    }

    if (const auto *Op = dyn_cast<BinaryOperator>(Inst)) {
      auto Lhs = resolveValue(In, Op->getOperand(0));
      auto Rhs = resolveValue(In, Op->getOperand(1));
      Out[Op] = evalBinaryOp(Op->getOpcode(), Lhs, Rhs);
      return Out;
    }

    return Out;
  }

  ConstantPropagationMap callFlow(Instruction *CallSite, Function *Callee,
                                  const ConstantPropagationMap &In) override {
    if (getAbstractDomain().isBottom(In))
      return In;
    ConstantPropagationMap Out;
    if (CallSite == nullptr || Callee == nullptr) {
      return Out;
    }

    auto *Call = dyn_cast<CallBase>(CallSite);
    if (Call == nullptr) {
      return Out;
    }

    // Map constants from actual args to formal args.
    auto *FormalIt = Callee->arg_begin();
    for (auto &Actual : Call->args()) {
      if (FormalIt == Callee->arg_end()) {
        break;
      }
      if (FormalIt->getType()->isIntegerTy()) {
        Out[&*FormalIt] = resolveValue(In, Actual.get());
      }
      ++FormalIt;
    }

    // Preserve globals (very conservative).
    for (const auto &Entry : In) {
      if (Entry.first != nullptr && isa<GlobalValue>(Entry.first)) {
        Out.insert(Entry);
      }
    }

    return Out;
  }

  ConstantPropagationMap returnFlow(Instruction *CallSite, Function *Callee,
                                    Instruction *ExitStmt, Instruction *RetSite,
                                    const ConstantPropagationMap &In) override {
    if (getAbstractDomain().isBottom(In))
      return In;
    (void)Callee;
    (void)RetSite;

    ConstantPropagationMap Out;
    for (const auto &Entry : In) {
      if (Entry.first != nullptr && isa<GlobalValue>(Entry.first)) {
        Out.insert(Entry);
      }
    }

    auto *Ret = dyn_cast_or_null<ReturnInst>(ExitStmt);
    auto *Call = dyn_cast_or_null<CallBase>(CallSite);
    if (Ret == nullptr || Call == nullptr) {
      return Out;
    }
    if (Call->getType()->isVoidTy()) {
      return Out;
    }

    auto *RetVal = Ret->getReturnValue();
    if (RetVal == nullptr || !RetVal->getType()->isIntegerTy()) {
      return Out;
    }

    Out[CallSite] = resolveValue(In, RetVal);
    return Out;
  }

  ConstantPropagationMap
  callToRetFlow(Instruction *CallSite, Instruction *RetSite,
                ArrayRef<Function *> Callees,
                const ConstantPropagationMap &In) override {
    if (getAbstractDomain().isBottom(In))
      return In;
    (void)RetSite;
    ConstantPropagationMap Out = In;

    auto *Call = dyn_cast_or_null<CallBase>(CallSite);
    if (Call == nullptr) {
      return Out;
    }
    if (Call->getType()->isVoidTy()) {
      return Out;
    }

    if (Callees.empty()) {
      Out[CallSite] = makeTop();
      return Out;
    }

    bool AllDefined = true;
    for (auto *Callee : Callees) {
      if (Callee == nullptr || Callee->isDeclaration() || Callee->empty()) {
        AllDefined = false;
        break;
      }
    }
    if (AllDefined) {
      // The call-to-return edge bypasses the callee body, so the call result
      // fact itself must contribute nothing here. Use Bottom so returnFlow can
      // establish the precise value without being overwritten by implicit Top.
      Out[CallSite] = makeBottom();
      return Out;
    }

    // Unknown/external path: the return value is unknown (Top).
    Out[CallSite] = makeTop();
    return Out;
  }

  std::unordered_map<Instruction *, ConstantPropagationMap>
  initialSeeds() override {
    std::unordered_map<Instruction *, ConstantPropagationMap> Seeds;
    Function *F = this->getEntryPoints().empty()
                      ? nullptr
                      : this->getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }
    Seeds[&F->getEntryBlock().front()] = ConstantPropagationMap{};
    return Seeds;
  }

  std::vector<Function *>
  resolve_indirect_callees(Instruction *CallSite) const override {
    auto Callees = resolveIndirectCalleesWithAA(CallSite, AA);
    if (!Callees.empty()) {
      return Callees;
    }
    return InterMonoProblem<ConstantPropagationAnalysisTypes>::resolve_indirect_callees(
        CallSite);
  }

private:
  lotus::AliasAnalysisWrapper *AA;

  // weakJoin: for may-aliased pointers, conflicting values → Top (unknown),
  // not Bottom (unreachable).
  static ConstantPropagationValue
  weakJoin(const ConstantPropagationValue &OldVal,
           const ConstantPropagationValue &NewVal) {
    if (equalValue(OldVal, NewVal)) {
      return OldVal;
    }
    return makeTop();
  }

  void collectAliasedPointers(const Value *Ptr,
                              const ConstantPropagationMap &State,
                              std::vector<const Value *> &MustAliases,
                              std::vector<const Value *> &MayAliases) const {
    if (Ptr == nullptr || !Ptr->getType()->isPointerTy()) {
      return;
    }

    const bool HaveAA = AA != nullptr && AA->isInitialized();
    const Value *PtrBase = Ptr->stripPointerCasts();
    auto Add = [&](const Value *Candidate) {
      if (Candidate == nullptr || !Candidate->getType()->isPointerTy()) {
        return;
      }
      const Value *CandidateBase = Candidate->stripPointerCasts();
      if (PtrBase != nullptr && CandidateBase != nullptr &&
          PtrBase == CandidateBase) {
        MustAliases.push_back(Candidate);
        return;
      }
      if (Candidate == Ptr) {
        MustAliases.push_back(Candidate);
        return;
      }
      if (!HaveAA) {
        MayAliases.push_back(Candidate);
        return;
      }
      auto Res = AA->query(Ptr, Candidate);
      if (Res == AliasResult::MustAlias) {
        MustAliases.push_back(Candidate);
      } else if (Res != AliasResult::NoAlias) {
        MayAliases.push_back(Candidate);
      }
    };

    Add(Ptr);
    for (const auto &Entry : State) {
      Add(Entry.first);
    }
  }

  ConstantPropagationValue
  resolveAliasValue(const Value *Ptr, const ConstantPropagationMap &In) const {
    if (AA == nullptr || !AA->isInitialized() || Ptr == nullptr ||
        !Ptr->getType()->isPointerTy()) {
      return makeTop();
    }

    bool Found = false;
    ConstantPropagationValue Val = makeTop();
    for (const auto &Entry : In) {
      auto *Alias = Entry.first;
      if (Alias == nullptr || !Alias->getType()->isPointerTy()) {
        continue;
      }
      if (AA->query(Ptr, Alias) == AliasResult::NoAlias) {
        continue;
      }
      auto It = In.find(Alias);
      if (It == In.end()) {
        continue;
      }
      if (!Found) {
        Val = It->second;
        Found = true;
      } else if (!equalValue(Val, It->second)) {
        // Multiple aliases disagree → unknown (Top), not unreachable (Bottom).
        return makeTop();
      }
    }
    return Found ? Val : makeTop();
  }

  void writeAliases(const Value *Ptr, const ConstantPropagationValue &Val,
                    ConstantPropagationMap &Out) const {
    std::vector<const Value *> MustAliases;
    std::vector<const Value *> MayAliases;
    collectAliasedPointers(Ptr, Out, MustAliases, MayAliases);

    for (const auto *Alias : MustAliases) {
      if (Alias != nullptr) {
        Out[Alias] = Val;
      }
    }

    for (const auto *Alias : MayAliases) {
      if (Alias == nullptr) {
        continue;
      }
      auto It = Out.find(Alias);
      auto OldVal = It != Out.end() ? It->second : makeTop();
      Out[Alias] = weakJoin(OldVal, Val);
    }
  }
};

} // namespace

InterMonoConstantPropagationAnalysisResult
runInterMonoConstantPropagation(Function *Entry) {
  InterMonoConstantPropagationAnalysisResult Result;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Result;
  }

  auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
      *Entry->getParent(),
      lotus::AAConfig(lotus::AAConfig::Implementation::DyckAA,
                      lotus::AAConfig::ContextSensitivity::None, 0, true,
                      lotus::AAConfig::Solver::Default));
  InterMonoConstantPropagation Problem(Entry, AA.get());
  InterMonoSolver<ConstantPropagationAnalysisTypes,
                  kDefaultConstantPropagationCallStringLength>
      Solver(Problem);
  Solver.solve();

  if (const auto *Raw = Solver.getResults()) {
    Result.Results = std::make_unique<InterMonoConstantPropagationResult>(*Raw);
  }
  return Result;
}

} // namespace mono
