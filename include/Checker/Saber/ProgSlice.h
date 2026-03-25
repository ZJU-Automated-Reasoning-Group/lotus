//===- ProgSlice.h -- Program slicing based on SVF----------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
//
//===----------------------------------------------------------------------===//

#ifndef PROGSLICE_H_
#define PROGSLICE_H_

#include "Alias/DDA/CxtDPItem.h"
#include "Checker/Saber/SaberCondAllocator.h"
#include "IR/SVFG/SVFG.h"

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>

namespace lotus {
namespace analysis {

class ProgSlice {

public:
  using SVFGNodeSet = std::set<const SVFGNode *>;
  using SVFGNodeSetIter = SVFGNodeSet::const_iterator;
  using Condition = SaberCondAllocator::Condition;
  using SVFGNodeToCondMap = std::map<const SVFGNode *, Condition>;
  using VFWorkList = std::deque<const SVFGNode *>;
  using CFWorkList = std::deque<const llvm::BasicBlock *>;
  using SVFGNodeToSVFGNodeSetMap = SaberCondAllocator::SVFGNodeToSVFGNodeSetMap;

  ProgSlice(const SVFGNode *src, SaberCondAllocator *pa, const SVFG *graph)
      : root(src), partialReachable(false), fullReachable(false),
        reachGlob(false), pathAllocator(pa), curSVFGNode(nullptr),
        finalCond(pa->getFalseCond()), falseCond_(pa->getFalseCond()),
        svfg(graph) {}

  virtual ~ProgSlice() = default;

  uint32_t getForwardSliceSize() const { return forwardslice.size(); }
  uint32_t getBackwardSliceSize() const { return backwardslice.size(); }

  void addToForwardSlice(const SVFGNode *node) { forwardslice.insert(node); }
  void addToBackwardSlice(const SVFGNode *node) { backwardslice.insert(node); }
  bool inForwardSlice(const SVFGNode *node) const {
    return forwardslice.find(node) != forwardslice.end();
  }
  bool inBackwardSlice(const SVFGNode *node) const {
    return backwardslice.find(node) != backwardslice.end();
  }

  void addToSinks(const SVFGNode *node) { sinks.insert(node); }
  bool isSink(const SVFGNode *node) const {
    return sinks.find(node) != sinks.end();
  }

  void setPartialReachable() { partialReachable = true; }
  void setAllReachable() { fullReachable = true; }
  void setReachGlobal() { reachGlob = true; }

  bool isPartialReachable() const { return partialReachable || reachGlob; }
  bool isAllReachable() const { return fullReachable || reachGlob; }
  bool isReachGlobal() const { return reachGlob; }

  bool AllPathReachableSolve();
  bool isSatisfiableForAll();
  bool isSatisfiableForPairs();

  const SVFGNode *getSource() const { return root; }

  const SVFGNodeSet &getSinks() const { return sinks; }
  SVFGNodeSetIter sinksBegin() const { return sinks.begin(); }
  SVFGNodeSetIter sinksEnd() const { return sinks.end(); }

  SVFGNodeSetIter forwardSliceBegin() const { return forwardslice.begin(); }
  SVFGNodeSetIter forwardSliceEnd() const { return forwardslice.end(); }

  SVFGNodeSetIter backwardSliceBegin() const { return backwardslice.begin(); }
  SVFGNodeSetIter backwardSliceEnd() const { return backwardslice.end(); }

  void setCurSVFGNode(const SVFGNode *node) {
    curSVFGNode = node;
    pathAllocator->setCurEvalSVFGNode(node);
  }
  const SVFGNode *getCurSVFGNode() const { return curSVFGNode; }

  bool setVFCond(const SVFGNode *node, const Condition &cond) {
    auto it = vfConds.find(node);
    if (it != vfConds.end() &&
        pathAllocator->isEquivalentBranchCond(it->second, cond))
      return false;
    vfConds[node] = cond;
    return true;
  }
  const Condition &getVFCond(const SVFGNode *node) const {
    auto it = vfConds.find(node);
    if (it != vfConds.end())
      return it->second;
    return falseCond_;
  }
  bool hasVFCond(const SVFGNode *node) const {
    return vfConds.find(node) != vfConds.end();
  }

  const SVFGNodeToSVFGNodeSetMap &getRemovedSUVFEdges() const {
    return pathAllocator->getRemovedSUVFEdges();
  }

  void clearCFCond() { pathAllocator->clearCFCond(); }
  const llvm::BasicBlock *getSVFGNodeBB(const SVFGNode *node) const;
  Condition computeInvalidCondFromRemovedSUVFEdge(const SVFGNode *cur);

  const Condition &getFinalCond() const { return finalCond; }
  void setFinalCond(const Condition &cond) { finalCond = cond; }

  /// Evaluate final path condition to a string (SVF-compatible).
  std::string evalFinalCond() const;

  /// Path-condition event: (branch instruction, branch taken).
  using PathCondEvent = std::pair<const llvm::Instruction *, bool>;
  using EventStack = std::vector<PathCondEvent>;
  /// Append path condition literals to event stack (SVF evalFinalCond2Event).
  void evalFinalCond2Event(EventStack &eventStack) const;

  /// Helper methods matching SVF API
  const llvm::CallBase *getCallSite(const SVFGEdge *edge) const;
  const llvm::CallBase *getRetSite(const SVFGEdge *edge) const;

  Condition condAnd(const Condition &lhs, const Condition &rhs) const {
    return pathAllocator->condAnd(lhs, rhs);
  }
  Condition condOr(const Condition &lhs, const Condition &rhs) const {
    return pathAllocator->condOr(lhs, rhs);
  }
  Condition condNeg(const Condition &cond) const {
    return pathAllocator->condNeg(cond);
  }
  Condition getTrueCond() const { return pathAllocator->getTrueCond(); }
  Condition getFalseCond() const { return pathAllocator->getFalseCond(); }
  std::string dumpCond(const Condition &cond) const {
    return pathAllocator->dumpCond(cond);
  }

  bool isEquivalentBranchCond(const Condition &lhs,
                              const Condition &rhs) const {
    return pathAllocator->isEquivalentBranchCond(lhs, rhs);
  }

protected:
  void destroy() {}

  const SVFGNode *root;
  bool partialReachable = false;
  bool fullReachable = false;
  bool reachGlob = false;
  SaberCondAllocator *pathAllocator;
  const SVFGNode *curSVFGNode = nullptr;
  Condition finalCond;
  Condition falseCond_;
  const SVFG *svfg;
  SVFGNodeSet forwardslice;
  SVFGNodeSet backwardslice;
  SVFGNodeSet sinks;
  SVFGNodeToCondMap vfConds;
};

} // namespace analysis
} // namespace lotus

#endif
