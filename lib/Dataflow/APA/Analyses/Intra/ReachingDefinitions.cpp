#include "Dataflow/APA/Analyses/Intra/ReachingDefinitions.h"

#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/IR/Instructions.h"

namespace elimination {
namespace {

class ElimReachingDefinitionsProblem
    : public LLVMIntraEliminationProblem<ReachingDefinitionsFact> {
public:
  explicit ElimReachingDefinitionsProblem(llvm::Function *F,
                                          llvm::AAResults *AA = nullptr,
                                          llvm::MemorySSA *MSSA = nullptr)
      : LLVMIntraEliminationProblem<ReachingDefinitionsFact>(F), AA(AA),
        MSSA(MSSA) {}

  ReachingDefinitionsFact
  applyTransfer(const transfer_t &T,
                const ReachingDefinitionsFact &In) const override {
    auto *Inst = T;
    ReachingDefinitionsFact Out = In;
    if (Inst == nullptr) {
      return Out;
    }

    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Inst)) {
      if (MSSA != nullptr) {
        killStoresWithMemorySSA(Store, Out);
      } else if (AA == nullptr) {
        killAllStores(Out);
      } else {
        killAliasedStores(Store, Out);
      }
      Out.insert(Store);
      return Out;
    }

    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      if (Call->mayWriteToMemory()) {
        if (MSSA != nullptr) {
          killStoresWithMemorySSA(Call, Out);
        } else if (AA == nullptr) {
          killAllStores(Out);
        } else {
          killStoresModdedByCall(Call, Out);
        }
      }
    }

    if (!Inst->getType()->isVoidTy()) {
      Out.insert(Inst);
    }

    return Out;
  }

  ReachingDefinitionsFact
  meet(const ReachingDefinitionsFact &Lhs,
       const ReachingDefinitionsFact &Rhs) const override {
    return ReachingDefinitionsDomain::meet(Lhs, Rhs);
  }

  bool equal_to(const ReachingDefinitionsFact &Lhs,
                const ReachingDefinitionsFact &Rhs) const override {
    return ReachingDefinitionsDomain::equal(Lhs, Rhs);
  }

  ReachingDefinitionsFact meetIdentity() const override {
    return ReachingDefinitionsDomain::meetIdentity();
  }

  ReachingDefinitionsFact initialFact() const override {
    ReachingDefinitionsFact Out;
    auto *F = this->entry() != nullptr ? this->entry()->getFunction() : nullptr;
    if (F == nullptr) {
      return Out;
    }
    for (auto &Arg : F->args()) {
      Out.insert(&Arg);
    }
    return Out;
  }

private:
  llvm::AAResults *AA = nullptr;
  llvm::MemorySSA *MSSA = nullptr;

  static void killAllStores(ReachingDefinitionsFact &Out) {
    for (auto It = Out.begin(); It != Out.end();) {
      if (llvm::isa<llvm::StoreInst>(*It)) {
        It = Out.erase(It);
      } else {
        ++It;
      }
    }
  }

  void killAliasedStores(const llvm::StoreInst *Store,
                         ReachingDefinitionsFact &Out) const {
    if (AA == nullptr || Store == nullptr) {
      killAllStores(Out);
      return;
    }
    auto StoreLoc = llvm::MemoryLocation::get(Store);
    for (auto It = Out.begin(); It != Out.end();) {
      auto *Def = llvm::dyn_cast<llvm::StoreInst>(*It);
      if (Def == nullptr) {
        ++It;
        continue;
      }
      auto DefLoc = llvm::MemoryLocation::get(Def);
      if (AA->alias(StoreLoc, DefLoc) != llvm::AliasResult::NoAlias) {
        It = Out.erase(It);
        continue;
      }
      ++It;
    }
  }

  void killStoresModdedByCall(const llvm::CallBase *Call,
                              ReachingDefinitionsFact &Out) const {
    if (AA == nullptr || Call == nullptr) {
      killAllStores(Out);
      return;
    }
    for (auto It = Out.begin(); It != Out.end();) {
      auto *Def = llvm::dyn_cast<llvm::StoreInst>(*It);
      if (Def == nullptr) {
        ++It;
        continue;
      }
      auto DefLoc = llvm::MemoryLocation::get(Def);
      auto Info = AA->getModRefInfo(Call, DefLoc);
      if (llvm::isModSet(Info)) {
        It = Out.erase(It);
        continue;
      }
      ++It;
    }
  }

  void killStoresWithMemorySSA(const llvm::Instruction *Inst,
                               ReachingDefinitionsFact &Out) const {
    if (MSSA == nullptr || Inst == nullptr) {
      killAllStores(Out);
      return;
    }
    auto *MA = MSSA->getMemoryAccess(Inst);
    if (MA == nullptr) {
      killAllStores(Out);
      return;
    }
    auto *Walker = MSSA->getWalker();
    for (auto It = Out.begin(); It != Out.end();) {
      auto *Def = llvm::dyn_cast<llvm::StoreInst>(*It);
      if (Def == nullptr) {
        ++It;
        continue;
      }
      auto DefLoc = llvm::MemoryLocation::get(Def);
      auto *Clobber = Walker->getClobberingMemoryAccess(MA, DefLoc);
      if (Clobber == MA) {
        It = Out.erase(It);
        continue;
      }
      ++It;
    }
  }
};

} // namespace

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F, EliminationOptions Opts) {
  return runIntraElimReachingDefinitions(F, nullptr, Opts);
}

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F, llvm::AAResults *AA,
                                EliminationOptions Opts) {
  return runIntraElimReachingDefinitions(F, AA, nullptr, Opts);
}

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F, llvm::AAResults *AA,
                                llvm::MemorySSA *MSSA,
                                EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return ReachingDefinitionsResult{};
  }

  ElimReachingDefinitionsProblem Problem(F, AA, MSSA);
  IntraEliminationSolver<LLVMAnalysisTypes<ReachingDefinitionsFact>> Solver(
      Problem, Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  Out.setSolveMetadata(Status, Solver.getDiagnostics());
  return Out;
}

} // namespace elimination
