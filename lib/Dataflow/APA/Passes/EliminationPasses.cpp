#include "Dataflow/APA/Passes/EliminationPasses.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace elimination {
namespace {

cl::opt<std::string> ElimMethodOpt(
    "elim-method",
    cl::desc("Elimination solver method: state|adt-simple|adt-delayed"),
    cl::init("state"));

cl::opt<bool> ElimReachPrint("elim-reachable-print",
                             cl::desc("Print elimination reachability facts"),
                             cl::init(false));

cl::opt<bool>
    ElimConstPrint("elim-constprop-print",
                   cl::desc("Print elimination constant propagation facts"),
                   cl::init(false));

cl::opt<bool>
    ElimUninitPrint("elim-uninit-print",
                    cl::desc("Print elimination uninitialized variable facts"),
                    cl::init(false));

cl::opt<bool>
    ElimRDPrint("elim-rd-print",
                cl::desc("Print elimination reaching definitions facts"),
                cl::init(false));

cl::opt<bool>
    ElimAvailPrint("elim-available-print",
                   cl::desc("Print elimination available expressions facts"),
                   cl::init(false));

cl::opt<bool> ElimLivePrint("elim-live-print",
                            cl::desc("Print elimination live variables facts"),
                            cl::init(false));

cl::opt<bool>
    ElimBusyPrint("elim-busy-print",
                  cl::desc("Print elimination very busy expressions facts"),
                  cl::init(false));

cl::opt<bool> ElimNonNullPrint("elim-nonnull-print",
                               cl::desc("Print elimination nonnull facts"),
                               cl::init(false));

cl::opt<bool> ElimUseMemorySSA(
    "elim-use-memssa",
    cl::desc("Use MemorySSA to refine elimination memory analyses"),
    cl::init(true));

EliminationOptions getElimOptions() {
  EliminationOptions Opts;
  if (ElimMethodOpt == "adt-simple") {
    Opts.Method = EliminationMethod::ADTSimple;
  } else if (ElimMethodOpt == "adt-delayed") {
    Opts.Method = EliminationMethod::ADTDelayed;
  } else {
    Opts.Method = EliminationMethod::StateElimination;
  }
  return Opts;
}

void printValueShort(raw_ostream &OS, const Value *V) {
  if (V == nullptr) {
    OS << "<null>";
    return;
  }
  V->printAsOperand(OS, false);
}

void printConstMap(raw_ostream &OS, const ConstantPropagationMap &Map) {
  if (Map.empty()) {
    OS << "{}";
    return;
  }
  OS << "{";
  bool First = true;
  for (const auto &Entry : Map) {
    if (!First) {
      OS << ", ";
    }
    First = false;
    printValueShort(OS, Entry.first);
    OS << "=";
    OS << Entry.second;
  }
  OS << "}";
}

void printUninitSet(raw_ostream &OS, const UninitVariablesFact &Set) {
  if (Set.empty()) {
    OS << "{}";
    return;
  }
  OS << "{";
  bool First = true;
  for (const auto *V : Set) {
    if (!First) {
      OS << ", ";
    }
    First = false;
    printValueShort(OS, V);
  }
  OS << "}";
}

void printValueSet(raw_ostream &OS, const std::set<const Value *> &Set) {
  if (Set.empty()) {
    OS << "{}";
    return;
  }
  OS << "{";
  bool First = true;
  for (const auto *V : Set) {
    if (!First) {
      OS << ", ";
    }
    First = false;
    printValueShort(OS, V);
  }
  OS << "}";
}

void printInstSet(raw_ostream &OS, const std::set<const Instruction *> &Set) {
  if (Set.empty()) {
    OS << "{}";
    return;
  }
  OS << "{";
  bool First = true;
  for (const auto *I : Set) {
    if (!First) {
      OS << ", ";
    }
    First = false;
    printValueShort(OS, I);
  }
  OS << "}";
}

void printExprSet(raw_ostream &OS, const std::set<ExpressionKey> &Set) {
  if (Set.empty()) {
    OS << "{}";
    return;
  }
  OS << "{";
  bool First = true;
  for (const auto &Key : Set) {
    if (!First) {
      OS << ", ";
    }
    First = false;
    OS << Instruction::getOpcodeName(Key.Opcode);
    if (Key.Opcode == Instruction::ICmp || Key.Opcode == Instruction::FCmp) {
      OS << ":"
         << CmpInst::getPredicateName(
                static_cast<CmpInst::Predicate>(Key.Predicate));
    }
    OS << "(";
    for (std::size_t I = 0; I < Key.Ops.size(); ++I) {
      if (I != 0) {
        OS << ", ";
      }
      printValueShort(OS, Key.Ops[I]);
    }
    OS << ")";
  }
  OS << "}";
}

const char *toString(EliminationMethod M) {
  switch (M) {
  case EliminationMethod::StateElimination:
    return "state";
  case EliminationMethod::ADTSimple:
    return "adt-simple";
  case EliminationMethod::ADTDelayed:
    return "adt-delayed";
  }
  return "unknown";
}

const char *toString(SolveStatus S) {
  switch (S) {
  case SolveStatus::Ok:
    return "ok";
  case SolveStatus::FallbackToState:
    return "fallback-to-state";
  case SolveStatus::NonConvergentStar:
    return "non-convergent-star";
  case SolveStatus::InvalidProblem:
    return "invalid-problem";
  }
  return "unknown";
}

const char *toString(FallbackReason R) {
  switch (R) {
  case FallbackReason::None:
    return "none";
  case FallbackReason::ADTRejected:
    return "adt-rejected";
  case FallbackReason::InvalidProblem:
    return "invalid-problem";
  }
  return "unknown";
}

template <typename ResultT> void printSolveMetadata(raw_ostream &OS,
                                                    const ResultT &Result) {
  if (!Result.hasSolveMetadata()) {
    return;
  }
  const auto &Diag = Result.solveDiagnostics();
  OS << "  [solver] status=" << toString(Result.solveStatus())
     << ", requested=" << toString(Diag.requested_method)
     << ", executed=" << toString(Diag.executed_method)
     << ", used_adt=" << (Diag.used_adt ? "true" : "false")
     << ", fallback=" << toString(Diag.fallback_reason)
     << ", star_iters=" << Diag.star_iterations_total
     << ", max_star_hit=" << (Diag.max_star_hit ? "true" : "false") << "\n";
}

} // namespace

