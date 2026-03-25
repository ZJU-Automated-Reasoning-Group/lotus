#include "Alias/Dynamic/DynamicAliasAnalysis.h"
#include "Alias/Dynamic/IDAssigner.h"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace dynamic;
using namespace llvm;

enum class AAType { BasicAA };

cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<bitcode file>"));
cl::opt<std::string> LogFilename(cl::Positional, cl::desc("<log file>"));
cl::opt<AAType>
    AA(cl::Positional, cl::desc("<alias-analysis>"),
       cl::values(clEnumValN(AAType::BasicAA, "basic-aa", "Basic-AA")));

void checkAAResult(AAResults &aaResult, const DenseSet<AliasPair> &aliasSet,
                   const IDAssigner &idMap) {
  for (auto const &pair : aliasSet) {
    const auto *valA = idMap.getValue(pair.getFirst());
    const auto *valB = idMap.getValue(pair.getSecond());
    if (valA == nullptr || valB == nullptr)
      continue;

    // Create MemoryLocation objects from the values - use MemoryLocation's
    // static method
    auto aliasResult = aaResult.alias(valA, valB);

    if (aliasResult == AliasResult::NoAlias) {
      outs() << "\nFIND AA BUG:\n";
      outs() << "  ValA = " << *valA << '\n';
      outs() << "  ValB = " << *valB << '\n';
      outs() << "  DynamicAA said DidAlias but the tested AA said "
                "NoAlias\n";
    }
  }
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);

  LLVMContext context;
  SMDiagnostic error;
  auto module = parseIRFile(InputFilename, error, context);
  if (!module) {
    error.print(InputFilename.data(), errs());
    return -1;
  }

  // Perform dynamic alias analysis and get all DidAlias pairs
  DynamicAliasAnalysis dynAA(LogFilename.data());
  dynAA.runAnalysis();

  // Set up aa pipeline
  FunctionAnalysisManager funManager;
  ModuleAnalysisManager modManager;

  // Register target library info
  TargetLibraryAnalysis TLI;
  funManager.registerPass([&] { return TLI; });

  AAManager aaManager;
  switch (AA) {
  case AAType::BasicAA:
    aaManager.registerFunctionAnalysis<BasicAA>();
    break;
  default:
    llvm_unreachable("Unknown alias analysis type");
  }

  IDAssigner idMap(*module);
  for (auto &f : *module) {
    if (const auto *id = idMap.getID(f)) {
      if (const auto *aliasSet = dynAA.getAliasPairs(*id)) {
        auto result = aaManager.run(f, funManager);
        checkAAResult(result, *aliasSet, idMap);
      }
    }
  }
}