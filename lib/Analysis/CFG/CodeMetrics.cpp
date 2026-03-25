// Bug 17 fix: definitions moved here from CodeMetrics.h so that the header
// can be included by multiple translation units without ODR violations or
// duplicate RegisterPass<> registrations.

#include "Analysis/CFG/CodeMetrics.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

using namespace llvm;

// ---------------------------------------------------------------------------
// 1. Cyclomatic Complexity
// ---------------------------------------------------------------------------

unsigned calcCyclomaticComplexity(Function &F) {
  unsigned Blocks = 0, Edges = 0;

  for (auto &BB : F) {
    ++Blocks;
    for (auto *Succ : successors(&BB)) {
      (void)Succ;
      ++Edges;
    }
  }

  // Standard McCabe formula: V(G) = E - N + 2  (P == 1 for a single function).
  // Note: call sites are NOT decision points in the standard definition and
  // are therefore not counted here (the old header-only version incorrectly
  // added them).
  return (Edges >= Blocks) ? (2 + Edges - Blocks) : 2;
}

// ---------------------------------------------------------------------------
// 2. Loop count / max nesting depth
// ---------------------------------------------------------------------------

static void scanLoop(const Loop *L, unsigned Depth, LoopMetrics &M) {
  ++M.NumLoops;
  M.MaxDepth = std::max(M.MaxDepth, Depth);
  for (auto *Child : L->getSubLoops())
    scanLoop(Child, Depth + 1, M);
}

LoopMetrics collectLoopMetrics(Function &F, LoopInfo &LI) {
  (void)F;
  LoopMetrics M;
  for (auto *Top : LI)
    scanLoop(Top, 1, M);
  return M;
}

// ---------------------------------------------------------------------------
// 3. NPath complexity
//
// Uses an iterative topological-order traversal with back-edge skipping to
// avoid infinite recursion on loops (old recursive version had no cycle
// detection). Uses saturating uint64_t addition to avoid silent overflow.
// ---------------------------------------------------------------------------

/// Saturating addition for uint64_t — returns UINT64_MAX on overflow.
static inline uint64_t sat_add(uint64_t a, uint64_t b) {
  if (b > std::numeric_limits<uint64_t>::max() - a)
    return std::numeric_limits<uint64_t>::max();
  return a + b;
}

uint64_t nPath(Function &F) {
  if (F.empty())
    return 0;

  // Collect back-edges so we can skip them during traversal.
  SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 8> backEdges;
  FindFunctionBackedges(F, backEdges);
  std::sort(backEdges.begin(), backEdges.end());

  auto isBackEdge = [&](const BasicBlock *Src, const BasicBlock *Dst) -> bool {
    return std::binary_search(backEdges.begin(), backEdges.end(),
                              std::make_pair(Src, Dst));
  };

  // paths[BB] = number of acyclic paths from BB to any exit, ignoring
  // back-edges.  Computed in reverse RPO (exits first).
  DenseMap<const BasicBlock *, uint64_t> paths;

  ReversePostOrderTraversal<const Function *> RPOT(&F);
  std::vector<const BasicBlock *> order(RPOT.begin(), RPOT.end());

  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const BasicBlock *BB = *it;
    uint64_t sum = 0;
    bool hasNonBackSucc = false;
    for (const BasicBlock *Succ : successors(BB)) {
      if (isBackEdge(BB, Succ))
        continue;
      hasNonBackSucc = true;
      auto it2 = paths.find(Succ);
      uint64_t succPaths = (it2 != paths.end()) ? it2->second : 0;
      sum = sat_add(sum, succPaths);
    }
    // A block with no non-back-edge successors is an exit: counts as 1 path.
    paths[BB] = hasNonBackSucc ? sum : 1;
  }

  return paths[&F.getEntryBlock()];
}

// ---------------------------------------------------------------------------
// Legacy pass manager wrapper
// ---------------------------------------------------------------------------

ComplexityLegacy::ComplexityLegacy() : FunctionPass(ID) {}

bool ComplexityLegacy::runOnFunction(Function &F) {
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto CC = calcCyclomaticComplexity(F);
  auto LM = collectLoopMetrics(F, LI);
  auto NP = nPath(F);

  errs() << "== " << F.getName() << " ==\n"
         << "  Cyclomatic    : " << CC << '\n'
         << "  NPath         : ";
  if (NP == std::numeric_limits<uint64_t>::max())
    errs() << ">= UINT64_MAX (saturated)\n";
  else
    errs() << NP << '\n';
  errs() << "  Loops         : " << LM.NumLoops << "  (max depth "
         << LM.MaxDepth << ")\n";

  return false; // analysis pass — does not modify IR
}

void ComplexityLegacy::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addPreserved<LoopInfoWrapperPass>();
}

// Bug 17 fix: char ID and RegisterPass<> are defined exactly once here in the
// .cpp file. Previously they were in the header, causing one definition per
// including TU — an ODR violation and a duplicate-registration crash.
char ComplexityLegacy::ID = 0;

static RegisterPass<ComplexityLegacy> X("complexity-legacy",
                                        "Complexity metrics (legacy PM)",
                                        /*cfgOnly=*/false,
                                        /*is_analysis=*/true);
