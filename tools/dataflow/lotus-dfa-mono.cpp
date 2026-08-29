/*
 * lotus-dfa-mono
 *
 * Dataflow testing tool: Mono engine.
 */

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

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
static cl::opt<std::string>
    AnalysisOpt("analysis",
                cl::desc("Analysis: liveness (default), reachable, "
                         "constant_prop, uninitialized"),
                cl::init("liveness"));

namespace {

using lotus::dataflow_tool::FunctionView;
using lotus::dataflow_tool::ValueIdMap;

mono::DebugConfig quietMonoDebugConfig() {
  mono::DebugConfig Config;
  Config.collect_statistics = false;
  return Config;
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

void runLiveness(raw_ostream &OS, const FunctionView &View) {
  if (auto Res = mono::runLiveVariablesAnalysis(&View.Function,
                                                quietMonoDebugConfig()))
    lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
      lotus::dataflow_tool::formatValueSet(OS, Res->IN(I), View.ValueToId);
    });
}

void runReachable(raw_ostream &OS, const FunctionView &View) {
  if (auto Res =
          mono::runReachableAnalysis(&View.Function, quietMonoDebugConfig()))
    lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
      lotus::dataflow_tool::formatValueSet(OS, Res->IN(I), View.ValueToId);
    });
}

void runConstantPropagation(raw_ostream &OS, const FunctionView &View) {
  auto Res = mono::runIntraMonoConstantPropagation(&View.Function,
                                                   quietMonoDebugConfig());
  lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
    auto It = Res.find(I);
    if (It != Res.end())
      lotus::dataflow_tool::formatValueMap(
          OS, It->second, View.ValueToId,
          [&](const mono::ConstantPropagationValue &Value) {
            return formatMonoConstantValue(Value);
          });
  });
}

void runUninitialized(raw_ostream &OS, const FunctionView &View) {
  if (auto Res = mono::runIntraMonoUninitVariables(&View.Function,
                                                   quietMonoDebugConfig()))
    lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
      lotus::dataflow_tool::formatValueSet(OS, Res->IN(I), View.ValueToId);
    });
}

struct AnalysisHandler final {
  StringRef Name;
  void (*Run)(raw_ostream &, const FunctionView &);
};

const AnalysisHandler Handlers[] = {
    {"liveness", &runLiveness},
    {"reachable", &runReachable},
    {"constant_prop", &runConstantPropagation},
    {"uninitialized", &runUninitialized},
};

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Mono engine testing\n");

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
      StdoutOpt, OutDir, "mono.txt", FileOS, NullOS, EC);
  if (EC) {
    errs() << "error: cannot create " << OutDir << "/mono.txt: " << EC.message()
           << "\n";
    return 1;
  }

  const auto *Handler =
      lotus::dataflow_tool::findHandler(AnalysisOpt, Handlers);
  if (!Handler) {
    errs() << "error: unknown mono analysis '" << AnalysisOpt << "'\n";
    return 1;
  }

  OS << "[mono:" << AnalysisOpt << "]\n";
  lotus::dataflow_tool::forEachDefinedFunction(
      *M, OS, [&](const FunctionView &View) { Handler->Run(OS, View); });

  return 0;
}
