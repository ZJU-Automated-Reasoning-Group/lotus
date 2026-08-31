/**
 * @file MultiStageSlicer.h
 * @brief MSli-style staged slicing for thread-aware sparse value flow.
 */
#pragma once

#include "IR/GraphView.h"

#include <cstddef>
#include <memory>
#include <unordered_set>
#include <vector>

namespace lotus::analysis {

class ThreadCreationTree;

class MultiStageSlicer {
public:
  struct Statistics {
    std::size_t originalNodes = 0;
    std::size_t candidateRoots = 0;
    std::size_t candidateNodes = 0;
    std::size_t synchronizationNodes = 0;
    std::size_t pointsToNodes = 0;
  };

  explicit MultiStageSlicer(const SVFG &graph,
                            const ThreadCreationTree *threadTree = nullptr)
      : graph_(&graph), threadTree_(threadTree) {}

  /// Builds the final PTA slice from the current thread-interference overlay.
  std::unique_ptr<FilteredSVFGView> slice();

  const Statistics &statistics() const { return stats_; }
  const std::unordered_set<const llvm::Instruction *> &
  synchronizationInstructions() const {
    return synchronizationInstructions_;
  }

private:
  using NodeSet = FilteredSVFGView::NodeSet;

  NodeSet collectCandidateRoots() const;
  NodeSet backwardClosure(const NodeSet &roots, const NodeSet *allowed,
                          bool includeThreadEdges) const;
  NodeSet expandSynchronizationAndCallDependence(NodeSet nodes);

  const SVFG *graph_;
  const ThreadCreationTree *threadTree_;
  std::unordered_set<const llvm::Instruction *> synchronizationInstructions_;
  Statistics stats_;
};

} // namespace lotus::analysis
