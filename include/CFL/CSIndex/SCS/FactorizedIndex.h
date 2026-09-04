#pragma once

#include "CFL/CSIndex/SCS/Index.h"

#include <utility>
#include <vector>

namespace lotus::cfl::cs_index::scs {

/**
 * Exact OR-composition for independently updated category policies whose joint
 * vulnerability condition is disjunctive.
 */
class FactorizedIndex {
public:
  explicit FactorizedIndex(std::vector<Index *> indexes)
      : indexes_(std::move(indexes)) {}

  bool reachable(int source, int sink) {
    for (Index *index : indexes_) {
      if (index && index->reachable(source, sink))
        return true;
    }
    return false;
  }

private:
  std::vector<Index *> indexes_;
};

} // namespace lotus::cfl::cs_index::scs
