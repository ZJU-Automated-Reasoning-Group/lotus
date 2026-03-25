// SymAbsAI reporting helpers: configuration and analysis result
// rendering
#include "Verification/SymAbsAI/Utils/Reporting.h"

#include "Verification/SymAbsAI/Analyzers/Analyzer.h"
#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace symabs_ai;

void printEffectiveConfiguration(
    const std::string &configSource, const std::string &domainName,
    const std::string &domainSource, bool fallbackToFirst,
    const std::string &fragmentStrategy, const std::string &fragmentOrigin,
    const std::string &analyzerVariant, bool incremental, int wideningDelay,
    int wideningFrequency, const std::string &wideningOrigin,
    const std::string &memoryVariant, int addressBits,
    const std::string &memoryOrigin) {
  outs() << "Effective configuration:\n";
  outs() << "  Config source: " << configSource << "\n";
  outs() << "  Abstract domain (" << domainSource;
  if (fallbackToFirst)
    outs() << ", fallback";
  if (domainSource == "built-in defaults")
    outs() << ", default";
  outs() << "): " << domainName << "\n";
  outs() << "  Fragment strategy: " << fragmentStrategy << " ("
         << fragmentOrigin << ")\n";
  outs() << "  Analyzer: " << analyzerVariant
         << (incremental ? " [incremental]" : " [non-incremental]") << "\n";
  outs() << "  Widening delay/frequency: " << wideningDelay << "/"
         << wideningFrequency << " (" << wideningOrigin << ")\n";
  outs() << "  Memory model: " << memoryVariant;
  if (addressBits >= 0)
    outs() << " (address bits=" << addressBits << ")";
  outs() << " (" << memoryOrigin << ")\n\n";
}

void printEntryResult(Analyzer *analyzer, Function *func) {
  auto *entryResult = analyzer->at(&func->getEntryBlock());
  PrettyPrinter entryPp(true);
  entryResult->prettyPrint(entryPp);
  outs() << "\nAnalysis result at entry:\n" << entryPp.str() << "\n";
}

void printAllBlocksResults(Analyzer *analyzer, Function *func) {
  outs() << "\nAnalysis results for all basic blocks:\n";
  for (auto &BB : *func) {
    outs() << "\n--- Basic block: " << BB.getName() << " ---\n";
    auto *atResult = analyzer->at(&BB);
    PrettyPrinter atPp(true);
    atResult->prettyPrint(atPp);
    outs() << "At beginning:\n" << atPp.str() << "\n";

    auto *afterResult = analyzer->after(&BB);
    PrettyPrinter afterPp(true);
    afterResult->prettyPrint(afterPp);
    outs() << "After execution:\n" << afterPp.str() << "\n";
  }
}

void printExitBlocksResults(Analyzer *analyzer, Function *func) {
  outs() << "\nAnalysis results at exit blocks:\n";
  for (auto &BB : *func) {
    if (llvm::isa<llvm::ReturnInst>(BB.getTerminator())) {
      outs() << "\n--- Exit block: " << BB.getName() << " ---\n";
      auto *exitResult = analyzer->after(&BB);
      PrettyPrinter exitPp(true);
      exitResult->prettyPrint(exitPp);
      outs() << exitPp.str() << "\n";
    }
  }
}
