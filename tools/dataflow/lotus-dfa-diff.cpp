/*
 * lotus-dfa-diff
 *
 * Differential testing for lib/Dataflow engines.
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
#include "Dataflow/IFDS/Clients/IFDSReachingDefinitions.h"
#include "Dataflow/IFDS/Clients/IFDSUninitializedVariables.h"
#include "Dataflow/IFDS/Solvers/IFDSSolver.h"
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

void formatElimConstPropMap(
    raw_ostream &OS,
    const std::unordered_map<const Value *, ValueLatticeElement> &M,
    const std::unordered_map<const Value *, std::string> &ValueToId) {
  std::vector<std::string> entries;
  for (const auto &p : M) {
    std::ostringstream ss;
    auto It = ValueToId.find(p.first);
    ss << (It != ValueToId.end() ? It->second : "v") << "="
       << formatValueLatticeElement(p.second);
    entries.push_back(ss.str());
  }
  std::sort(entries.begin(), entries.end());
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i)
      OS << ",";
    OS << entries[i];
  }
}

void formatMonoConstPropMap(
    raw_ostream &OS,
    const std::unordered_map<const Value *, mono::ConstantPropagationValue> &M,
    const std::unordered_map<const Value *, std::string> &ValueToId) {
  std::vector<std::string> entries;
  for (const auto &p : M) {
    std::ostringstream ss;
    auto It = ValueToId.find(p.first);
    ss << (It != ValueToId.end() ? It->second : "v") << "="
       << formatMonoConstantValue(p.second);
    entries.push_back(ss.str());
  }
  std::sort(entries.begin(), entries.end());
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i)
      OS << ",";
    OS << entries[i];
  }
}

std::string formatIFDSFact(
    const ifds::DefinitionFact &fact,
    const std::unordered_map<const Value *, std::string> &ValueToId) {
  if (fact.is_zero())
    return "zero";
  std::ostringstream ss;
  auto varIt = ValueToId.find(fact.get_variable());
  auto defIt = ValueToId.find(fact.get_definition_site());
  ss << "def(" << (varIt != ValueToId.end() ? varIt->second : "v") << ","
     << (defIt != ValueToId.end() ? defIt->second : "i") << ")";
  return ss.str();
}

std::string formatIFDSFact(
    const ifds::UninitVarFact &fact,
    const std::unordered_map<const Value *, std::string> &ValueToId) {
  if (fact.is_zero())
    return "zero";
  std::ostringstream ss;
  auto It = ValueToId.find(fact.value);
  ss << (fact.is_uninitialized() ? "uninit(" : "init(")
     << (It != ValueToId.end() ? It->second : "v") << ")";
  return ss.str();
}

template <typename Fact>
void formatIFDSFactSet(
    raw_ostream &OS, const std::set<Fact> &facts,
    const std::unordered_map<const Value *, std::string> &ValueToId) {
  std::vector<std::string> formatted;
  for (const auto &fact : facts)
    formatted.push_back(formatIFDSFact(fact, ValueToId));
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
  std::unique_ptr<raw_fd_ostream> elim_out, mono_out, ifds_out;

public:
  raw_ostream &getStream(const std::string &Engine) {
    auto open = [&](auto &out, const char *name) -> raw_ostream & {
      if (!out) {
        std::error_code EC;
        out = std::make_unique<raw_fd_ostream>(
            OutDir.empty() ? "" : OutDir + "/" + name, EC);
        if (EC)
          errs() << "warning: cannot create " << name << ": " << EC.message()
                 << "\n";
      }
      return *out;
    };
    if (Engine == "elim")
      return open(elim_out, "elim.txt");
    if (Engine == "mono")
      return open(mono_out, "mono.txt");
    if (Engine == "ifds")
      return open(ifds_out, "ifds.txt");
    return outs();
  }
  void close() {
    if (elim_out)
      elim_out->close();
    if (mono_out)
      mono_out->close();
    if (ifds_out)
      ifds_out->close();
  }
};

void runEliminationAnalysis(Module &M, const std::string &AnalysisName,
                            OutputManager &OutMgr,
                            const elimination::EliminationOptions &ElimOpts) {
  raw_ostream &OS = OutMgr.getStream("elim");
  bool firstFunc = true;
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    std::unordered_map<const Value *, std::string> ValueToId;
    std::vector<Instruction *> OrderedInsts;
    buildValueIds(&F, ValueToId, OrderedInsts);
    if (OutDir.empty() && firstFunc) {
      OS << "[elim:" << AnalysisName << "]\n";
      firstFunc = false;
    }
    OS << "FUNC " << F.getName().str() << "\n";

    if (AnalysisName == "liveness") {
      auto Res = elimination::runIntraElimLiveVariables(&F, ElimOpts);
      for (auto *I : OrderedInsts) {
        OS << "  " << ValueToId.at(I) << " IN: ";
        formatValueSet(OS, Res.IN(I), ValueToId);
        OS << "\n";
      }
    } else if (AnalysisName == "reaching_defs") {
      auto Res =
          elimination::runIntraElimReachingDefinitions(&F, nullptr, ElimOpts);
      for (auto *I : OrderedInsts) {
        OS << "  " << ValueToId.at(I) << " IN: ";
        formatValueSet(OS, Res.IN(I), ValueToId);
        OS << "\n";
      }
    } else if (AnalysisName == "uninitialized") {
      auto Res =
          elimination::runIntraElimUninitVariables(&F, nullptr, ElimOpts);
      for (auto *I : OrderedInsts) {
        OS << "  " << ValueToId.at(I) << " IN: ";
        formatValueSet(OS, Res.IN(I), ValueToId);
        OS << "\n";
      }
    } else if (AnalysisName == "constant_prop") {
      auto Res =
          elimination::runIntraElimConstantPropagation(&F, nullptr, ElimOpts);
      for (auto *I : OrderedInsts) {
        OS << "  " << ValueToId.at(I) << " IN: ";
        formatElimConstPropMap(OS, Res.IN(I), ValueToId);
        OS << "\n";
      }
    } else if (AnalysisName == "available_exprs") {
      auto Res =
          elimination::runIntraElimAvailableExpressions(&F, nullptr, ElimOpts);
      for (auto *I : OrderedInsts) {
        OS << "  " << ValueToId.at(I) << " IN: ";
        std::vector<std::string> exprs;
        for (const auto &expr : Res.IN(I))
          exprs.push_back(formatExpressionKey(expr));
        std::sort(exprs.begin(), exprs.end());
        for (size_t i = 0; i < exprs.size(); ++i) {
          if (i)
            OS << ",";
          OS << exprs[i];
        }
        OS << "\n";
      }
    } else if (AnalysisName == "reachable") {
      auto Res = elimination::runIntraElimReachable(&F, ElimOpts);
      for (auto *I : OrderedInsts) {
        OS << "  " << ValueToId.at(I)
           << " IN: " << (Res.IN(I) ? "true" : "false") << "\n";
      }
    }
  }
}

void runMonoAnalysis(Module &M, const std::string &AnalysisName,
                     OutputManager &OutMgr) {
  raw_ostream &OS = OutMgr.getStream("mono");
  bool firstFunc = true;
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    std::unordered_map<const Value *, std::string> ValueToId;
    std::vector<Instruction *> OrderedInsts;
    buildValueIds(&F, ValueToId, OrderedInsts);
    if (OutDir.empty() && firstFunc) {
      OS << "[mono:" << AnalysisName << "]\n";
      firstFunc = false;
    }
    OS << "FUNC " << F.getName().str() << "\n";

    if (AnalysisName == "liveness") {
      auto Res = mono::runLiveVariablesAnalysis(&F);
      if (Res)
        for (auto *I : OrderedInsts) {
          OS << "  " << ValueToId.at(I) << " IN: ";
          formatValueSet(OS, Res->IN(I), ValueToId);
          OS << "\n";
        }
    } else if (AnalysisName == "reachable") {
      auto Res = mono::runReachableAnalysis(&F);
      if (Res)
        for (auto *I : OrderedInsts) {
          OS << "  " << ValueToId.at(I) << " IN: ";
          formatValueSet(OS, Res->IN(I), ValueToId);
          OS << "\n";
        }
    } else if (AnalysisName == "constant_prop") {
      auto Res = mono::runIntraMonoConstantPropagation(&F);
      for (auto *I : OrderedInsts) {
        OS << "  " << ValueToId.at(I) << " IN: ";
        auto It = Res.find(I);
        if (It != Res.end())
          formatMonoConstPropMap(OS, It->second, ValueToId);
        OS << "\n";
      }
    } else if (AnalysisName == "uninitialized") {
      auto Res = mono::runIntraMonoUninitVariables(&F);
      if (Res)
        for (auto *I : OrderedInsts) {
          OS << "  " << ValueToId.at(I) << " IN: ";
          formatValueSet(OS, Res->IN(I), ValueToId);
          OS << "\n";
        }
    }
  }
}

void runIFDSAnalysis(Module &M, const std::string &AnalysisName,
                     OutputManager &OutMgr) {
  raw_ostream &OS = OutMgr.getStream("ifds");
  bool firstFunc = true;

  if (AnalysisName == "reaching_defs") {
    for (auto &F : M) {
      if (F.isDeclaration())
        continue;
      std::unordered_map<const Value *, std::string> ValueToId;
      std::vector<Instruction *> OrderedInsts;
      buildValueIds(&F, ValueToId, OrderedInsts);
      if (OutDir.empty() && firstFunc) {
        OS << "[ifds:" << AnalysisName << "]\n";
        firstFunc = false;
      }
      OS << "FUNC " << F.getName().str() << "\n";

      ifds::ReachingDefinitionsAnalysis problem;
      ifds::IFDSSolver<ifds::ReachingDefinitionsAnalysis> solver(problem);
      solver.solve(M);
      auto allResults = solver.get_all_results();
      for (auto *I : OrderedInsts) {
        const Instruction *nextInst = getNextInstruction(I);
        OS << "  " << ValueToId.at(I) << " IN: ";
        if (nextInst) {
          auto node = ifds::ExplodedSupergraph<ifds::DefinitionFact>::Node(
              nextInst, ifds::DefinitionFact::zero());
          auto It = allResults.find(node);
          if (It != allResults.end())
            formatIFDSFactSet(OS, It->second, ValueToId);
        }
        OS << "\n";
      }
    }
  } else if (AnalysisName == "uninitialized") {
    for (auto &F : M) {
      if (F.isDeclaration())
        continue;
      std::unordered_map<const Value *, std::string> ValueToId;
      std::vector<Instruction *> OrderedInsts;
      buildValueIds(&F, ValueToId, OrderedInsts);
      if (OutDir.empty() && firstFunc) {
        OS << "[ifds:" << AnalysisName << "]\n";
        firstFunc = false;
      }
      OS << "FUNC " << F.getName().str() << "\n";

      ifds::UninitializedVariablesAnalysis problem;
      ifds::IFDSSolver<ifds::UninitializedVariablesAnalysis> solver(problem);
      solver.solve(M);
      auto allResults = solver.get_all_results();
      for (auto *I : OrderedInsts) {
        const Instruction *nextInst = getNextInstruction(I);
        OS << "  " << ValueToId.at(I) << " IN: ";
        if (nextInst) {
          auto node = ifds::ExplodedSupergraph<ifds::UninitVarFact>::Node(
              nextInst, ifds::UninitVarFact::zero());
          auto It = allResults.find(node);
          if (It != allResults.end())
            formatIFDSFactSet(OS, It->second, ValueToId);
        }
        OS << "\n";
      }
    }
  } else {
    if (OutDir.empty() && firstFunc)
      OS << "[ifds:" << AnalysisName << "]\n  (not implemented for IFDS)\n";
  }
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Dataflow engine diff testing\n");

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

  OutputManager OutMgr;
  const auto ElimOpts = getElimOptions();
  bool runElim = (EngineOpt == "elim" || EngineOpt == "all");
  bool runMono = (EngineOpt == "mono" || EngineOpt == "all");
  bool runIFDS = (EngineOpt == "ifds" || EngineOpt == "all");

  if (runElim)
    runEliminationAnalysis(*M, AnalysisOpt, OutMgr, ElimOpts);
  if (runMono)
    runMonoAnalysis(*M, AnalysisOpt, OutMgr);
  if (runIFDS)
    runIFDSAnalysis(*M, AnalysisOpt, OutMgr);

  OutMgr.close();
  return 0;
}
