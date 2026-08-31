#include "IR/SVFG/MemoryRegionPartitioner.h"

#include <algorithm>

namespace lotus::analysis {

void MemoryRegionPartitioner::reset(MemoryRegionPartitionStrategy strategy) {
  strategy_ = strategy;
  unknownObject_ = 0;
  frozen_ = false;
  observations_.clear();
  partitions_.clear();
  stats_ = {};
}

const llvm::Function *
MemoryRegionPartitioner::partitionScope(const llvm::Function *scope) const {
  return strategy_ == MemoryRegionPartitionStrategy::InterDisjoint ? nullptr
                                                                   : scope;
}

bool MemoryRegionPartitioner::intersects(const SVFGNodeBS &lhs,
                                         const SVFGNodeBS &rhs) {
  const SVFGNodeBS *small = &lhs;
  const SVFGNodeBS *large = &rhs;
  if (large->size() < small->size())
    std::swap(small, large);
  return std::any_of(small->begin(), small->end(), [&](uint32_t object) {
    return large->count(object) != 0;
  });
}

void MemoryRegionPartitioner::addToPartition(RegionList &regions,
                                             const SVFGNodeBS &pointsTo,
                                             std::size_t &mergedSets) {
  SVFGNodeBS merged = pointsTo;
  bool absorbed = false;
  for (auto it = regions.begin(); it != regions.end();) {
    if (!intersects(*it, merged)) {
      ++it;
      continue;
    }
    merged.insert(it->begin(), it->end());
    it = regions.erase(it);
    absorbed = true;
  }
  if (absorbed)
    ++mergedSets;
  regions.push_back(std::move(merged));
}

void MemoryRegionPartitioner::observe(const llvm::Function *scope,
                                      const SVFGNodeBS &pointsTo) {
  if (frozen_ || pointsTo.empty())
    return;
  ++stats_.observations;
  observations_.emplace_back(scope, pointsTo);
}

void MemoryRegionPartitioner::freeze() {
  if (frozen_)
    return;
  frozen_ = true;
  stats_.inputSets = observations_.size();

  if (strategy_ == MemoryRegionPartitionStrategy::Distinct) {
    for (const auto &[scope, pointsTo] : observations_) {
      RegionList &regions = partitions_[scope];
      if (std::find(regions.begin(), regions.end(), pointsTo) == regions.end())
        regions.push_back(pointsTo);
    }
  } else {
    for (const auto &[scope, pointsTo] : observations_) {
      if (unknownObject_ != 0 && pointsTo.count(unknownObject_) != 0)
        continue;
      addToPartition(partitions_[partitionScope(scope)], pointsTo,
                     stats_.mergedSets);
    }
  }

  for (const auto &entry : partitions_) {
    stats_.regions += entry.second.size();
  }
}

SVFGNodeBS
MemoryRegionPartitioner::canonicalize(const llvm::Function *scope,
                                      const SVFGNodeBS &pointsTo) const {
  if (pointsTo.empty() || strategy_ == MemoryRegionPartitionStrategy::Distinct)
    return pointsTo;
  if (unknownObject_ != 0 && pointsTo.count(unknownObject_) != 0)
    return SVFGNodeBS{unknownObject_};

  auto partition = partitions_.find(partitionScope(scope));
  if (partition == partitions_.end())
    return pointsTo;
  for (const SVFGNodeBS &region : partition->second) {
    if (intersects(region, pointsTo))
      return region;
  }
  return pointsTo;
}

} // namespace lotus::analysis
