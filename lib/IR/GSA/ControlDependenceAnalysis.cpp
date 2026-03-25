//===- ControlDependenceAnalysis.cpp - Control dependence for GSA ---------===//
//
// Constructs block-level control dependence information required by the Gated
// SSA (GSA) transformation. The implementation follows the classical
// post-dominance frontier algorithm described in
//   Ferrante, Ottenstein, Warren: "The Program Dependence Graph and its Uses",
//   ACM TOPLAS, 1987.
//
// Author: rainoftime
//
// ===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"

#include "IR/GSA/GSA.h"
// #include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using namespace llvm;

namespace gsa {

namespace {

class ControlDependenceAnalysisImpl : public ControlDependenceAnalysis {
  Function &m_function;

  // Maps basic blocks to ids in a reverse-topological linearization.
  DenseMap<const BasicBlock *, unsigned> m_BBToIdx;
  // List of basic blocks in reverse-topological order.
  std::vector<const BasicBlock *> m_postOrderBlocks;
  // Map from a basic block to all reachable blocks.
  mutable DenseMap<const BasicBlock *, SparseBitVector<>> m_reach;
  // Maps a basic block to the blocks it is control dependent on.
  DenseMap<const BasicBlock *, SmallVector<BasicBlock *, 4>> m_cdInfo;
  SmallVector<BasicBlock *, 0> m_empty;

public:
  ControlDependenceAnalysisImpl(Function &f, PostDominatorTree &pdt)
      : m_function(f) {
    initReach();
    calculate(pdt);
  }

  bool isTracked(const BasicBlock &BB) const override {
    return m_BBToIdx.count(&BB) > 0;
  }

  ArrayRef<BasicBlock *> getCDBlocks(BasicBlock *BB) const override {
    if (BB == nullptr || !isTracked(*BB))
      return m_empty;
    auto it = m_cdInfo.find(BB);
    return it != m_cdInfo.end() ? ArrayRef<BasicBlock *>(it->second) : m_empty;
  }

  bool isReachable(BasicBlock *Src, BasicBlock *Dst) const override {
    // Unreachable blocks (absent from m_BBToIdx / m_reach because post_order
    // never visited them) can never be the source of a CFG path, and are
    // never the destination of a path from a reachable block.
    auto reachIt = m_reach.find(Src);
    if (reachIt == m_reach.end())
      return false; // Src not reachable from entry
    auto dstIdxIt = m_BBToIdx.find(Dst);
    if (dstIdxIt == m_BBToIdx.end())
      return false; // Dst not reachable from entry; Src cannot reach it
    return reachIt->second.test(dstIdxIt->second);
  }

