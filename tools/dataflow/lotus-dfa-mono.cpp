/*
 * lotus-dfa-mono
 *
 * Dataflow testing tool: Mono engine.
 */

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"

#include "Dataflow/Mono/Analyses/Intra/IntraConstantPropagation.h"
#include "Dataflow/Mono/Analyses/Intra/IntraLiveVariables.h"
#include "Dataflow/Mono/Analyses/Intra/IntraReachable.h"
#include "Dataflow/Mono/Analyses/Intra/IntraUninitVariables.h"

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
static cl::opt<std::string>
    AnalysisOpt("analysis",
                cl::desc("Analysis: liveness (default), reachable, "
                         "constant_prop, uninitialized"),
                cl::init("liveness"));

namespace {

void buildValueIds(Function *F,
                   std::unordered_map<const Value *, std::string> &ValueToId,
                   std::vector<Instruction *> &OrderedInsts) {
  unsigned ArgIdx = 0;
  for (auto &Arg : F->args())
    ValueToId[&Arg] = "arg" + std::to_string(ArgIdx++);
  unsigned InstIdx = 0;
  for (auto &BB : *F)
    for (auto &I : BB) {
      OrderedInsts.push_back(&I);
      ValueToId[&I] = "i" + std::to_string(InstIdx++);
    }
}

template <typename T>
void formatValueSet(
    raw_ostream &OS, const std::set<T> &S,
    const std::unordered_map<const Value *, std::string> &ValueToId) {
  std::vector<std::string> ids;
  for (const Value *V : S) {
    auto It = ValueToId.find(V);
    if (It != ValueToId.end())
      ids.push_back(It->second);
  }
  std::sort(ids.begin(), ids.end());
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i)
      OS << ",";
    OS << ids[i];
  }
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

template <typename ValueType>
void formatConstPropMap(
    raw_ostream &OS, const std::unordered_map<const Value *, ValueType> &M,
    const std::unordered_map<const Value *, std::string> &ValueToId) {
  std::vector<std::string> entries;
  for (const auto &p : M) {
    std::ostringstream ss;
    auto It = ValueToId.find(p.first);
    ss << (It != ValueToId.end() ? It->second : "v") << "=";
    ss << formatMonoConstantValue(p.second);
    entries.push_back(ss.str());
  }
  std::sort(entries.begin(), entries.end());
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i)
      OS << ",";
    OS << entries[i];
  }
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Mono engine testing\n");

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  legacy::PassManager PM;
  PM.add(createPromoteMemoryToRegisterPass());
  PM.add(createInstructionNamerPass());
  PM.run(*M);

  raw_ostream *OutOS = &outs();
  std::unique_ptr<raw_fd_ostream> FileOS;
  if (!OutDir.empty()) {
    std::error_code EC;
    FileOS = std::make_unique<raw_fd_ostream>(OutDir + "/mono.txt", EC);
    if (EC) {
      errs() << "error: cannot create " << OutDir
             << "/mono.txt: " << EC.message() << "\n";
      return 1;
    }
    OutOS = FileOS.get();
  }
  raw_ostream &OS = *OutOS;

  OS << "[mono:" << AnalysisOpt << "]\n";

  for (auto &F : *M) {
    if (F.isDeclaration())
      continue;

    std::unordered_map<const Value *, std::string> ValueToId;
    std::vector<Instruction *> OrderedInsts;
    buildValueIds(&F, ValueToId, OrderedInsts);
    OS << "FUNC " << F.getName().str() << "\n";

    if (AnalysisOpt == "liveness") {
      auto Res = mono::runLiveVariablesAnalysis(&F);
      if (Res)
        for (auto *I : OrderedInsts) {
          OS << "  " << ValueToId.at(I) << " IN: ";
          formatValueSet(OS, Res->IN(I), ValueToId);
          OS << "\n";
        }
    } else if (AnalysisOpt == "reachable") {
      auto Res = mono::runReachableAnalysis(&F);
      if (Res)
        for (auto *I : OrderedInsts) {
          OS << "  " << ValueToId.at(I) << " IN: ";
          formatValueSet(OS, Res->IN(I), ValueToId);
          OS << "\n";
        }
    } else if (AnalysisOpt == "constant_prop") {
      auto Res = mono::runIntraMonoConstantPropagation(&F);
      for (auto *I : OrderedInsts) {
        OS << "  " << ValueToId.at(I) << " IN: ";
        auto It = Res.find(I);
        if (It != Res.end())
          formatConstPropMap(OS, It->second, ValueToId);
        OS << "\n";
      }
    } else if (AnalysisOpt == "uninitialized") {
      auto Res = mono::runIntraMonoUninitVariables(&F);
      if (Res)
        for (auto *I : OrderedInsts) {
          OS << "  " << ValueToId.at(I) << " IN: ";
          formatValueSet(OS, Res->IN(I), ValueToId);
          OS << "\n";
        }
    }
  }

  return 0;
}
