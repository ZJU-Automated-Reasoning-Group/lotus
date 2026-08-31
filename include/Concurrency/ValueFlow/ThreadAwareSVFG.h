/**
 * @file ThreadAwareSVFG.h
 * @brief Thread-interference overlay for Lotus's sparse value-flow graph.
 */
#pragma once

#include "IR/GraphView.h"

#include <cstddef>
#include <unordered_set>

namespace mhp {
class IMHPAnalysis;
class LockSetAnalysis;
} // namespace mhp

namespace lotus::analysis {

class ThreadCreationTree;

/// Adds guarded Store->Load and symmetric Store<->Store value-flow edges for
/// accesses that may execute concurrently. The base SVFG remains owned by the
/// caller and can be restored with clear().
class ThreadAwareSVFGBuilder {
public:
  struct Statistics {
    std::size_t stores = 0;
    std::size_t loads = 0;
    std::size_t aliasCandidates = 0;
    std::size_t parallelCandidates = 0;
    std::size_t mutuallyExcludedCandidates = 0;
    std::size_t edgesAdded = 0;
    std::size_t forkParameterEdges = 0;
    std::size_t forkMemoryEdges = 0;
    std::size_t joinMemoryEdges = 0;
    std::size_t joinMemoryNodes = 0;
  };

  ThreadAwareSVFGBuilder(SVFG &graph, const mhp::IMHPAnalysis &mhp,
                         const mhp::LockSetAnalysis *locks = nullptr,
                         const FilteredSVFGView *scope = nullptr,
                         const ThreadCreationTree *threadTree = nullptr);
  ~ThreadAwareSVFGBuilder();

  ThreadAwareSVFGBuilder(const ThreadAwareSVFGBuilder &) = delete;
  ThreadAwareSVFGBuilder &operator=(const ThreadAwareSVFGBuilder &) = delete;

  const Statistics &build();
  void clear();

  const Statistics &statistics() const { return stats_; }
  std::size_t edgeCount() const { return overlayEdges_.size(); }

private:
  bool inScope(const SVFGNode *node) const;
  SVFGNodeBS intersectTargets(const SVFGNode &lhs, const SVFGNode &rhs) const;
  bool addInterferenceEdge(SVFGNode &source, SVFGNode &destination,
                           const SVFGNodeBS &guard);
  bool addOwnedEdge(SVFGNode &source, SVFGNode &destination, SVFGEdgeK kind,
                    const llvm::CallBase *callSite,
                    const SVFGNodeBS &guard = {});
  void connectForkJoinMemoryFlow();
  void connectForkFlow();
  void connectJoinFlow();

  SVFG *graph_;
  const mhp::IMHPAnalysis *mhp_;
  const mhp::LockSetAnalysis *locks_;
  const FilteredSVFGView *scope_;
  const ThreadCreationTree *threadTree_;
  std::unordered_set<SVFGEdge *> overlayEdges_;
  Statistics stats_;
};

} // namespace lotus::analysis
