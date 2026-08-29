/*
 * lotus-dfa-apa
 *
 * Dataflow testing tool: APA (Algebraic Program Analysis) engine.
 */

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/APA/Analyses/Inter/ConstantPropagation.h"
#include "Dataflow/APA/Analyses/Inter/LiveVariables.h"
#include "Dataflow/APA/Analyses/Inter/Reachability.h"
#include "Dataflow/APA/Analyses/Inter/ReachingDefinitions.h"
#include "Dataflow/APA/Analyses/Inter/UninitializedVariables.h"
#include "Dataflow/APA/Analyses/Intra/AvailableExpressions.h"
#include "Dataflow/APA/Analyses/Intra/ConstantPropagation.h"
#include "Dataflow/APA/Analyses/Intra/LiveVariables.h"
#include "Dataflow/APA/Analyses/Intra/Reachability.h"
#include "Dataflow/APA/Analyses/Intra/ReachingDefinitions.h"
#include "Dataflow/APA/Analyses/Intra/UninitializedVariables.h"
#include "ToolSupport.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<bitcode>"),
                                          cl::Required);
static cl::opt<std::string> OutDir("out-dir", cl::desc("Output directory"),
                                   cl::value_desc("dir"), cl::init(""));
static cl::opt<bool> StdoutOpt(
    "stdout",
    cl::desc("Write analysis results to stdout when --out-dir is not set"),
    cl::init(false));
static cl::opt<std::string> AnalysisOpt(
    "analysis",
    cl::desc("Analysis: liveness (default), reaching_defs, uninitialized, "
             "constant_prop, available_exprs, reachable, inter_liveness, "
             "inter_reaching_defs, inter_uninitialized, inter_constant_prop, "
             "inter_reachable"),
    cl::init("liveness"));
static cl::opt<std::string>
    EntryFunctionOpt("entry-function",
                     cl::desc("Entry function for interprocedural analyses"),
                     cl::init("main"));
static cl::opt<std::string> ElimMethodOpt(
    "elim-method",
    cl::desc("Elimination solver method: state|adt-simple|adt-delayed"),
    cl::init("state"));
static cl::opt<bool>
    DumpProfileOpt("dump-profile",
                   cl::desc("Dump solver and path-expression profiling data"),
                   cl::init(false));
static cl::opt<bool>
    DumpExprsOpt("dump-exprs",
                 cl::desc("Dump per-instruction path-expression summaries"),
                 cl::init(false));

namespace {

using lotus::dataflow_tool::FunctionView;
using lotus::dataflow_tool::ValueIdMap;
using InstructionExprFactory = elimination::PathExprFactory<Instruction *>;
using InstructionExprRef = InstructionExprFactory::Ref;

ValueIdMap buildModuleValueIdMap(Module &M) {
  ValueIdMap ValueToId;
  for (auto &G : M.globals()) {
    ValueToId[&G] = (G.hasName() ? G.getName() : "global").str();
  }

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    const std::string Prefix = F.getName().str();
    unsigned ArgIdx = 0;
    for (auto &Arg : F.args()) {
      ValueToId[&Arg] = Prefix + ".arg" + std::to_string(ArgIdx++);
    }

    unsigned InstIdx = 0;
    for (auto &BB : F) {
      for (auto &I : BB) {
        ValueToId[&I] = Prefix + ".i" + std::to_string(InstIdx++);
      }
    }
  }
  return ValueToId;
}

template <typename ContextT> std::string contextToString(const ContextT &Ctx) {
  std::string Buffer;
  raw_string_ostream OS(Buffer);
  Ctx.print(OS);
  return OS.str();
}

const char *toString(elimination::EliminationMethod M) {
  switch (M) {
  case elimination::EliminationMethod::StateElimination:
    return "state";
  case elimination::EliminationMethod::ADTSimple:
    return "adt-simple";
  case elimination::EliminationMethod::ADTDelayed:
    return "adt-delayed";
  }
  return "unknown";
}

const char *toString(elimination::SolveStatus S) {
  switch (S) {
  case elimination::SolveStatus::Ok:
    return "ok";
  case elimination::SolveStatus::FallbackToState:
    return "fallback-to-state";
  case elimination::SolveStatus::NonConvergentStar:
    return "non-convergent-star";
  case elimination::SolveStatus::InvalidProblem:
    return "invalid-problem";
  }
  return "unknown";
}

