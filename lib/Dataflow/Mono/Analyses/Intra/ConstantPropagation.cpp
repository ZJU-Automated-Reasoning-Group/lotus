/*
 *
 * Author: rainoftime
 */
#include "Dataflow/Mono/Analyses/Intra/ConstantPropagation.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <memory>

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

bool isTop(const ConstantPropagationValue &V) {
  return V.Tag == ConstantPropagationTag::Top;
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
                                      const llvm::Value *V) {
  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V)) {
    return makeConst(CI->getSExtValue());
  }

  auto It = In.find(V);
  if (It != In.end()) {
    return It->second;
  }

  return makeTop();
}

ConstantPropagationValue evalBinaryOp(unsigned Opcode,
                                      const ConstantPropagationValue &Lhs,
                                      const ConstantPropagationValue &Rhs) {
  // Distinguish Top (unknown) from Bottom (unreachable/error).
  //   - Bottom propagates as Bottom (unreachable code stays unreachable).
  //   - Top means the value is unknown; the result is also unknown (Top),
  //     NOT Bottom.  Returning Bottom for Top inputs was unsound: it caused
  //     reachable computations to be treated as unreachable, producing false
  //     negatives in constant detection.
  if (isBottom(Lhs) || isBottom(Rhs)) {
    return makeBottom();
  }
  if (isTop(Lhs) || isTop(Rhs)) {
    return makeTop();
  }

  if (!isConst(Lhs) || !isConst(Rhs)) {
    return makeTop();
  }

  auto LV = Lhs.ConstValue;
  auto RV = Rhs.ConstValue;
  switch (Opcode) {
  case llvm::Instruction::Add:
    return makeConst(LV + RV);
  case llvm::Instruction::Sub:
    return makeConst(LV - RV);
  case llvm::Instruction::Mul:
    return makeConst(LV * RV);
  case llvm::Instruction::SDiv:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(LV / RV);
  case llvm::Instruction::UDiv:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(static_cast<uint64_t>(LV) / static_cast<uint64_t>(RV));
  case llvm::Instruction::SRem:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(LV % RV);
  case llvm::Instruction::URem:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(static_cast<uint64_t>(LV) % static_cast<uint64_t>(RV));
  default:
    // Unknown opcode: result is unknown (Top), not unreachable (Bottom).
    return makeTop();
  }
}

class IntraMonoConstantPropagation
    : public IntraMonoProblem<ConstantPropagationAnalysisTypes> {
public:
  explicit IntraMonoConstantPropagation(llvm::Function *F,
                                        lotus::AliasAnalysisWrapper *AA)
      : IntraMonoProblem<ConstantPropagationAnalysisTypes>({F}, AA), AA(AA) {}

  ConstantPropagationMap normalFlow(llvm::Instruction *Inst,
                                    const ConstantPropagationMap &In) override {
    if (getAbstractDomain().isBottom(In))
      return In;
    ConstantPropagationMap Out = In;

    if (const auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Inst)) {
      if (Alloca->getAllocatedType()->isIntegerTy()) {
        Out[Alloca] = makeTop();
      }
      return Out;
    }

    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(Inst)) {
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

    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
      auto It = In.find(Load->getPointerOperand());
      if (It != In.end()) {
        Out[Load] = It->second;
      } else {
        Out[Load] = resolveAliasValue(Load->getPointerOperand(), In);
      }
      return Out;
    }

    if (const auto *Op = llvm::dyn_cast<llvm::BinaryOperator>(Inst)) {
      auto Lhs = resolveValue(In, Op->getOperand(0));
      auto Rhs = resolveValue(In, Op->getOperand(1));
      Out[Op] = evalBinaryOp(Op->getOpcode(), Lhs, Rhs);
      return Out;
    }

    return Out;
  }

  std::unordered_map<llvm::Instruction *, ConstantPropagationMap>
  initialSeeds() override {
    std::unordered_map<llvm::Instruction *, ConstantPropagationMap> Seeds;
    auto *F = this->getEntryPoints().empty() ? nullptr
                                             : this->getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }
    Seeds[&F->getEntryBlock().front()] = ConstantPropagationMap{};
    return Seeds;
  }

  void printContainer(llvm::raw_ostream &OS,
                      const ConstantPropagationMap &Map) const override {
    if (Map.empty()) {
      OS << "EMPTY";
      return;
    }
    for (const auto &Entry : Map) {
      OS << "  ";
      Entry.first->print(OS);
      OS << " => ";
      switch (Entry.second.Tag) {
      case ConstantPropagationTag::Top:
        OS << "TOP";
        break;
      case ConstantPropagationTag::Bottom:
        OS << "BOTTOM";
        break;
      case ConstantPropagationTag::Const:
        OS << Entry.second.ConstValue;
        break;
      }
      OS << "\n";
    }
  }

