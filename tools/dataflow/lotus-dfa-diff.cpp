/*
 * lotus-dfa-diff
 *
 * Differential testing for lib/Dataflow engines.
 */

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/APA/Analyses/Intra/AvailableExpressions.h"
#include "Dataflow/APA/Analyses/Intra/ConstantPropagation.h"
#include "Dataflow/APA/Analyses/Intra/LiveVariables.h"
#include "Dataflow/APA/Analyses/Intra/Reachability.h"
#include "Dataflow/APA/Analyses/Intra/ReachingDefinitions.h"
#include "Dataflow/APA/Analyses/Intra/UninitializedVariables.h"
#include "Dataflow/IFDS/Analyses/IFDSReachingDefinitions.h"
#include "Dataflow/IFDS/Analyses/IFDSUninitializedVariables.h"
#include "Dataflow/IFDS/Solver/IFDSSolver.h"
#include "Dataflow/Mono/Analyses/Intra/ConstantPropagation.h"
#include "Dataflow/Mono/Analyses/Intra/LiveVariables.h"
#include "Dataflow/Mono/Analyses/Intra/Reachability.h"
#include "Dataflow/Mono/Analyses/Intra/UninitializedVariables.h"
#include "ToolSupport.h"

#include <algorithm>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
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
             "constant_prop, available_exprs, reachable"),
    cl::init("liveness"));
static cl::opt<std::string> ElimMethodOpt(
    "elim-method",
    cl::desc("Elimination solver method: state|adt-simple|adt-delayed"),
    cl::init("state"));
static cl::opt<std::string>
    EngineOpt("engine",
              cl::desc("Engine(s): elim, mono, ifds, all (default: all)"),
              cl::init("all"));

namespace {

using lotus::dataflow_tool::FunctionView;
using lotus::dataflow_tool::ValueIdMap;

mono::DebugConfig quietMonoDebugConfig() {
  mono::DebugConfig Config;
  Config.collect_statistics = false;
  return Config;
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

std::string formatMonoConstantValue(const mono::ConstantPropagationValue &Val) {
  std::ostringstream ss;
  switch (Val.Tag) {
  case mono::ConstantPropagationTag::Top:
    ss << "top";
    break;
  case mono::ConstantPropagationTag::Const:
    ss << "const" << Val.ConstValue;
    break;
  case mono::ConstantPropagationTag::Bottom:
    ss << "bottom";
    break;
  }
  return ss.str();
}

std::string formatIFDSFact(const ifds::DefinitionFact &Fact,
                           const ValueIdMap &ValueToId) {
  if (Fact.is_zero())
    return "zero";
  std::ostringstream ss;
  auto VarIt = ValueToId.find(Fact.get_variable());
  auto DefIt = ValueToId.find(Fact.get_definition_site());
  ss << "def(" << (VarIt != ValueToId.end() ? VarIt->second : "v") << ","
     << (DefIt != ValueToId.end() ? DefIt->second : "i") << ")";
  return ss.str();
}

std::string formatIFDSFact(const ifds::UninitVarFact &Fact,
                           const ValueIdMap &ValueToId) {
  if (Fact.is_zero())
    return "zero";
  std::ostringstream ss;
  auto It = ValueToId.find(Fact.value);
  ss << (Fact.is_uninitialized() ? "uninit(" : "init(")
     << (It != ValueToId.end() ? It->second : "v") << ")";
  return ss.str();
}

template <typename FactT>
void formatIFDSFactSet(raw_ostream &OS, const std::set<FactT> &Facts,
                       const ValueIdMap &ValueToId) {
  std::vector<std::string> formatted;
  for (const auto &Fact : Facts)
    formatted.push_back(formatIFDSFact(Fact, ValueToId));
  std::sort(formatted.begin(), formatted.end());
  for (size_t i = 0; i < formatted.size(); ++i) {
    if (i)
      OS << ",";
    OS << formatted[i];
  }
}

const Instruction *getNextInstruction(const Instruction *I) {
  if (auto *Next = I->getNextNode())
    return Next;
  for (auto *Succ : successors(I->getParent())) {
    if (Succ->isLandingPad() || Succ->empty())
      continue;
    return &Succ->front();
  }
  return nullptr;
}

class OutputManager {
  std::unique_ptr<raw_fd_ostream> ElimOut;
  std::unique_ptr<raw_fd_ostream> MonoOut;
  std::unique_ptr<raw_fd_ostream> IFDSOut;
  raw_null_ostream NullOut;

public:
  raw_ostream &getStream(StringRef Engine) {
    if (OutDir.empty()) {
      if (StdoutOpt)
        return outs();
      return NullOut;
    }

    auto open = [&](std::unique_ptr<raw_fd_ostream> &Out,
                    const char *Name) -> raw_ostream & {
      if (!Out) {
        std::error_code EC;
        Out = lotus::dataflow_tool::openOutputFileOrReport(OutDir, Name, EC);
        if (EC)
          errs() << "warning: cannot create " << Name << ": " << EC.message()
                 << "\n";
      }
      if (Out)
        return *Out;
      return NullOut;
    };

    if (Engine == "elim")
      return open(ElimOut, "elim.txt");
    if (Engine == "mono")
      return open(MonoOut, "mono.txt");
    if (Engine == "ifds")
      return open(IFDSOut, "ifds.txt");
    if (StdoutOpt)
      return outs();
    return NullOut;
  }
};

struct ElimHandler final {
  StringRef Name;
  void (*Run)(raw_ostream &, const FunctionView &,
              const elimination::EliminationOptions &);
};

void runElimLiveness(raw_ostream &OS, const FunctionView &View,
                     const elimination::EliminationOptions &Opts) {
  auto Res = elimination::runIntraElimLiveVariables(&View.Function, Opts);
  lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
    lotus::dataflow_tool::formatValueSet(OS, Res.IN(I), View.ValueToId);
  });
}

