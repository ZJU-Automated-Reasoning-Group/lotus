#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lotus::cfl::classical {

struct ConstraintGroundingStatistics {
  std::uint64_t processed_constraints = 0;
  std::size_t constraint_variables = 0;
  std::size_t set_variables = 0;
  std::size_t grounded_variables = 0;
};

/// Structural set-constraint grounding analysis. This is not a CFL
/// reachability backend and does not produce a node-pair relation. Production
/// shapes generate dependencies; terminal identities are validated at
/// construction but are not matched during grounding.
class ConstraintGroundingSolver {
public:
  ConstraintGroundingStatistics ground(const LabeledGraph &graph,
                                       const Grammar &grammar) const;

private:
  using VariableId = std::uint32_t;

  struct Constraint {
    VariableId lhs = 0;
    VariableId rhs = 0;
  };

  struct ConstraintSystem {
    std::vector<VariableId> seeds;
    std::vector<std::vector<Constraint>> con1_by_lhs;
    std::vector<std::vector<Constraint>> pro_by_dependency;
  };

  ConstraintSystem buildConstraintSystem(const LabeledGraph &graph,
                                         const Grammar &grammar) const;
};

} // namespace lotus::cfl::classical
