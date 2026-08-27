#pragma once

#include "CFL/Classical/Grammar.h"
#include "CFL/Classical/Graph.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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
  using ConstraintBucket =
      std::unordered_map<std::string, std::unordered_set<std::string>>;

  struct ConstraintSystem {
    ConstraintBucket con0;
    ConstraintBucket con1;
    ConstraintBucket pro;
    std::unordered_set<std::string> set_variables;
  };

  using WorkItem =
      std::tuple<std::string, std::string, std::string, std::string>;

  ConstraintSystem buildConstraintSystem(const LabeledGraph &graph,
                                         const Grammar &grammar) const;
  static std::vector<std::string> splitConstraint(const std::string &value);
};

} // namespace lotus::cfl::classical