void runElimReachingDefinitions(raw_ostream &OS, const FunctionView &View,
                                const elimination::EliminationOptions &Opts) {
  auto Res = elimination::runIntraElimReachingDefinitions(&View.Function,
                                                          nullptr, Opts);
  lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
    lotus::dataflow_tool::formatValueSet(OS, Res.IN(I), View.ValueToId);
  });
}

void runElimUninitialized(raw_ostream &OS, const FunctionView &View,
                          const elimination::EliminationOptions &Opts) {
  auto Res =
      elimination::runIntraElimUninitVariables(&View.Function, nullptr, Opts);
  lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
    lotus::dataflow_tool::formatValueSet(OS, Res.IN(I), View.ValueToId);
  });
}

void runElimConstantPropagation(raw_ostream &OS, const FunctionView &View,
                                const elimination::EliminationOptions &Opts) {
  auto Res = elimination::runIntraElimConstantPropagation(&View.Function,
                                                          nullptr, Opts);
  lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
    lotus::dataflow_tool::formatValueMap(
        OS, Res.IN(I), View.ValueToId, [&](const ValueLatticeElement &Val) {
          return formatValueLatticeElement(Val);
        });
  });
}

void runElimAvailableExpressions(raw_ostream &OS, const FunctionView &View,
                                 const elimination::EliminationOptions &Opts) {
  auto Res = elimination::runIntraElimAvailableExpressions(&View.Function,
                                                           nullptr, Opts);
  lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
    std::vector<std::string> Exprs;
    for (const auto &Expr : Res.IN(I))
      Exprs.push_back(formatExpressionKey(Expr));
    std::sort(Exprs.begin(), Exprs.end());
    for (size_t Index = 0; Index < Exprs.size(); ++Index) {
      if (Index)
        OS << ",";
      OS << Exprs[Index];
    }
  });
}

void runElimReachable(raw_ostream &OS, const FunctionView &View,
                      const elimination::EliminationOptions &Opts) {
  auto Res = elimination::runIntraElimReachable(&View.Function, Opts);
  lotus::dataflow_tool::printInstructionStates(
      OS, View, [&](Instruction *I) { OS << (Res.IN(I) ? "true" : "false"); });
}

struct MonoHandler final {
  StringRef Name;
  void (*Run)(raw_ostream &, const FunctionView &);
};

void runMonoLiveness(raw_ostream &OS, const FunctionView &View) {
  if (auto Res = mono::runLiveVariablesAnalysis(&View.Function,
                                                quietMonoDebugConfig()))
    lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
      lotus::dataflow_tool::formatValueSet(OS, Res->IN(I), View.ValueToId);
    });
}

void runMonoReachable(raw_ostream &OS, const FunctionView &View) {
  if (auto Res =
          mono::runReachableAnalysis(&View.Function, quietMonoDebugConfig()))
    lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
      lotus::dataflow_tool::formatValueSet(OS, Res->IN(I), View.ValueToId);
    });
}

void runMonoConstantPropagation(raw_ostream &OS, const FunctionView &View) {
  auto Res = mono::runIntraMonoConstantPropagation(&View.Function,
                                                   quietMonoDebugConfig());
  lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
    auto It = Res.find(I);
    if (It != Res.end())
      lotus::dataflow_tool::formatValueMap(
          OS, It->second, View.ValueToId,
          [&](const mono::ConstantPropagationValue &Val) {
            return formatMonoConstantValue(Val);
          });
  });
}

void runMonoUninitialized(raw_ostream &OS, const FunctionView &View) {
  if (auto Res = mono::runIntraMonoUninitVariables(&View.Function,
                                                   quietMonoDebugConfig()))
    lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
      lotus::dataflow_tool::formatValueSet(OS, Res->IN(I), View.ValueToId);
    });
}

struct IFDSHandler final {
  StringRef Name;
  void (*Run)(raw_ostream &, Module &);
};

template <typename Fact, typename ResultsT>
void printIFDSResults(raw_ostream &OS, const FunctionView &View,
                      const ResultsT &AllResults) {
  lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
    if (const Instruction *NextInst = getNextInstruction(I)) {
      auto Node =
          typename ifds::ExplodedSupergraph<Fact>::Node(NextInst, Fact::zero());
      auto It = AllResults.find(Node);
      if (It != AllResults.end())
        formatIFDSFactSet(OS, It->second, View.ValueToId);
    }
  });
}