const char *toString(elimination::FallbackReason R) {
  switch (R) {
  case elimination::FallbackReason::None:
    return "none";
  case elimination::FallbackReason::ADTRejected:
    return "adt-rejected";
  case elimination::FallbackReason::InvalidProblem:
    return "invalid-problem";
  }
  return "unknown";
}

std::string formatExpressionKey(const elimination::ExpressionKey &Key) {
  std::ostringstream ss;
  ss << "op" << Key.Opcode << "(";
  for (size_t i = 0; i < Key.Ops.size(); ++i) {
    if (i)
      ss << ",";
    ss << Key.Ops[i];
  }
  ss << ")";
  return ss.str();
}

std::string formatValueLatticeElement(const ValueLatticeElement &Val) {
  std::ostringstream ss;
  if (Val.isUndef())
    ss << "undef";
  else if (Val.isUnknown())
    ss << "unknown";
  else if (Val.isOverdefined())
    ss << "overdefined";
  else if (Val.isNotConstant())
    ss << "notconst";
  else if (Val.isConstant()) {
    if (auto *CI = dyn_cast<ConstantInt>(Val.getConstant()))
      ss << "const" << CI->getZExtValue();
    else
      ss << "const";
  } else
    ss << "lattice";
  return ss.str();
}

struct CFGStats final {
  size_t Arguments = 0;
  size_t Blocks = 0;
  size_t Instructions = 0;
  size_t Edges = 0;
  size_t BranchingBlocks = 0;
  size_t MaxSuccessors = 0;
  size_t PhiNodes = 0;
  size_t Calls = 0;
  size_t Returns = 0;
  size_t Unreachable = 0;
};

CFGStats collectCFGStats(const Function &F) {
  CFGStats Stats;
  Stats.Arguments = F.arg_size();
  for (const auto &BB : F) {
    ++Stats.Blocks;
    Stats.Instructions += BB.size();
    for (const auto &I : BB) {
      if (isa<PHINode>(I))
        ++Stats.PhiNodes;
      if (isa<CallBase>(I))
        ++Stats.Calls;
      if (isa<ReturnInst>(I))
        ++Stats.Returns;
      if (isa<UnreachableInst>(I))
        ++Stats.Unreachable;
    }
    const size_t Succs = succ_size(&BB);
    Stats.Edges += Succs;
    Stats.MaxSuccessors = std::max(Stats.MaxSuccessors, Succs);
    if (Succs > 1)
      ++Stats.BranchingBlocks;
  }
  return Stats;
}

struct ExprProfile final {
  size_t UniqueNodes = 0;
  size_t SharedRefs = 0;
  size_t MaxDepth = 0;
  size_t ZeroNodes = 0;
  size_t OneNodes = 0;
  size_t AtomNodes = 0;
  size_t UnionNodes = 0;
  size_t ConcatNodes = 0;
  size_t StarNodes = 0;
};

void collectExprProfileImpl(const InstructionExprRef &Expr, size_t Depth,
                            std::unordered_set<const void *> &Visited,
                            ExprProfile &Profile) {
  if (!Expr)
    return;

  Profile.MaxDepth = std::max(Profile.MaxDepth, Depth);
  if (!Visited.insert(Expr.get()).second) {
    ++Profile.SharedRefs;
    return;
  }

  ++Profile.UniqueNodes;
  switch (Expr->K) {
  case InstructionExprFactory::Kind::Zero:
    ++Profile.ZeroNodes;
    return;
  case InstructionExprFactory::Kind::One:
    ++Profile.OneNodes;
    return;
  case InstructionExprFactory::Kind::Atom:
    ++Profile.AtomNodes;
    return;
  case InstructionExprFactory::Kind::Union:
    ++Profile.UnionNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    collectExprProfileImpl(Expr->R, Depth + 1, Visited, Profile);
    return;
  case InstructionExprFactory::Kind::Concat:
    ++Profile.ConcatNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    collectExprProfileImpl(Expr->R, Depth + 1, Visited, Profile);
    return;
  case InstructionExprFactory::Kind::Star:
    ++Profile.StarNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    return;
  }
}

