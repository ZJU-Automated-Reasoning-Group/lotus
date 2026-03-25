/*
 * lotus-dfa-ifds
 *
 * Dataflow testing tool: IFDS engine.
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

#include "Dataflow/IFDS/Clients/IFDSReachingDefinitions.h"
#include "Dataflow/IFDS/Clients/IFDSUninitializedVariables.h"
#include "Dataflow/IFDS/Solvers/IFDSSolver.h"

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
                cl::desc("Analysis: reaching_defs (default), uninitialized"),
                cl::init("reaching_defs"));

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

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "IFDS engine testing\n");

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
    FileOS = std::make_unique<raw_fd_ostream>(OutDir + "/ifds.txt", EC);
    if (EC) {
      errs() << "error: cannot create " << OutDir
             << "/ifds.txt: " << EC.message() << "\n";
      return 1;
    }
    OutOS = FileOS.get();
  }
  raw_ostream &OS = *OutOS;

  OS << "[ifds:" << AnalysisOpt << "]\n";

  for (auto &F : *M) {
    if (F.isDeclaration())
      continue;

    std::unordered_map<const Value *, std::string> ValueToId;
    std::vector<Instruction *> OrderedInsts;
    buildValueIds(&F, ValueToId, OrderedInsts);
    OS << "FUNC " << F.getName().str() << "\n";

    if (AnalysisOpt == "reaching_defs") {
      ifds::ReachingDefinitionsAnalysis problem;
      ifds::IFDSSolver<ifds::ReachingDefinitionsAnalysis> solver(problem);
      solver.solve(*M);
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
    } else if (AnalysisOpt == "uninitialized") {
      ifds::UninitializedVariablesAnalysis problem;
      ifds::IFDSSolver<ifds::UninitializedVariablesAnalysis> solver(problem);
      solver.solve(*M);
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
  }

  return 0;
}