void runIFDSReachingDefinitions(raw_ostream &OS, Module &M) {
  ifds::ReachingDefinitionsAnalysis Problem;
  ifds::IFDSSolver<ifds::ReachingDefinitionsAnalysis> Solver(Problem);
  Solver.solve(M);
  const auto AllResults = Solver.get_all_results();
  lotus::dataflow_tool::forEachDefinedFunction(
      M, OS, [&](const FunctionView &View) {
        printIFDSResults<ifds::DefinitionFact>(OS, View, AllResults);
      });
}

void runIFDSUninitialized(raw_ostream &OS, Module &M) {
  ifds::UninitializedVariablesAnalysis Problem;
  ifds::IFDSSolver<ifds::UninitializedVariablesAnalysis> Solver(Problem);
  Solver.solve(M);
  const auto AllResults = Solver.get_all_results();
  lotus::dataflow_tool::forEachDefinedFunction(
      M, OS, [&](const FunctionView &View) {
        printIFDSResults<ifds::UninitVarFact>(OS, View, AllResults);
      });
}

const ElimHandler ElimHandlers[] = {
    {"liveness", &runElimLiveness},
    {"reaching_defs", &runElimReachingDefinitions},
    {"uninitialized", &runElimUninitialized},
    {"constant_prop", &runElimConstantPropagation},
    {"available_exprs", &runElimAvailableExpressions},
    {"reachable", &runElimReachable},
};

const MonoHandler MonoHandlers[] = {
    {"liveness", &runMonoLiveness},
    {"reachable", &runMonoReachable},
    {"constant_prop", &runMonoConstantPropagation},
    {"uninitialized", &runMonoUninitialized},
};

const IFDSHandler IFDSHandlers[] = {
    {"reaching_defs", &runIFDSReachingDefinitions},
    {"uninitialized", &runIFDSUninitialized},
};

template <typename HandlerT>
bool emitHeader(raw_ostream &OS, StringRef Engine, const HandlerT *Handler) {
  if (!OutDir.empty())
    return Handler != nullptr;
  if (!StdoutOpt)
    return Handler != nullptr;
  OS << "[" << Engine << ":" << AnalysisOpt << "]\n";
  return Handler != nullptr;
}

void runEliminationAnalysis(Module &M, OutputManager &OutMgr,
                            const elimination::EliminationOptions &ElimOpts) {
  raw_ostream &OS = OutMgr.getStream("elim");
  const auto *Handler =
      lotus::dataflow_tool::findHandler(StringRef(AnalysisOpt), ElimHandlers);
  if (!emitHeader(OS, "elim", Handler))
    return;

  lotus::dataflow_tool::forEachDefinedFunction(
      M, OS,
      [&](const FunctionView &View) { Handler->Run(OS, View, ElimOpts); });
}

void runMonoAnalysis(Module &M, OutputManager &OutMgr) {
  raw_ostream &OS = OutMgr.getStream("mono");
  const auto *Handler =
      lotus::dataflow_tool::findHandler(StringRef(AnalysisOpt), MonoHandlers);
  if (!emitHeader(OS, "mono", Handler))
    return;

  lotus::dataflow_tool::forEachDefinedFunction(
      M, OS, [&](const FunctionView &View) { Handler->Run(OS, View); });
}

void runIFDSAnalysis(Module &M, OutputManager &OutMgr) {
  raw_ostream &OS = OutMgr.getStream("ifds");
  const auto *Handler =
      lotus::dataflow_tool::findHandler(StringRef(AnalysisOpt), IFDSHandlers);
  if (!OutDir.empty() && !Handler)
    return;

  OS << "[ifds:" << AnalysisOpt << "]\n";
  if (!Handler) {
    OS << "  (not implemented for IFDS)\n";
    return;
  }
  Handler->Run(OS, M);
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Dataflow engine diff testing\n");

  LLVMContext Context;
  SMDiagnostic Err;
  auto M = lotus::dataflow_tool::loadModuleOrReport(InputFilename, Context, Err,
                                                    argv[0]);
  if (!M)
    return 1;

  lotus::dataflow_tool::prepareModule(*M);

  OutputManager OutMgr;
  const auto ElimOpts =
      lotus::dataflow_tool::parseEliminationOptions(ElimMethodOpt);
  const bool RunElim = EngineOpt == "elim" || EngineOpt == "all";
  const bool RunMono = EngineOpt == "mono" || EngineOpt == "all";
  const bool RunIFDS = EngineOpt == "ifds" || EngineOpt == "all";

  if (RunElim)
    runEliminationAnalysis(*M, OutMgr, ElimOpts);
  if (RunMono)
    runMonoAnalysis(*M, OutMgr);
  if (RunIFDS)
    runIFDSAnalysis(*M, OutMgr);

  return 0;
}