ExprProfile collectExprProfile(const InstructionExprRef &Expr) {
  ExprProfile Profile;
  std::unordered_set<const void *> Visited;
  collectExprProfileImpl(Expr, 1, Visited, Profile);
  return Profile;
}

std::string formatTransfer(const Instruction *I, const ValueIdMap &ValueToId) {
  if (!I)
    return "null";
  auto It = ValueToId.find(I);
  return It != ValueToId.end() ? It->second : "inst";
}

void formatPathExpr(raw_ostream &OS, const InstructionExprRef &Expr,
                    const ValueIdMap &ValueToId) {
  if (!Expr) {
    OS << "null";
    return;
  }

  switch (Expr->K) {
  case InstructionExprFactory::Kind::Zero:
    OS << "zero";
    return;
  case InstructionExprFactory::Kind::One:
    OS << "one";
    return;
  case InstructionExprFactory::Kind::Atom:
    OS << "atom("
       << formatTransfer(Expr->Transfer ? *Expr->Transfer : nullptr, ValueToId)
       << ")";
    return;
  case InstructionExprFactory::Kind::Union:
    OS << "union(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ",";
    formatPathExpr(OS, Expr->R, ValueToId);
    OS << ")";
    return;
  case InstructionExprFactory::Kind::Concat:
    OS << "concat(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ",";
    formatPathExpr(OS, Expr->R, ValueToId);
    OS << ")";
    return;
  case InstructionExprFactory::Kind::Star:
    OS << "star(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ")";
    return;
  }
}

template <typename ResultT>
void printSolveMetadata(raw_ostream &OS, const ResultT &Result) {
  if (!Result.hasSolveMetadata())
    return;
  const auto &Diag = Result.solveDiagnostics();
  OS << "  [solver] status=" << toString(Result.solveStatus())
     << ", requested=" << toString(Diag.requested_method)
     << ", executed=" << toString(Diag.executed_method)
     << ", used_adt=" << (Diag.used_adt ? "true" : "false")
     << ", fallback=" << toString(Diag.fallback_reason)
     << ", star_iters=" << Diag.star_iterations_total
     << ", max_star_hit=" << (Diag.max_star_hit ? "true" : "false") << "\n";
}

template <unsigned K, typename FactT, typename TransferT, typename NodeT>
void printSolveMetadata(
    raw_ostream &OS,
    const elimination::InterDataFlowResultT<K, FactT, TransferT, NodeT>
        &Result) {
  if (!Result.hasSolveMetadata())
    return;
  OS << "  [solver] status=" << toString(Result.solveStatus()) << "\n";
}

