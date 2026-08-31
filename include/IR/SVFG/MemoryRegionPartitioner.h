/**
 * @file MemoryRegionPartitioner.h
 * @brief Frozen points-to-set partition policies for sparse MemorySSA.
 */
#pragma once

#include "IR/SVFG/SVFGNode.h"

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace llvm {
class Function;
}

namespace lotus::analysis {

enum class MemoryRegionPartitionStrategy {
  Distinct,
  IntraDisjoint,
  InterDisjoint,
};

/// Computes canonical, non-overlapping region covers before MemorySSA nodes are
/// created. IntraDisjoint partitions independently per function;
/// InterDisjoint partitions the whole module. Distinct preserves exact sets.
class MemoryRegionPartitioner {
public:
  struct Statistics {
    std::size_t observations = 0;
    std::size_t inputSets = 0;
    std::size_t regions = 0;
    std::size_t mergedSets = 0;
  };

  explicit MemoryRegionPartitioner(MemoryRegionPartitionStrategy strategy =
                                       MemoryRegionPartitionStrategy::Distinct)
      : strategy_(strategy) {}

  void reset(MemoryRegionPartitionStrategy strategy);
  void setUnknownObject(uint32_t object) { unknownObject_ = object; }
  void observe(const llvm::Function *scope, const SVFGNodeBS &pointsTo);
  void freeze();

  SVFGNodeBS canonicalize(const llvm::Function *scope,
                          const SVFGNodeBS &pointsTo) const;

  bool frozen() const { return frozen_; }
  MemoryRegionPartitionStrategy strategy() const { return strategy_; }
  const Statistics &statistics() const { return stats_; }

private:
  using RegionList = std::vector<SVFGNodeBS>;

  const llvm::Function *partitionScope(const llvm::Function *scope) const;
  static bool intersects(const SVFGNodeBS &lhs, const SVFGNodeBS &rhs);
  static void addToPartition(RegionList &regions, const SVFGNodeBS &pointsTo,
                             std::size_t &mergedSets);

  MemoryRegionPartitionStrategy strategy_;
  uint32_t unknownObject_ = 0;
  bool frozen_ = false;
  std::vector<std::pair<const llvm::Function *, SVFGNodeBS>> observations_;
  std::unordered_map<const llvm::Function *, RegionList> partitions_;
  Statistics stats_;
};

} // namespace lotus::analysis
