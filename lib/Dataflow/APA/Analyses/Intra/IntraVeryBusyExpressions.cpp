#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"

#include "Dataflow/APA/Clients/LLVM/Intra/VeryBusyExpressions.h"
#include "Dataflow/APA/Core/Problem.h"
#include "Dataflow/APA/Engines/Solver.h"
#include "Dataflow/ControlFlow/IntraCFG.h"

#include <algorithm>
#include <iterator>
#include <unordered_set>

namespace elimination {
namespace {

bool isCandidateExpr(const llvm::Instruction *Inst,
                     const llvm::DominatorTree *DT,
                     const llvm::TargetLibraryInfo *TLI) {
  if (Inst == nullptr || Inst->getType()->isVoidTy()) {
    return false;
  }
  if (llvm::isa<llvm::PHINode>(Inst) || llvm::isa<llvm::AllocaInst>(Inst) ||
      Inst->isTerminator()) {
    return false;
  }
  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
    return !Load->isVolatile();
  }
  if (Inst->mayHaveSideEffects()) {
    return false;
  }
  return llvm::isSafeToSpeculativelyExecute(Inst, Inst, DT, TLI);
}

ExpressionKey makeKeyWithMSSA(const llvm::Instruction *Inst,
                              llvm::MemorySSA *MSSA) {
  auto Key = makeExpressionKey(Inst);
  if (MSSA == nullptr || Inst == nullptr) {
    return Key;
  }
  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
    auto *Walker = MSSA->getWalker();
    auto *Clobber = Walker->getClobberingMemoryAccess(Load);
    Key.MemoryAccess = Clobber;
  }
  return Key;
}

struct VeryBusyDomain {
  using n_t = llvm::Instruction *;
  using fact_t = VeryBusyExpressionsFact;
  using transfer_t = llvm::Instruction *;
};

class ReverseVeryBusyProblem : public IntraEliminationProblem<VeryBusyDomain> {
public:
  explicit ReverseVeryBusyProblem(llvm::Function *F, llvm::Instruction *Entry,
                                  llvm::AAResults *AA, llvm::DominatorTree *DT,
                                  llvm::TargetLibraryInfo *TLI,
                                  llvm::MemorySSA *MSSA)
      : F(F), Entry(Entry), AA(AA), DT(DT), TLI(TLI), MSSA(MSSA) {
    buildUniverse(F);
  }

  std::vector<n_t> nodes() const override {
    ensurePrepared();
    return Nodes;
  }

  n_t entry() const override { return Entry; }

  std::vector<n_t> succs(n_t Node) const override {
    return CFG.getSuccsOf(Node, dataflow::controlflow::FlowDirection::Backward);
  }

  transfer_t edgeTransfer(n_t /*Src*/, n_t Dst) const override { return Dst; }

  fact_t applyTransfer(const transfer_t &T, const fact_t &In) const override {
    auto *Inst = T;
    fact_t Out = In;
    if (Inst == nullptr) {
      return Out;
    }

    if (killsMemory(Inst)) {
      if (MSSA != nullptr) {
        killLoadsWithMemorySSA(Inst, Out);
      } else if (AA == nullptr) {
        killAllLoads(Out);
      } else {
        killAliasedLoads(Inst, Out);
      }
    }

    if (isCandidateExpr(Inst, DT, TLI)) {
      Out.insert(makeKeyWithMSSA(Inst, MSSA));
    }

    return Out;
  }

  fact_t meet(const fact_t &Lhs, const fact_t &Rhs) const override {
    fact_t Out;
    std::set_intersection(Lhs.begin(), Lhs.end(), Rhs.begin(), Rhs.end(),
                          std::inserter(Out, Out.begin()));
    return Out;
  }

  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return Lhs == Rhs;
  }

  fact_t meetIdentity() const override { return AllExprs; }

  fact_t initialFact() const override { return fact_t{}; }

  const fact_t &allExprs() const { return AllExprs; }

