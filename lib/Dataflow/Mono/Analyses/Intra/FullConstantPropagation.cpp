#include "Dataflow/Mono/Analyses/Intra/FullConstantPropagation.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

#include <memory>

using namespace llvm;

namespace mono {
namespace {

FullConstantValue joinValues(const FullConstantValue &Lhs,
                             const FullConstantValue &Rhs) {
  if (Lhs.Tag == FullConstantTag::Bottom) {
    return Rhs;
  }
  if (Rhs.Tag == FullConstantTag::Bottom) {
    return Lhs;
  }
  if (Lhs.Tag == FullConstantTag::Const && Rhs.Tag == FullConstantTag::Const &&
      Lhs.ConstValue == Rhs.ConstValue) {
    return Lhs;
  }
  return FullConstantValue::top();
}

FullConstantValue resolveValue(const FullConstantPropagationState &In,
                               const Value *V) {
  if (In.Unreachable) {
    return FullConstantValue::bottom();
  }

  if (auto *CI = dyn_cast<ConstantInt>(V)) {
    return FullConstantValue::constant(CI->getSExtValue());
  }

  auto It = In.Values.find(V);
  if (It != In.Values.end()) {
    return It->second;
  }

  return FullConstantValue::top();
}

FullConstantValue evalBinaryOp(unsigned Opcode, const FullConstantValue &Lhs,
                               const FullConstantValue &Rhs) {
  if (Lhs.Tag == FullConstantTag::Bottom ||
      Rhs.Tag == FullConstantTag::Bottom) {
    return FullConstantValue::bottom();
  }
  if (Lhs.Tag != FullConstantTag::Const || Rhs.Tag != FullConstantTag::Const) {
    return FullConstantValue::top();
  }

  auto LV = Lhs.ConstValue;
  auto RV = Rhs.ConstValue;
  switch (Opcode) {
  case Instruction::Add:
    return FullConstantValue::constant(LV + RV);
  case Instruction::Sub:
    return FullConstantValue::constant(LV - RV);
  case Instruction::Mul:
    return FullConstantValue::constant(LV * RV);
  case Instruction::SDiv:
    if (RV == 0) {
      return FullConstantValue::top();
    }
    return FullConstantValue::constant(LV / RV);
  case Instruction::UDiv:
    if (RV == 0) {
      return FullConstantValue::top();
    }
    return FullConstantValue::constant(static_cast<uint64_t>(LV) /
                                       static_cast<uint64_t>(RV));
  case Instruction::SRem:
    if (RV == 0) {
      return FullConstantValue::top();
    }
    return FullConstantValue::constant(LV % RV);
  case Instruction::URem:
    if (RV == 0) {
      return FullConstantValue::top();
    }
    return FullConstantValue::constant(static_cast<uint64_t>(LV) %
                                       static_cast<uint64_t>(RV));
  default:
    return FullConstantValue::top();
  }
}

class IntraMonoFullConstantPropagation
    : public IntraMonoProblem<FullConstantPropagationAnalysisTypes> {
public:
  explicit IntraMonoFullConstantPropagation(Function *F,
                                            lotus::AliasAnalysisWrapper *AA)
      : IntraMonoProblem<FullConstantPropagationAnalysisTypes>(
            std::vector<Function *>{F}, AA),
        AA(AA) {}

  mono_container_t normalFlow(Instruction *Inst,
                              const mono_container_t &In) override {
    if (In.Unreachable) {
      return In;
    }

    mono_container_t Out = In;

    if (const auto *Alloca = dyn_cast<AllocaInst>(Inst)) {
      if (Alloca->getAllocatedType()->isIntegerTy()) {
        Out.Values[Alloca] = FullConstantValue::top();
      }
      return Out;
    }

    if (const auto *Store = dyn_cast<StoreInst>(Inst)) {
      if (!Store->getValueOperand()->getType()->isIntegerTy()) {
        return Out;
      }
      auto *Ptr = Store->getPointerOperand();
      Out.Values[Ptr] = resolveValue(In, Store->getValueOperand());
      writeAliases(Ptr, Out.Values[Ptr], Out);
      return Out;
    }

    if (const auto *Load = dyn_cast<LoadInst>(Inst)) {
      auto It = In.Values.find(Load->getPointerOperand());
      if (It != In.Values.end()) {
        Out.Values[Load] = It->second;
      } else {
        Out.Values[Load] = resolveAliasValue(Load->getPointerOperand(), In);
      }
      return Out;
    }

    if (const auto *Op = dyn_cast<BinaryOperator>(Inst)) {
      auto L = resolveValue(In, Op->getOperand(0));
      auto R = resolveValue(In, Op->getOperand(1));
      Out.Values[Op] = evalBinaryOp(Op->getOpcode(), L, R);
      return Out;
    }

    return Out;
  }

  std::unordered_map<Instruction *, mono_container_t> initialSeeds() override {
    std::unordered_map<Instruction *, mono_container_t> Seeds;
    Function *F = this->getEntryPoints().empty()
                      ? nullptr
                      : this->getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }
    mono_container_t Init;
    Init.Unreachable = false;
    Seeds[&F->getEntryBlock().front()] = Init;
    return Seeds;
  }

private:
  lotus::AliasAnalysisWrapper *AA;