void ElimReachablePass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

bool ElimReachablePass::runOnFunction(Function &F) {
  Result = runIntraElimReachable(&F, getElimOptions());
  if (ElimReachPrint) {
    errs() << "== Elimination Reachability: " << F.getName() << " ==\n";
    printSolveMetadata(errs(), Result);
    for (auto &BB : F) {
      for (auto &I : BB) {
        errs() << "  ";
        I.print(errs());
        const auto *Fact = Result.tryIN(&I);
        errs() << " :: "
               << ((Fact != nullptr && *Fact) ? "reachable" : "unreachable")
               << "\n";
      }
    }
    errs() << "\n";
  }
  return false;
}

void ElimConstantPropagationPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addRequired<AssumptionCacheTracker>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
}

bool ElimConstantPropagationPass::runOnFunction(Function &F) {
  auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
  auto &AC = getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  Result = runIntraElimConstantPropagation(&F, &AA, &AC, &DT, &TLI,
                                           getElimOptions());
  if (ElimConstPrint) {
    errs() << "== Elimination Constant Propagation: " << F.getName() << " ==\n";
    printSolveMetadata(errs(), Result);
    for (auto &BB : F) {
      for (auto &I : BB) {
        errs() << "  ";
        I.print(errs());
        errs() << " :: ";
        if (const auto *Fact = Result.tryIN(&I)) {
          printConstMap(errs(), *Fact);
        } else {
          const ConstantPropagationMap Empty{};
          printConstMap(errs(), Empty);
        }
        errs() << "\n";
      }
    }
    errs() << "\n";
  }
  return false;
}

void ElimReachingDefinitionsPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addRequired<MemorySSAWrapperPass>();
}

bool ElimReachingDefinitionsPass::runOnFunction(Function &F) {
  auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
  llvm::MemorySSA *MSSA = nullptr;
  if (ElimUseMemorySSA) {
    MSSA = &getAnalysis<MemorySSAWrapperPass>().getMSSA();
  }
  Result = runIntraElimReachingDefinitions(&F, &AA, MSSA, getElimOptions());
  if (ElimRDPrint) {
    errs() << "== Elimination Reaching Definitions: " << F.getName() << " ==\n";
    printSolveMetadata(errs(), Result);
    for (auto &BB : F) {
      for (auto &I : BB) {
        errs() << "  ";
        I.print(errs());
        errs() << " :: ";
        if (const auto *Fact = Result.tryIN(&I)) {
          printValueSet(errs(), *Fact);
        } else {
          const std::set<const Value *> Empty{};
          printValueSet(errs(), Empty);
        }
        errs() << "\n";
      }
    }
    errs() << "\n";
  }
  return false;
}

void ElimAvailableExpressionsPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<MemorySSAWrapperPass>();
}

bool ElimAvailableExpressionsPass::runOnFunction(Function &F) {
  auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  llvm::MemorySSA *MSSA = nullptr;
  if (ElimUseMemorySSA) {
    MSSA = &getAnalysis<MemorySSAWrapperPass>().getMSSA();
  }
  Result = runIntraElimAvailableExpressions(&F, &AA, &DT, &TLI, MSSA,
                                            getElimOptions());
  if (ElimAvailPrint) {
    errs() << "== Elimination Available Expressions: " << F.getName()
           << " ==\n";
    printSolveMetadata(errs(), Result);
    for (auto &BB : F) {
      for (auto &I : BB) {
        errs() << "  ";
        I.print(errs());
        errs() << " :: ";
        if (const auto *Fact = Result.tryIN(&I)) {
          printExprSet(errs(), *Fact);
        } else {
          const std::set<ExpressionKey> Empty{};
          printExprSet(errs(), Empty);
        }
        errs() << "\n";
      }
    }
    errs() << "\n";
  }
  return false;
}

void ElimUninitVariablesPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addRequired<AssumptionCacheTracker>();
  AU.addRequired<DominatorTreeWrapperPass>();
}

bool ElimUninitVariablesPass::runOnFunction(Function &F) {
  auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
  auto &AC = getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  Result = runIntraElimUninitVariables(&F, &AA, &AC, &DT, getElimOptions());
  if (ElimUninitPrint) {
    errs() << "== Elimination Uninitialized Variables: " << F.getName()
           << " ==\n";
    printSolveMetadata(errs(), Result);
    for (auto &BB : F) {
      for (auto &I : BB) {
        errs() << "  ";
        I.print(errs());
        errs() << " :: ";
        if (const auto *Fact = Result.tryIN(&I)) {
          printUninitSet(errs(), *Fact);
        } else {
          const UninitVariablesFact Empty{};
          printUninitSet(errs(), Empty);
        }
        errs() << "\n";
      }
    }
    errs() << "\n";
  }
  return false;
}

void ElimLiveVariablesPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

bool ElimLiveVariablesPass::runOnFunction(Function &F) {
  Result = runIntraElimLiveVariables(&F, getElimOptions());
  if (ElimLivePrint) {
    errs() << "== Elimination Live Variables: " << F.getName() << " ==\n";
    printSolveMetadata(errs(), Result);
    for (auto &BB : F) {
      for (auto &I : BB) {
        errs() << "  ";
        I.print(errs());
        errs() << " :: ";
        if (const auto *Fact = Result.tryIN(&I)) {
          printValueSet(errs(), *Fact);
        } else {
          const std::set<const Value *> Empty{};
          printValueSet(errs(), Empty);
        }
        errs() << "\n";
      }
    }
    errs() << "\n";
  }
  return false;
}

void ElimVeryBusyExpressionsPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<MemorySSAWrapperPass>();
}

bool ElimVeryBusyExpressionsPass::runOnFunction(Function &F) {
  auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  llvm::MemorySSA *MSSA = nullptr;
  if (ElimUseMemorySSA) {
    MSSA = &getAnalysis<MemorySSAWrapperPass>().getMSSA();
  }
  Result = runIntraElimVeryBusyExpressions(&F, &AA, &DT, &TLI, MSSA,
                                           getElimOptions());
  if (ElimBusyPrint) {
    errs() << "== Elimination Very Busy Expressions: " << F.getName()
           << " ==\n";
    printSolveMetadata(errs(), Result);
    for (auto &BB : F) {
      for (auto &I : BB) {
        errs() << "  ";
        I.print(errs());
        errs() << " :: ";
        if (const auto *Fact = Result.tryIN(&I)) {
          printExprSet(errs(), *Fact);
        } else {
          const std::set<ExpressionKey> Empty{};
          printExprSet(errs(), Empty);
        }
        errs() << "\n";
      }
    }
    errs() << "\n";
  }
  return false;
}

void ElimNonNullPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<AssumptionCacheTracker>();
  AU.addRequired<DominatorTreeWrapperPass>();
}

bool ElimNonNullPass::runOnFunction(Function &F) {
  auto &AC = getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  Result = runIntraElimNonNull(&F, &AC, &DT, getElimOptions());
  if (ElimNonNullPrint) {
    errs() << "== Elimination NonNull: " << F.getName() << " ==\n";
    printSolveMetadata(errs(), Result);
    for (auto &BB : F) {
      for (auto &I : BB) {
        errs() << "  ";
        I.print(errs());
        errs() << " :: ";
        if (const auto *Fact = Result.tryIN(&I)) {
          printValueSet(errs(), *Fact);
        } else {
          const std::set<const Value *> Empty{};
          printValueSet(errs(), Empty);
        }
        errs() << "\n";
      }
    }
    errs() << "\n";
  }
  return false;
}

char ElimReachablePass::ID = 0;
static RegisterPass<ElimReachablePass>
    X("elim-reachable", "Elimination-based reachability (intra)");

char ElimConstantPropagationPass::ID = 0;
static RegisterPass<ElimConstantPropagationPass>
    Y("elim-constprop", "Elimination-based constant propagation (intra)");

char ElimReachingDefinitionsPass::ID = 0;
static RegisterPass<ElimReachingDefinitionsPass>
    RD("elim-rd", "Elimination-based reaching definitions (intra)");

char ElimAvailableExpressionsPass::ID = 0;
static RegisterPass<ElimAvailableExpressionsPass>
    AE("elim-available", "Elimination-based available expressions (intra)");

char ElimUninitVariablesPass::ID = 0;
static RegisterPass<ElimUninitVariablesPass>
    Z("elim-uninit", "Elimination-based uninitialized variables (intra)");

char ElimLiveVariablesPass::ID = 0;
static RegisterPass<ElimLiveVariablesPass>
    LV("elim-live", "Elimination-based live variables (intra)");

char ElimVeryBusyExpressionsPass::ID = 0;
static RegisterPass<ElimVeryBusyExpressionsPass>
    VB("elim-busy", "Elimination-based very busy expressions (intra)");

char ElimNonNullPass::ID = 0;
static RegisterPass<ElimNonNullPass> NN("elim-nonnull",
                                        "Elimination-based nonnull (intra)");

} // namespace elimination
