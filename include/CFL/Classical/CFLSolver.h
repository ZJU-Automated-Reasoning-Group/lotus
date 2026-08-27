#pragma once

#include "CFL/Classical/Grammar.h"
#include "CFL/Classical/Graph.h"

#include <cstddef>
#include <cstdint>

namespace lotus::cfl::classical {

struct ReachabilityStats {
  std::uint64_t classical_iterations = 0;
  std::size_t added_edges = 0;
};

class CFLSolver {
public:
  ReachabilityStats solve(LabeledGraph &graph, const Grammar &grammar) const;
};

} // namespace lotus::cfl::classical
