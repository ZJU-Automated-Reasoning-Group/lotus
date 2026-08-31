/**
 * @file SparseValueFlowRefinement.h
 * @brief Lightweight concurrency-specific value-flow refinement over SVFG.
 */
#pragma once

#include "Alias/Infrastructure/PtsSet/HashConsedPointsToSet.h"
#include "IR/GraphView.h"

#include <cstddef>
#include <optional>
#include <unordered_map>

namespace lotus::analysis {

/// Solves pointer-value flow over the SVFG and its thread-interference overlay.
/// Memory values are attached to sparse MemorySSA definitions rather than to
/// every LLVM instruction.
class SparseValueFlowRefinement {
public:
  struct Statistics {
    std::size_t iterations = 0;
    std::size_t pointsToFacts = 0;
    std::size_t memoryFacts = 0;
    std::size_t strongUpdates = 0;
    std::size_t hashConsedUniqueSets = 0;
    std::size_t hashConsedStoredElements = 0;
    std::size_t hashConsedUnionRequests = 0;
    std::size_t hashConsedUnionCacheHits = 0;
  };

  explicit SparseValueFlowRefinement(
      const SVFG &graph, const FilteredSVFGView *scope = nullptr,
      lotus::alias::PointsToSetBackend backend =
          lotus::alias::PointsToSetBackend::Mutable)
      : graph_(&graph), scope_(scope), backend_(backend) {}

  const Statistics &solve();

  const SVFGNodeBS &pointsTo(const SVFGNode *node) const;
  const SVFGNodeBS &memoryValue(const SVFGNode *node) const;
  bool hasCompletePointsTo(const SVFGNode *node) const;

  /// Flow-sensitive points-to set for a pointer value, when complete.
  std::optional<SVFGNodeBS> pointsTo(const llvm::Value *value) const;

  /// Flow-sensitive target set of an LLVM load/store/atomic access.
  std::optional<SVFGNodeBS>
  accessTargets(const llvm::Instruction *access) const;

  /// Returns nullopt if refinement is unavailable. Otherwise returns whether
  /// the two accesses may still address a common abstract object.
  std::optional<bool> mayAliasAccesses(const llvm::Instruction *lhs,
                                       const llvm::Instruction *rhs) const;

  const Statistics &statistics() const { return stats_; }
  lotus::alias::PointsToSetBackend backend() const { return backend_; }

private:
  struct WorkingSet {
    SVFGNodeBS mutableSet;
    lotus::alias::HashConsedPointsToSetArena::SetID interned =
        lotus::alias::HashConsedPointsToSetArena::EmptySet;
  };

  bool isStrongUpdate(const StoreSVFGNode &store) const;
  bool containsUnknown(const SVFGNodeBS &pointsTo) const;
  bool inScope(const SVFGNode *node) const;
  WorkingSet singleton(uint32_t object);
  bool mergeSet(WorkingSet &destination, const WorkingSet &source);
  bool mergeSet(WorkingSet &destination, const SVFGNodeBS &source);
  const SVFGNodeBS &materialize(const WorkingSet &set) const;
  const WorkingSet &nodeSet(const SVFGNode *node) const;
  const WorkingSet &memorySet(const SVFGNode *node) const;

  const SVFG *graph_;
  const FilteredSVFGView *scope_;
  lotus::alias::PointsToSetBackend backend_;
  lotus::alias::HashConsedPointsToSetArena hashConsedArena_;
  std::unordered_map<uint32_t, WorkingSet> nodePointsTo_;
  std::unordered_map<uint32_t, WorkingSet> memoryValues_;
  std::unordered_map<uint32_t, bool> nodeComplete_;
  std::unordered_map<uint32_t, bool> memoryComplete_;
  Statistics stats_;
};

} // namespace lotus::analysis