template <typename ResultT>
void dumpProfile(raw_ostream &OS, const FunctionView &View,
                 const ResultT &Result, std::chrono::microseconds Elapsed) {
  const auto CFG = collectCFGStats(View.Function);
  OS << "  [cfg] args=" << CFG.Arguments << ", blocks=" << CFG.Blocks
     << ", insts=" << CFG.Instructions << ", edges=" << CFG.Edges
     << ", branching_blocks=" << CFG.BranchingBlocks
     << ", max_succs=" << CFG.MaxSuccessors << ", phis=" << CFG.PhiNodes
     << ", calls=" << CFG.Calls << ", returns=" << CFG.Returns
     << ", unreachable=" << CFG.Unreachable
     << ", elapsed_us=" << Elapsed.count() << "\n";
  printSolveMetadata(OS, Result);

  size_t NodesWithExpr = 0;
  size_t MissingExpr = 0;
  size_t TotalUniqueNodes = 0;
  size_t TotalSharedRefs = 0;
  size_t TotalStars = 0;
  size_t TotalUnions = 0;
  size_t TotalConcats = 0;
  size_t MaxExprNodes = 0;
  size_t MaxExprDepth = 0;
  std::string MaxExprInst = "none";
  std::string DeepestExprInst = "none";

  for (auto *I : View.OrderedInsts) {
    const auto Expr = Result.ExprTo(I);
    if (!Expr) {
      ++MissingExpr;
      continue;
    }
    ++NodesWithExpr;
    const auto Profile = collectExprProfile(Expr);
    TotalUniqueNodes += Profile.UniqueNodes;
    TotalSharedRefs += Profile.SharedRefs;
    TotalStars += Profile.StarNodes;
    TotalUnions += Profile.UnionNodes;
    TotalConcats += Profile.ConcatNodes;
    if (Profile.UniqueNodes > MaxExprNodes) {
      MaxExprNodes = Profile.UniqueNodes;
      MaxExprInst = View.ValueToId.at(I);
    }
    if (Profile.MaxDepth > MaxExprDepth) {
      MaxExprDepth = Profile.MaxDepth;
      DeepestExprInst = View.ValueToId.at(I);
    }
  }

  OS << "  [expr-profile] with_expr=" << NodesWithExpr
     << ", missing_expr=" << MissingExpr
     << ", total_unique_nodes=" << TotalUniqueNodes
     << ", total_shared_refs=" << TotalSharedRefs
     << ", total_unions=" << TotalUnions << ", total_concats=" << TotalConcats
     << ", total_stars=" << TotalStars << ", max_nodes=" << MaxExprNodes << "@"
     << MaxExprInst << ", max_depth=" << MaxExprDepth << "@" << DeepestExprInst
     << "\n";

  if (!DumpExprsOpt)
    return;

  for (auto *I : View.OrderedInsts) {
    const auto Expr = Result.ExprTo(I);
    OS << "  [expr] " << View.ValueToId.at(I);
    if (!Expr) {
      OS << " missing\n";
      continue;
    }
    const auto Profile = collectExprProfile(Expr);
    OS << " nodes=" << Profile.UniqueNodes << ", depth=" << Profile.MaxDepth
       << ", atoms=" << Profile.AtomNodes << ", unions=" << Profile.UnionNodes
       << ", concats=" << Profile.ConcatNodes << ", stars=" << Profile.StarNodes
       << ", shared_refs=" << Profile.SharedRefs << ", expr=";
    formatPathExpr(OS, Expr, View.ValueToId);
    OS << "\n";
  }
}

template <typename ResultT, typename Printer>
void dumpTimedResult(raw_ostream &OS, const FunctionView &View, ResultT &Result,
                     std::chrono::microseconds Elapsed, Printer &&PrintState) {
  if (DumpProfileOpt || DumpExprsOpt)
    dumpProfile(OS, View, Result, Elapsed);
  lotus::dataflow_tool::printInstructionStates(
      OS, View, [&](Instruction *I) { PrintState(I, Result); });
}

template <typename Runner, typename Printer>
void runTimedAnalysis(raw_ostream &OS, const FunctionView &View,
                      const elimination::EliminationOptions &ElimOpts,
                      Runner &&Run, Printer &&PrintState) {
  const auto Start = std::chrono::steady_clock::now();
  auto Result = Run(View.Function, ElimOpts);
  const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - Start);
  dumpTimedResult(OS, View, Result, Elapsed, std::forward<Printer>(PrintState));
}

template <typename ResultT, typename Printer>
void dumpInterproceduralResult(raw_ostream &OS, Module &M,
                               const ValueIdMap &ValueToId,
                               const ResultT &Result, Printer &&PrintState) {
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    lotus::dataflow_tool::emitFunctionHeader(OS, F);
    for (auto &BB : F) {
      for (auto &I : BB) {
        auto Contexts = Result.contextsForInstruction(&I);
        std::sort(Contexts.begin(), Contexts.end(),
                  [&](const auto &Lhs, const auto &Rhs) {
                    return contextToString(Lhs.Ctx) < contextToString(Rhs.Ctx);
                  });
        if (Contexts.empty()) {
          OS << "  " << ValueToId.at(&I) << " IN: <no-context>\n";
          continue;
        }
        for (const auto &Key : Contexts) {
          OS << "  " << ValueToId.at(&I) << " IN [" << contextToString(Key.Ctx)
             << "]: ";
          PrintState(Key, Result);
          OS << "\n";
        }
      }
    }
  }
}

