/**
 * @file SparseFlowSensitivePTA.h
 * @brief Whole-program sparse flow-sensitive points-to refinement over SVFG.
 */
#pragma once

#include "IR/SVFG/SVFG.h"

#include <cstddef>
#include <optional>
#include <unordered_map>

namespace lotus::analysis {

/// Solves pointer-value flow over the SVFG and its thread-interference overlay.
/// Memory values are attached to sparse MemorySSA definitions rather than to
/// every LLVM instruction.
class SparseFlowSensitivePTA {
public:
  struct Statistics {
    std::size_t iterations = 0;
    std::size_t pointsToFacts = 0;
    std::size_t memoryFacts = 0;
    std::size_t strongUpdates = 0;
  };

  explicit SparseFlowSensitivePTA(const SVFG &graph) : graph_(&graph) {}

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

private:
  bool isStrongUpdate(const StoreSVFGNode &store) const;
  bool containsUnknown(const SVFGNodeBS &pointsTo) const;

  const SVFG *graph_;
  std::unordered_map<uint32_t, SVFGNodeBS> nodePointsTo_;
  std::unordered_map<uint32_t, SVFGNodeBS> memoryValues_;
  std::unordered_map<uint32_t, bool> nodeComplete_;
  std::unordered_map<uint32_t, bool> memoryComplete_;
  Statistics stats_;
};

} // namespace lotus::analysis