  void collectAliasedPointers(const Value *Ptr, const mono_container_t &State,
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
    for (const auto &Entry : State.Values) {
      Add(Entry.first);
    }
  }

  FullConstantValue resolveAliasValue(const Value *Ptr,
                                      const mono_container_t &In) const {
    if (AA == nullptr || !AA->isInitialized() || Ptr == nullptr ||
        !Ptr->getType()->isPointerTy()) {
      return FullConstantValue::top();
    }

    bool Found = false;
    FullConstantValue Val = FullConstantValue::top();
    for (const auto &Entry : In.Values) {
      auto *Alias = Entry.first;
      if (Alias == nullptr || !Alias->getType()->isPointerTy()) {
        continue;
      }
      if (AA->query(Ptr, Alias) == AliasResult::NoAlias) {
        continue;
      }
      auto It = In.Values.find(Alias);
      if (It == In.Values.end()) {
        continue;
      }
      if (!Found) {
        Val = It->second;
        Found = true;
      } else if (!(Val == It->second)) {
        return FullConstantValue::top();
      }
    }
    return Found ? Val : FullConstantValue::top();
  }

  void writeAliases(const Value *Ptr, const FullConstantValue &Val,
                    mono_container_t &Out) const {
    std::vector<const Value *> MustAliases;
    std::vector<const Value *> MayAliases;
    collectAliasedPointers(Ptr, Out, MustAliases, MayAliases);

    for (const auto *Alias : MustAliases) {
      if (Alias != nullptr) {
        Out.Values[Alias] = Val;
      }
    }

    for (const auto *Alias : MayAliases) {
      if (Alias == nullptr) {
        continue;
      }
      auto It = Out.Values.find(Alias);
      auto OldVal =
          It != Out.Values.end() ? It->second : FullConstantValue::top();
      Out.Values[Alias] = joinValues(OldVal, Val);
    }
  }
};

} // namespace

std::unordered_map<Instruction *, FullConstantPropagationState>
runIntraMonoFullConstantPropagation(Function *F) {
  if (F == nullptr || F->isDeclaration()) {
    return {};
  }

  auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
      *F->getParent(),
      lotus::AAConfig(lotus::AAConfig::Implementation::DyckAA,
                      lotus::AAConfig::ContextSensitivity::None, 0, true,
                      lotus::AAConfig::Solver::Default));
  IntraMonoFullConstantPropagation Problem(F, AA.get());
  IntraMonoSolver<FullConstantPropagationAnalysisTypes> Solver(Problem);
  Solver.solve();
  return Solver.getInResults();
}

} // namespace mono
