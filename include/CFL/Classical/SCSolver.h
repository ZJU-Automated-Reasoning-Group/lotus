#pragma once

#include "CFL/Classical/Grammar.h"
#include "CFL/Classical/Graph.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lotus::cfl::classical {

struct SCStatistics {
  std::uint64_t classical_iterations = 0;
  std::size_t constraint_variables = 0;
  std::size_t set_variables = 0;
  std::size_t grounded_variables = 0;
};

class SCSolver {
public:
  SCStatistics solve(const LabeledGraph &graph, const Grammar &grammar) const;

private:
  using VariableId = std::uint32_t;

  struct Constraint {
    VariableId lhs = 0;
    SymbolId label = 0;
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