template <typename Runner, typename Printer>
void runTimedInterproceduralAnalysis(raw_ostream &OS, Module &M,
                                     Function &Entry, Runner &&Run,
                                     Printer &&PrintState) {
  const auto Start = std::chrono::steady_clock::now();
  auto Result = Run(Entry);
  const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - Start);
  const auto ValueToId = buildModuleValueIdMap(M);
  OS << "  [profile] elapsed_us=" << Elapsed.count() << "\n";
  printSolveMetadata(OS, Result);
  dumpInterproceduralResult(OS, M, ValueToId, Result,
                            [&](const auto &Key, const auto &Res) {
                              PrintState(Key, Res, ValueToId);
                            });
}

template <typename Runner>
void runSetIntraAnalysis(raw_ostream &OS, const FunctionView &View,
                         const elimination::EliminationOptions &ElimOpts,
                         Runner &&Run) {
  runTimedAnalysis(OS, View, ElimOpts, std::forward<Runner>(Run),
                   [&](Instruction *I, auto &Result) {
                     lotus::dataflow_tool::formatValueSet(OS, Result.IN(I),
                                                          View.ValueToId);
                   });
}

template <typename Runner>
void runBoolIntraAnalysis(raw_ostream &OS, const FunctionView &View,
                          const elimination::EliminationOptions &ElimOpts,
                          Runner &&Run) {
  runTimedAnalysis(OS, View, ElimOpts, std::forward<Runner>(Run),
                   [&](Instruction *I, auto &Result) {
                     OS << (Result.IN(I) ? "true" : "false");
                   });
}

template <typename Runner>
void runSetInterAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                         Runner &&Run) {
  runTimedInterproceduralAnalysis(
      OS, M, Entry, std::forward<Runner>(Run),
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueSet(OS, Result.IN(Key), ValueToId);
      });
}

template <typename Runner, typename Formatter>
void runMapInterAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                         Runner &&Run, Formatter &&FormatValue) {
  runTimedInterproceduralAnalysis(
      OS, M, Entry, std::forward<Runner>(Run),
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueMap(OS, Result.IN(Key), ValueToId,
                                             FormatValue);
      });
}

template <typename Runner>
void runBoolInterAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                          Runner &&Run) {
  runTimedInterproceduralAnalysis(
      OS, M, Entry, std::forward<Runner>(Run),
      [&](const auto &Key, const auto &Result, const auto & /*ValueToId*/) {
        OS << (Result.IN(Key) ? "true" : "false");
      });
}

void runLiveness(raw_ostream &OS, const FunctionView &View,
                 const elimination::EliminationOptions &ElimOpts) {
  runSetIntraAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimLiveVariables(&F, Opts);
      });
}

void runReachingDefinitions(raw_ostream &OS, const FunctionView &View,
                            const elimination::EliminationOptions &ElimOpts) {
  runSetIntraAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimReachingDefinitions(&F, nullptr, Opts);
      });
}

void runUninitialized(raw_ostream &OS, const FunctionView &View,
                      const elimination::EliminationOptions &ElimOpts) {
  runSetIntraAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimUninitVariables(&F, nullptr, Opts);
      });
}

void runConstantPropagation(raw_ostream &OS, const FunctionView &View,
                            const elimination::EliminationOptions &ElimOpts) {
  runTimedAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimConstantPropagation(&F, nullptr, Opts);
      },
      [&](Instruction *I, auto &Result) {
        lotus::dataflow_tool::formatValueMap(
            OS, Result.IN(I), View.ValueToId,
            [&](const ValueLatticeElement &Value) {
              return formatValueLatticeElement(Value);
            });
      });
}

void runAvailableExpressions(raw_ostream &OS, const FunctionView &View,
                             const elimination::EliminationOptions &ElimOpts) {
  runTimedAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimAvailableExpressions(&F, nullptr, Opts);
      },
      [&](Instruction *I, auto &Result) {
        std::vector<std::string> Exprs;
        for (const auto &Expr : Result.IN(I))
          Exprs.push_back(formatExpressionKey(Expr));
        std::sort(Exprs.begin(), Exprs.end());
        for (size_t Index = 0; Index < Exprs.size(); ++Index) {
          if (Index)
            OS << ",";
          OS << Exprs[Index];
        }
      });
}

void runReachable(raw_ostream &OS, const FunctionView &View,
                  const elimination::EliminationOptions &ElimOpts) {
  runBoolIntraAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimReachable(&F, Opts);
      });
}