private:
  llvm::Function *F = nullptr;
  llvm::Instruction *Entry = nullptr;
  llvm::AAResults *AA = nullptr;
  llvm::DominatorTree *DT = nullptr;
  llvm::TargetLibraryInfo *TLI = nullptr;
  llvm::MemorySSA *MSSA = nullptr;
  dataflow::controlflow::LLVMIntraCFG CFG;
  mutable bool Prepared = false;
  mutable std::vector<n_t> Nodes;
  fact_t AllExprs;
  std::set<ExpressionKey> LoadExprs;

  void buildUniverse(llvm::Function *Func) {
    AllExprs.clear();
    LoadExprs.clear();
    if (Func == nullptr || Func->isDeclaration()) {
      return;
    }
    for (auto &BB : *Func) {
      for (auto &I : BB) {
        if (!isCandidateExpr(&I, DT, TLI)) {
          continue;
        }
        auto Key = makeKeyWithMSSA(&I, MSSA);
        AllExprs.insert(Key);
        if (llvm::isa<llvm::LoadInst>(&I)) {
          LoadExprs.insert(Key);
        }
      }
    }
  }

  void ensurePrepared() const {
    if (Prepared) {
      return;
    }
    Prepared = true;
    Nodes.clear();
    if (F == nullptr || F->isDeclaration() || Entry == nullptr) {
      return;
    }

    std::unordered_set<n_t> Reach;
    std::vector<n_t> Stack;
    Stack.push_back(Entry);
    Reach.insert(Entry);
    while (!Stack.empty()) {
      auto *Cur = Stack.back();
      Stack.pop_back();
      for (auto *Succ : CFG.getSuccsOf(
               Cur, dataflow::controlflow::FlowDirection::Backward)) {
        if (Reach.insert(Succ).second) {
          Stack.push_back(Succ);
        }
      }
    }

    for (auto *N : CFG.getAllInstructionsOf(F)) {
      if (Reach.count(N)) {
        Nodes.push_back(N);
      }
    }
  }

  static bool killsMemory(const llvm::Instruction *Inst) {
    if (Inst == nullptr) {
      return false;
    }
    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
      return Load->isVolatile();
    }
    return Inst->mayWriteToMemory();
  }

  void killAllLoads(fact_t &Out) const {
    for (auto It = Out.begin(); It != Out.end();) {
      if (LoadExprs.count(*It)) {
        It = Out.erase(It);
      } else {
        ++It;
      }
    }
  }

  void killAliasedLoads(const llvm::Instruction *Inst, fact_t &Out) const {
    if (AA == nullptr || Inst == nullptr) {
      killAllLoads(Out);
      return;
    }

    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Inst)) {
      auto StoreLoc = llvm::MemoryLocation::get(Store);
      for (auto It = Out.begin(); It != Out.end();) {
        const auto &Key = *It;
        if (!isLoadKey(Key)) {
          ++It;
          continue;
        }
        auto *Ptr = getLoadPointerOperand(Key);
        if (Ptr == nullptr) {
          It = Out.erase(It);
          continue;
        }
        auto LoadLoc = llvm::MemoryLocation(
            Ptr, llvm::LocationSize::beforeOrAfterPointer(), Key.AATags);
        if (AA->alias(StoreLoc, LoadLoc) != llvm::AliasResult::NoAlias) {
          It = Out.erase(It);
          continue;
        }
        ++It;
      }
      return;
    }

    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      if (!Call->mayWriteToMemory()) {
        return;
      }
      for (auto It = Out.begin(); It != Out.end();) {
        const auto &Key = *It;
        if (!isLoadKey(Key)) {
          ++It;
          continue;
        }
        auto *Ptr = getLoadPointerOperand(Key);
        if (Ptr == nullptr) {
          It = Out.erase(It);
          continue;
        }
        auto LoadLoc = llvm::MemoryLocation(
            Ptr, llvm::LocationSize::beforeOrAfterPointer(), Key.AATags);
        auto Info = AA->getModRefInfo(Call, LoadLoc);
        if (llvm::isModSet(Info)) {
          It = Out.erase(It);
          continue;
        }
        ++It;
      }
      return;
    }

    killAllLoads(Out);
  }

  void killLoadsWithMemorySSA(const llvm::Instruction *Inst,
                              fact_t &Out) const {
    if (MSSA == nullptr || Inst == nullptr) {
      killAllLoads(Out);
      return;
    }
    auto *MA = MSSA->getMemoryAccess(Inst);
    if (MA == nullptr) {
      killAllLoads(Out);
      return;
    }
    auto *Walker = MSSA->getWalker();
    for (auto It = Out.begin(); It != Out.end();) {
      const auto &Key = *It;
      if (!isLoadKey(Key)) {
        ++It;
        continue;
      }
      auto *Ptr = getLoadPointerOperand(Key);
      if (Ptr == nullptr) {
        It = Out.erase(It);
        continue;
      }
      llvm::MemoryLocation LoadLoc(
          Ptr, llvm::LocationSize::beforeOrAfterPointer(), Key.AATags);
      auto *Clobber = Walker->getClobberingMemoryAccess(MA, LoadLoc);
      if (Clobber == MA) {
        It = Out.erase(It);
        continue;
      }
      ++It;
    }
  }
};

