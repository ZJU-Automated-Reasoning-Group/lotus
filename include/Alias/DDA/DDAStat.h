//===- DDAStat.h -- DDA statistics (SVF-style) ---------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <set>
#include <string>

namespace lotus {
namespace analysis {

class FlowDDA;

/// Statistics for demand-driven analysis (steps, strong updates, etc.).
class DDAStat {
public:
  explicit DDAStat(FlowDDA *pta);

  /// Number of distinct DPM states visited by solver recursion.
  uint32_t numOfDPM = 0;
  /// Number of stores where strong update was applied.
  uint32_t numOfStrongUpdates = 0;
  /// Number of load/store pairs proven must-alias during store handling.
  uint32_t numOfMustAliases = 0;
  /// Number of paths pruned by context/path conditions.
  uint32_t numOfInfeasiblePath = 0;
  /// Total backward transfer steps executed.
  uint64_t numOfStep = 0;
  /// Number of recomputation steps caused by dependency invalidation.
  uint64_t numOfStepInCycle = 0;
  /// Number of top-level user/client queries issued.
  uint64_t numQueries = 0;
  /// Number of queries that exceeded step budget and used conservative
  /// fallback.
  uint64_t numOutOfBudgetQueries = 0;
  double anaTimePerQuery = 0.0;
  double totalTimeOfQueries = 0.0;
  double totalTimeOfBKCondition = 0.0;
  /// IDs of store nodes that performed strong updates.
  std::set<uint32_t> strongUpdateStores;

  /// Maximum context length and path length observed (SVF-style tracking).
  uint32_t maximumCxtSeen = 0;
  uint32_t maximumPathSeen = 0;

  void performStat();
  void printStat(const std::string &str = "");

private:
  FlowDDA *pta_ = nullptr;
};

} // namespace analysis
} // namespace lotus