void runInterLiveness(raw_ostream &OS, Module &M, Function &Entry) {
  runSetInterAnalysis(OS, M, Entry, [](Function &F) {
    return elimination::runInterElimLiveVariables(&F);
  });
}

void runInterReachingDefinitions(raw_ostream &OS, Module &M, Function &Entry) {
  runSetInterAnalysis(OS, M, Entry, [](Function &F) {
    return elimination::runInterElimReachingDefinitions(&F);
  });
}

void runInterUninitialized(raw_ostream &OS, Module &M, Function &Entry) {
  runSetInterAnalysis(OS, M, Entry, [](Function &F) {
    return elimination::runInterElimUninitVariables(&F);
  });
}

void runInterConstantPropagation(raw_ostream &OS, Module &M, Function &Entry) {
  runMapInterAnalysis(
      OS, M, Entry,
      [](Function &F) {
        return elimination::runInterElimConstantPropagation(&F);
      },
      [&](const ValueLatticeElement &Value) {
        return formatValueLatticeElement(Value);
      });
}

void runInterReachable(raw_ostream &OS, Module &M, Function &Entry) {
  runBoolInterAnalysis(OS, M, Entry, [](Function &F) {
    return elimination::runInterElimReachable(&F);
  });
}

struct AnalysisHandler final {
  StringRef Name;
  bool ModuleScoped = false;
  void (*RunFunction)(raw_ostream &, const FunctionView &,
                      const elimination::EliminationOptions &) = nullptr;
  void (*RunModule)(raw_ostream &, Module &, Function &) = nullptr;
};

const AnalysisHandler Handlers[] = {
    {"liveness", false, &runLiveness, nullptr},
    {"reaching_defs", false, &runReachingDefinitions, nullptr},
    {"uninitialized", false, &runUninitialized, nullptr},
    {"constant_prop", false, &runConstantPropagation, nullptr},
    {"available_exprs", false, &runAvailableExpressions, nullptr},
    {"reachable", false, &runReachable, nullptr},
    {"inter_liveness", true, nullptr, &runInterLiveness},
    {"inter_reaching_defs", true, nullptr, &runInterReachingDefinitions},
    {"inter_uninitialized", true, nullptr, &runInterUninitialized},
    {"inter_constant_prop", true, nullptr, &runInterConstantPropagation},
    {"inter_reachable", true, nullptr, &runInterReachable},
};

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Elimination engine testing\n");

  LLVMContext Context;
  SMDiagnostic Err;
  auto M = lotus::dataflow_tool::loadModuleOrReport(InputFilename, Context, Err,
                                                    argv[0]);
  if (!M)
    return 1;

  lotus::dataflow_tool::prepareModule(*M);

  raw_null_ostream NullOS;
  std::unique_ptr<raw_fd_ostream> FileOS;
  std::error_code EC;
  raw_ostream &OS = lotus::dataflow_tool::selectOutputStream(
      StdoutOpt, OutDir, "elim.txt", FileOS, NullOS, EC);
  if (EC) {
    errs() << "error: cannot create " << OutDir << "/elim.txt: " << EC.message()
           << "\n";
    return 1;
  }

  const auto *Handler =
      lotus::dataflow_tool::findHandler(AnalysisOpt, Handlers);
  if (!Handler) {
    errs() << "error: unknown elimination analysis '" << AnalysisOpt << "'\n";
    return 1;
  }

  const auto ElimOpts =
      lotus::dataflow_tool::parseEliminationOptions(ElimMethodOpt);
  OS << "[elim:" << AnalysisOpt << "]\n";

  if (Handler->ModuleScoped) {
    Function *Entry = M->getFunction(EntryFunctionOpt);
    if (Entry == nullptr || Entry->isDeclaration()) {
      errs() << "error: entry function '" << EntryFunctionOpt
             << "' not found or is a declaration\n";
      return 1;
    }
    Handler->RunModule(OS, *M, *Entry);
  } else {
    lotus::dataflow_tool::forEachDefinedFunction(
        *M, OS, [&](const FunctionView &View) {
          Handler->RunFunction(OS, View, ElimOpts);
        });
  }

  return 0;
}