// Return the set of "real" exit instructions for backward analysis.
// We only include ReturnInst terminators; UnreachableInst and other
// no-successor terminators are excluded because propagating facts backward
// from unreachable code is unsound.
std::vector<llvm::Instruction *> getExitInstructions(llvm::Function *F) {
  std::vector<llvm::Instruction *> Exits;
  if (F == nullptr || F->isDeclaration()) {
    return Exits;
  }
  for (auto &BB : *F) {
    if (auto *Term = BB.getTerminator()) {
      if (llvm::isa<llvm::ReturnInst>(Term)) {
        Exits.push_back(Term);
      }
    }
  }
  return Exits;
}

} // namespace

VeryBusyExpressionsResult
runIntraElimVeryBusyExpressions(llvm::Function *F, EliminationOptions Opts) {
  return runIntraElimVeryBusyExpressions(F, nullptr, Opts);
}

VeryBusyExpressionsResult
runIntraElimVeryBusyExpressions(llvm::Function *F, llvm::AAResults *AA,
                                EliminationOptions Opts) {
  return runIntraElimVeryBusyExpressions(F, AA, nullptr, nullptr, Opts);
}

VeryBusyExpressionsResult runIntraElimVeryBusyExpressions(
    llvm::Function *F, llvm::AAResults *AA, llvm::DominatorTree *DT,
    llvm::TargetLibraryInfo *TLI, EliminationOptions Opts) {
  return runIntraElimVeryBusyExpressions(F, AA, DT, TLI, nullptr, Opts);
}

VeryBusyExpressionsResult runIntraElimVeryBusyExpressions(
    llvm::Function *F, llvm::AAResults *AA, llvm::DominatorTree *DT,
    llvm::TargetLibraryInfo *TLI, llvm::MemorySSA *MSSA,
    EliminationOptions Opts) {
  VeryBusyExpressionsResult Combined;
  if (F == nullptr || F->isDeclaration()) {
    return Combined;
  }

  auto Exits = getExitInstructions(F);
  if (Exits.empty()) {
    return Combined;
  }

  auto OverallStatus = SolveStatus::Ok;
  SolveDiagnostics OverallDiag;
  OverallDiag.requested_method = Opts.Method;
  OverallDiag.executed_method = Opts.Method;
  bool Initialized = false;

  // Multi-exit handling: solve one reverse problem rooted at each return and
  // merge with intersection (must semantics: busy on all return paths).
  for (auto *Exit : Exits) {
    ReverseVeryBusyProblem Problem(F, Exit, AA, DT, TLI, MSSA);
    IntraEliminationSolver<VeryBusyDomain> Solver(Problem, Opts);
    auto Status = Solver.solve();
    const auto &Diag = Solver.getDiagnostics();
    OverallDiag.used_adt = OverallDiag.used_adt || Diag.used_adt;
    OverallDiag.star_iterations_total += Diag.star_iterations_total;
    OverallDiag.max_star_hit = OverallDiag.max_star_hit || Diag.max_star_hit;
    if (Diag.fallback_reason != FallbackReason::None) {
      OverallDiag.fallback_reason = Diag.fallback_reason;
    }
    if (Status == SolveStatus::NonConvergentStar) {
      OverallStatus = SolveStatus::NonConvergentStar;
    } else if (Status == SolveStatus::InvalidProblem &&
               OverallStatus != SolveStatus::NonConvergentStar) {
      OverallStatus = SolveStatus::InvalidProblem;
    } else if (Status == SolveStatus::FallbackToState &&
               OverallStatus == SolveStatus::Ok) {
      OverallStatus = SolveStatus::FallbackToState;
    }
    auto Res = Solver.getResults();

    const auto *ExitFacts = Res.tryIN(Exit);
    if (ExitFacts != nullptr) {
      Res.IN(Exit) = Problem.applyTransfer(Exit, *ExitFacts);
    }

    for (auto &BB : *F) {
      for (auto &I : BB) {
        auto *Inst = &I;
        auto &Out = Combined.IN(Inst);
        const auto *InFacts = Res.tryIN(Inst);
        if (InFacts == nullptr) {
          continue;
        }
        // Seed the first solve with the universe to implement intersection.
        if (!Initialized) {
          Out = Problem.allExprs();
        }
        VeryBusyExpressionsFact Intersected;
        std::set_intersection(Out.begin(), Out.end(), InFacts->begin(),
                              InFacts->end(),
                              std::inserter(Intersected, Intersected.begin()));
        Out.swap(Intersected);
      }
    }
    Initialized = true;
  }

  Combined.setSolveMetadata(OverallStatus, OverallDiag);
  return Combined;
}

} // namespace elimination