private:
  lotus::AliasAnalysisWrapper *AA;

  // weakJoin is used for may-aliased pointers: if the old and new values
  // agree, keep the value; otherwise the result is unknown (Top), not
  // unreachable (Bottom).  Returning Bottom here was unsound: it caused
  // a may-aliased store to mark the location as unreachable.
  static ConstantPropagationValue
  weakJoin(const ConstantPropagationValue &OldVal,
           const ConstantPropagationValue &NewVal) {
    if (equalValue(OldVal, NewVal)) {
      return OldVal;
    }
    return makeTop(); // conflicting values → unknown
  }

  void
  collectAliasedPointers(const llvm::Value *Ptr,
                         const ConstantPropagationMap &State,
                         std::vector<const llvm::Value *> &MustAliases,
                         std::vector<const llvm::Value *> &MayAliases) const {
    if (Ptr == nullptr || !Ptr->getType()->isPointerTy()) {
      return;
    }

    const bool HaveAA = AA != nullptr && AA->isInitialized();
    const llvm::Value *PtrBase = Ptr->stripPointerCasts();
    auto Add = [&](const llvm::Value *Candidate) {
      if (Candidate == nullptr || !Candidate->getType()->isPointerTy()) {
        return;
      }
      const llvm::Value *CandidateBase = Candidate->stripPointerCasts();
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
      if (Res == llvm::AliasResult::MustAlias) {
        MustAliases.push_back(Candidate);
      } else if (Res != llvm::AliasResult::NoAlias) {
        MayAliases.push_back(Candidate);
      }
    };

    Add(Ptr);
    for (const auto &Entry : State) {
      Add(Entry.first);
    }
  }

  ConstantPropagationValue
  resolveAliasValue(const llvm::Value *Ptr,
                    const ConstantPropagationMap &In) const {
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
      if (AA->query(Ptr, Alias) == llvm::AliasResult::NoAlias) {
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
        // Multiple aliases disagree on the value → unknown (Top), not
        // unreachable (Bottom).  Returning Bottom was unsound.
        return makeTop();
      }
    }
    return Found ? Val : makeTop();
  }

  void writeAliases(const llvm::Value *Ptr, const ConstantPropagationValue &Val,
                    ConstantPropagationMap &Out) const {
    std::vector<const llvm::Value *> MustAliases;
    std::vector<const llvm::Value *> MayAliases;
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

std::unordered_map<llvm::Instruction *, ConstantPropagationMap>
runIntraMonoConstantPropagation(llvm::Function *F,
                                const DebugConfig &DebugCfg) {
  if (F == nullptr || F->isDeclaration()) {
    return {};
  }

  auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
      *F->getParent(),
      lotus::AAConfig(lotus::AAConfig::Implementation::DyckAA,
                      lotus::AAConfig::ContextSensitivity::None, 0, true,
                      lotus::AAConfig::Solver::Default));
  IntraMonoConstantPropagation Problem(F, AA.get());
  ConstantPropagationSolver Solver(Problem);
  Solver.setDebugConfig(DebugCfg);
  Solver.solve();
  return Solver.getInResults();
}

} // namespace mono
