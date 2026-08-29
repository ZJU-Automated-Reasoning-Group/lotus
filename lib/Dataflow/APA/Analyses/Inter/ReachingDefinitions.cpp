#include "Dataflow/APA/Analyses/Inter/ReachingDefinitions.h"

#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/IR/Instructions.h"

#include "Dataflow/APA/Analyses/Inter/FlowHelpers.h"
#include "Dataflow/APA/LLVM/InterProblem.h"
#include "Dataflow/APA/Solver/ForwardInterSummarySolver.h"

#include <memory>

namespace elimination {
namespace {

struct InterReachingDefinitionsAnalysisTypes {
  using n_t = llvm::Instruction *;
  using fact_t = ReachingDefinitionsFact;
  using transfer_t = llvm::Instruction *;
  using f_t = llvm::Function *;
  using i_t = dataflow::controlflow::InterCFG;
};

class InterElimReachingDefinitionsProblem
    : public LLVMInterEliminationProblem<InterReachingDefinitionsAnalysisTypes> {
public:
  explicit InterElimReachingDefinitionsProblem(
      llvm::Function *Entry, llvm::AAResults *AA = nullptr,
      llvm::MemorySSA *MSSA = nullptr,
      const dataflow::controlflow::InterCFG *ICF = nullptr)
      : LLVMInterEliminationProblem<InterReachingDefinitionsAnalysisTypes>(
            std::vector<llvm::Function *>{Entry}, ICF),
        AA(AA), MSSA(MSSA) {}

  fact_t normalFlow(n_t Inst, const fact_t &In) override {
    fact_t Out = In;
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

  fact_t merge(const fact_t &Lhs, const fact_t &Rhs) const override {
    return ReachingDefinitionsDomain::meet(Lhs, Rhs);
  }

  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return ReachingDefinitionsDomain::equal(Lhs, Rhs);
  }

  fact_t allTop() const override {
    return ReachingDefinitionsDomain::meetIdentity();
  }

  fact_t callFlow(n_t /*CallSite*/, f_t Callee, const fact_t &In) override {
    fact_t Out;
    if (Callee == nullptr) {
      return Out;
    }

    for (auto &Arg : Callee->args()) {
      Out.insert(&Arg);
    }
    llvm_inter::copyGlobalValueFacts(In, Out);
    llvm_inter::copyStoreFacts(In, Out);
    return Out;
  }

  fact_t returnFlow(n_t CallSite, f_t Callee, n_t ExitStmt, n_t /*RetSite*/,
                    const fact_t &In) override {
    fact_t Out;
    llvm_inter::copyGlobalValueFacts(In, Out);
    llvm_inter::copyStoreFacts(In, Out);

    auto *Ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(ExitStmt);
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (!llvm_inter::hasConcreteReturnValue(Call, Ret)) {
      return Out;
    }

    Out.insert(CallSite);

    if (Callee != nullptr) {
      llvm_inter::forEachActualFormalPair(
          Call, Callee,
          [&](llvm::Value *Actual, llvm::Argument *Formal, unsigned /*Index*/) {
            if (!Formal->getType()->isPointerTy()) {
              return;
            }
            for (auto *Def : In) {
              auto *Store = llvm::dyn_cast<llvm::StoreInst>(Def);
              if (Store == nullptr) {
                continue;
              }
              if (AA == nullptr) {
                Out.insert(Store);
              } else if (AA->alias(
                             llvm::MemoryLocation::get(Store),
                             llvm::MemoryLocation(
                                 Actual,
                                 llvm::LocationSize::beforeOrAfterPointer(),
                                 llvm::AAMDNodes())) !=
                         llvm::AliasResult::NoAlias) {
                Out.insert(Store);
              }
            }
          });
    }
    return Out;
  }

  fact_t callToRetFlow(n_t CallSite, n_t /*RetSite*/,
                       const std::vector<f_t> & /*Callees*/,
                       const fact_t &In) override {
    return normalFlow(CallSite, In);
  }

  std::unordered_map<n_t, fact_t> initialSeeds() override {
    std::unordered_map<n_t, fact_t> Seeds;
    auto *Entry = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (Entry == nullptr || Entry->empty()) {
      return Seeds;
    }

    fact_t Init;
    for (auto &Arg : Entry->args()) {
      Init.insert(&Arg);
    }
    Seeds[&*Entry->getEntryBlock().begin()] = std::move(Init);
    return Seeds;
  }

private:
  llvm::AAResults *AA = nullptr;
  llvm::MemorySSA *MSSA = nullptr;

  static void killAllStores(fact_t &Out) {
    for (auto It = Out.begin(); It != Out.end();) {
      if (llvm::isa<llvm::StoreInst>(*It)) {
        It = Out.erase(It);
      } else {
        ++It;
      }
    }
  }

  void killAliasedStores(const llvm::StoreInst *Store, fact_t &Out) const {
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

  void killStoresModdedByCall(const llvm::CallBase *Call, fact_t &Out) const {
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
      if (llvm::isModSet(AA->getModRefInfo(Call, DefLoc))) {
        It = Out.erase(It);
        continue;
      }
      ++It;
    }
  }

  void killStoresWithMemorySSA(const llvm::Instruction *Inst,
                               fact_t &Out) const {
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

InterReachingDefinitionsResult
runInterElimReachingDefinitions(llvm::Function *Entry, llvm::AAResults *AA,
                                llvm::MemorySSA *MSSA,
                                const dataflow::controlflow::InterCFG *ICF) {
  InterReachingDefinitionsResult Out;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Out;
  }

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry != nullptr ? Entry->getParent() : nullptr);
    ICF = OwnedICF.get();
  }

  InterElimReachingDefinitionsProblem Problem(Entry, AA, MSSA, ICF);
  InterEliminationSolver<InterReachingDefinitionsAnalysisTypes,
                         kDefaultInterElimReachingDefinitionsCallStringLength>
      Solver(Problem);
  auto Status = Solver.solve();
  if (const auto *Res = Solver.getResults()) {
    Out = *Res;
  }
  Out.setSolveStatus(Status);
  return Out;
}

InterReachingDefinitionsResult runInterSummaryElimReachingDefinitions(
    llvm::Function *Entry, llvm::AAResults *AA, llvm::MemorySSA *MSSA,
    const dataflow::controlflow::InterCFG *ICF,
    PathSummaryEquationOptions Options) {
  InterReachingDefinitionsResult Out;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Out;
  }

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry != nullptr ? Entry->getParent() : nullptr);
    ICF = OwnedICF.get();
  }

  InterElimReachingDefinitionsProblem Problem(Entry, AA, MSSA, ICF);
  ForwardInterSummarySolver<
      InterReachingDefinitionsAnalysisTypes,
      kDefaultInterElimReachingDefinitionsCallStringLength>
      Solver(Problem, Options);
  auto Status = Solver.solve();
  if (const auto *Res = Solver.getResults()) {
    Out = *Res;
  }
  Out.setSolveStatus(Status);
  Out.setSummarySolveDiagnostics(Solver.resultDiagnostics());
  return Out;
}

} // namespace elimination
