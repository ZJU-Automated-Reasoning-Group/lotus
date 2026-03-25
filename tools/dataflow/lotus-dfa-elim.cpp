/*
 * lotus-dfa-elim
 *
 * Dataflow testing tool: Elimination engine.
 */

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"

#include "Dataflow/APA/Clients/LLVM/Intra/AvailableExpressions.h"
#include "Dataflow/APA/Clients/LLVM/Intra/ConstantPropagation.h"
#include "Dataflow/APA/Clients/LLVM/Intra/LiveVariables.h"
#include "Dataflow/APA/Clients/LLVM/Intra/Reachability.h"
#include "Dataflow/APA/Clients/LLVM/Intra/ReachingDefinitions.h"
#include "Dataflow/APA/Clients/LLVM/Intra/UninitializedVariables.h"

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
static cl::opt<std::string> AnalysisOpt(
    "analysis",
    cl::desc("Analysis: liveness (default), reaching_defs, uninitialized, "
             "constant_prop, available_exprs, reachable"),
    cl::init("liveness"));
static cl::opt<std::string> ElimMethodOpt(
    "elim-method",
    cl::desc("Elimination solver method: state|adt-simple|adt-delayed"),
    cl::init("state"));

namespace {

elimination::EliminationOptions getElimOptions() {
  elimination::EliminationOptions Opts;
  if (ElimMethodOpt == "adt-simple")
    Opts.Method = elimination::EliminationMethod::ADTSimple;
  else if (ElimMethodOpt == "adt-delayed")
    Opts.Method = elimination::EliminationMethod::ADTDelayed;
  else
    Opts.Method = elimination::EliminationMethod::StateElimination;
  return Opts;
}

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

template <typename ValueType>
void formatConstPropMap(
    raw_ostream &OS, const std::unordered_map<const Value *, ValueType> &M,
    const std::unordered_map<const Value *, std::string> &ValueToId) {
  std::vector<std::string> entries;
  for (const auto &p : M) {
    std::ostringstream ss;
    auto It = ValueToId.find(p.first);
    ss << (It != ValueToId.end() ? It->second : "v") << "=";
    ss << formatValueLatticeElement(p.second);
    entries.push_back(ss.str());
  }
  std::sort(entries.begin(), entries.end());
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i)
      OS << ",";
    OS << entries[i];
  }
}

void dumpFunctionAnalysis(raw_ostream &OS, Function &F,
                          const std::string &Analysis,
                          const elimination::EliminationOptions &ElimOpts) {
  std::unordered_map<const Value *, std::string> ValueToId;
  std::vector<Instruction *> OrderedInsts;
  buildValueIds(&F, ValueToId, OrderedInsts);
  OS << "FUNC " << F.getName().str() << "\n";

  if (Analysis == "liveness") {
    auto Result = elimination::runIntraElimLiveVariables(&F, ElimOpts);
    for (auto *I : OrderedInsts) {
      OS << "  " << ValueToId.at(I) << " IN: ";
      formatValueSet(OS, Result.IN(I), ValueToId);
      OS << "\n";
    }
  } else if (Analysis == "reaching_defs") {
    auto Result =
        elimination::runIntraElimReachingDefinitions(&F, nullptr, ElimOpts);
    for (auto *I : OrderedInsts) {
      OS << "  " << ValueToId.at(I) << " IN: ";
      formatValueSet(OS, Result.IN(I), ValueToId);
      OS << "\n";
    }
  } else if (Analysis == "uninitialized") {
    auto Result =
        elimination::runIntraElimUninitVariables(&F, nullptr, ElimOpts);
    for (auto *I : OrderedInsts) {
      OS << "  " << ValueToId.at(I) << " IN: ";
      formatValueSet(OS, Result.IN(I), ValueToId);
      OS << "\n";
    }
  } else if (Analysis == "constant_prop") {
    auto Result =
        elimination::runIntraElimConstantPropagation(&F, nullptr, ElimOpts);
    for (auto *I : OrderedInsts) {
      OS << "  " << ValueToId.at(I) << " IN: ";
      formatConstPropMap(OS, Result.IN(I), ValueToId);
      OS << "\n";
    }
  } else if (Analysis == "available_exprs") {
    auto Result =
        elimination::runIntraElimAvailableExpressions(&F, nullptr, ElimOpts);
    for (auto *I : OrderedInsts) {
      OS << "  " << ValueToId.at(I) << " IN: ";
      std::vector<std::string> exprs;
      for (const auto &expr : Result.IN(I))
        exprs.push_back(formatExpressionKey(expr));
      std::sort(exprs.begin(), exprs.end());
      for (size_t i = 0; i < exprs.size(); ++i) {
        if (i)
          OS << ",";
        OS << exprs[i];
      }
      OS << "\n";
    }
  } else if (Analysis == "reachable") {
    auto Result = elimination::runIntraElimReachable(&F, ElimOpts);
    for (auto *I : OrderedInsts) {
      OS << "  " << ValueToId.at(I) << " IN: ";
      OS << (Result.IN(I) ? "true" : "false");
      OS << "\n";
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Elimination engine testing\n");

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
    FileOS = std::make_unique<raw_fd_ostream>(OutDir + "/elim.txt", EC);
    if (EC) {
      errs() << "error: cannot create " << OutDir
             << "/elim.txt: " << EC.message() << "\n";
      return 1;
    }
    OutOS = FileOS.get();
  }
  raw_ostream &OS = *OutOS;
  const auto ElimOpts = getElimOptions();

  OS << "[elim:" << AnalysisOpt << "]\n";

  for (auto &F : *M)
    if (!F.isDeclaration())
      dumpFunctionAnalysis(OS, F, AnalysisOpt, ElimOpts);

  return 0;
}
