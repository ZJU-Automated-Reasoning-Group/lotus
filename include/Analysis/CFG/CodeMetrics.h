// Bug 17 fix: All function/class definitions and the RegisterPass<> object
// have been moved to CodeMetrics.cpp. This header now contains only
// declarations and inline helpers, so it is safe to include from multiple
// translation units without ODR violations or duplicate-pass-registration
// crashes.
#ifndef ANALYSIS_CFG_CODEMETRICS_H
#define ANALYSIS_CFG_CODEMETRICS_H

/*======================================================================*\
|  "ComplexityMetrics" – one-stop shop for quick-and-dirty metrics       |
|                                                                        |
| - Cyclomatic complexity: Measures the number of independent paths      |
|   through code by counting decision points (if, while, for, case).    |
|   Higher values mean more complex code that's harder to test.          |
|                                                                        |
| - Loop count / max nesting depth: Loop count tracks how many loops     |
|   exist in code. Max nesting depth measures how deeply nested your     |
|   control structures are. Deep nesting makes code hard to maintain.    |
|                                                                        |
| - NPath complexity: Counts the total number of unique execution paths  |
|   through a function, considering all possible combinations of         |
|   branches and loops. Grows exponentially with nested conditions.      |
\*======================================================================*/

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <limits>

// ---------------------------------------------------------------------------
// Plain-data result types
// ---------------------------------------------------------------------------

struct LoopMetrics {
  unsigned NumLoops = 0;
  unsigned MaxDepth = 0;
};

// ---------------------------------------------------------------------------
// Free-function declarations (defined in CodeMetrics.cpp)
// ---------------------------------------------------------------------------

/// Compute McCabe cyclomatic complexity: V(G) = E - N + 2.
unsigned calcCyclomaticComplexity(llvm::Function &F);

/// Collect loop count and maximum nesting depth using LoopInfo.
LoopMetrics collectLoopMetrics(llvm::Function &F, llvm::LoopInfo &LI);

/// Compute NPath complexity (acyclic path count, back-edges skipped).
/// Returns UINT64_MAX when the value saturates.
uint64_t nPath(llvm::Function &F);

// ---------------------------------------------------------------------------
// Legacy pass manager wrapper (registered in CodeMetrics.cpp)
// ---------------------------------------------------------------------------

struct ComplexityLegacy : public llvm::FunctionPass {
  static char ID;
  ComplexityLegacy();

  bool runOnFunction(llvm::Function &F) override;
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  llvm::StringRef getPassName() const override { return "ComplexityLegacy"; }
};

#endif // ANALYSIS_CFG_CODEMETRICS_H