  unsigned getBBTopoIdx(BasicBlock *BB) const override {
    auto it = m_BBToIdx.find(BB);
    assert(it != m_BBToIdx.end() && "Topology queried for untracked block");
    // m_BBToIdx supplies post-order numbers; reverse gives topo order.
    return m_BBToIdx.size() - it->second;
  }

private:
  void calculate(PostDominatorTree &PDT);
  void initReach();
};

void ControlDependenceAnalysisImpl::calculate(PostDominatorTree &PDT) {
  // Compute control dependences using the classic post-dominance test:
  // A is control dependent on B if B has a successor S not post-dominated by B
  // and A lies on the path from S up to but not including B in the postdom
  // tree.
  DenseMap<const BasicBlock *, DenseSet<const BasicBlock *>>
      dependentOn; // reverse mapping for deduplication.

  for (BasicBlock &BB : m_function) {
    auto *BBNode = PDT.getNode(&BB);
    if (!BBNode)
      continue; // Unreachable blocks.

    for (BasicBlock *Succ : successors(&BB)) {
      // If BB postdominates the successor, it does not create a control
      // dependence.
      if (PDT.dominates(&BB, Succ))
        continue;

      // Walk up the post-dominator tree from the successor until (and
      // excluding) BB, adding control dependences.
      auto *SuccNode = PDT.getNode(Succ);
      while (SuccNode && SuccNode != BBNode) {
        BasicBlock *Curr = SuccNode->getBlock();
        dependentOn[Curr].insert(&BB);
        SuccNode = SuccNode->getIDom();
      }
    }
  }

  // Materialize the final map with deterministic ordering.
  for (BasicBlock &BB : m_function) {
    auto &Vec = m_cdInfo[&BB];
    auto It = dependentOn.find(&BB);
    if (It != dependentOn.end()) {
      Vec.clear();
      Vec.reserve(It->second.size());
      for (const BasicBlock *BBPtr : It->second) {
        Vec.push_back(const_cast<BasicBlock *>(BBPtr));
      }
      // Use lookup() instead of operator[] to avoid silently inserting a
      // default 0 entry for any block not yet present in the map, which
      // would corrupt subsequent index lookups.
      std::sort(Vec.begin(), Vec.end(),
                [this](const BasicBlock *first, const BasicBlock *second) {
                  return m_BBToIdx.lookup(first) < m_BBToIdx.lookup(second);
                });
      Vec.erase(std::unique(Vec.begin(), Vec.end()), Vec.end());
    }
  }
}

void ControlDependenceAnalysisImpl::initReach() {
  m_postOrderBlocks.reserve(m_function.size());
  unsigned num = 0;
  for (BasicBlock *BB : llvm::post_order(&m_function.getEntryBlock())) {
    m_postOrderBlocks.push_back(BB);
    m_BBToIdx[BB] = num;
    m_reach[BB].set(num);
    ++num;
  }

  // Cache predecessors to avoid linear walks over terminators.
  // IMPORTANT: iterate only over *reachable* blocks (m_postOrderBlocks),
  // not over m_function.  Unreachable blocks are absent from m_BBToIdx; if
  // we used m_function here, DenseMap::operator[] would silently insert a
  // default index (0) for any unmapped successor, corrupting the index map
  // and producing wrong reachability results for all blocks with index 0.
  std::vector<DenseSet<BasicBlock *>> inverseSuccessors(m_BBToIdx.size());

  for (auto *BB : m_postOrderBlocks)
    for (auto *succ : successors(BB)) {
      // All successors of reachable blocks are themselves reachable and
      // therefore present in m_BBToIdx — no bounds check needed.
      m_reach[BB].set(m_BBToIdx[succ]);
      inverseSuccessors[m_BBToIdx[succ]].insert(const_cast<BasicBlock *>(BB));
    }

  // Propagate reachability backwards until a fixed point.
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto *BB : m_postOrderBlocks) {
      auto &currReach = m_reach[BB];
      for (BasicBlock *pred : inverseSuccessors[m_BBToIdx[BB]]) {
        auto &predReach = m_reach[pred];
        const size_t initSize = predReach.count();
        predReach |= currReach;
        if (predReach.count() != initSize)
          changed = true;
      }
    }
  }
}

} // namespace

char ControlDependenceAnalysisPass::ID = 0;

ControlDependenceAnalysisPass::ControlDependenceAnalysisPass()
    : ModulePass(ID) {}

void ControlDependenceAnalysisPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<PostDominatorTreeWrapperPass>();
  AU.setPreservesAll();
}

bool ControlDependenceAnalysisPass::runOnModule(Module &M) {
  bool changed = false;
  for (auto &F : M)
    if (!F.isDeclaration())
      changed |= runOnFunction(F);
  return changed;
}

bool ControlDependenceAnalysisPass::runOnFunction(Function &F) {
  auto &PDT = getAnalysis<PostDominatorTreeWrapperPass>(F).getPostDomTree();
  m_analyses[&F] = std::make_unique<ControlDependenceAnalysisImpl>(F, PDT);
  return false;
}

llvm::StringRef ControlDependenceAnalysisPass::getPassName() const {
  return "ControlDependenceAnalysisPass";
}

void ControlDependenceAnalysisPass::print(raw_ostream &os,
                                          const llvm::Module *) const {
  os << "ControlDependenceAnalysisPass::print\n";
}

bool ControlDependenceAnalysisPass::hasAnalysisFor(
    const llvm::Function &F) const {
  return m_analyses.count(&F) > 0;
}

ControlDependenceAnalysis &
ControlDependenceAnalysisPass::getControlDependenceAnalysis(
    const llvm::Function &F) {
  assert(hasAnalysisFor(F));
  return *m_analyses[&F];
}

llvm::ModulePass *createControlDependenceAnalysisPass() {
  return new ControlDependenceAnalysisPass();
}

} // namespace gsa

static llvm::RegisterPass<gsa::ControlDependenceAnalysisPass>
    GsaCD("gsa-cd-analysis", "Compute Control Dependence", true, true);
