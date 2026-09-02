#include "CFL/Classical/Solvers/ConstraintGrounding.h"

#include <limits>
#include <stdexcept>
#include <vector>

namespace lotus::cfl::classical {

ConstraintGroundingSolver::ConstraintSystem
ConstraintGroundingSolver::buildConstraintSystem(const LabeledGraph &graph,
                                                 const Grammar &grammar) const {
  ConstraintSystem system;
  auto allocateVariable = [&]() -> VariableId {
    if (system.con1_by_lhs.size() == std::numeric_limits<VariableId>::max()) {
      throw std::overflow_error(
          "Constraint grounding variable id space exhausted");
    }
    const auto id = static_cast<VariableId>(system.con1_by_lhs.size());
    system.con1_by_lhs.emplace_back();
    system.pro_by_dependency.emplace_back();
    return id;
  };

  std::vector<VariableId> node_variables;
  node_variables.reserve(graph.vertexCount());
  for (std::size_t i = 0; i < graph.vertexCount(); ++i) {
    const VariableId variable = allocateVariable();
    node_variables.push_back(variable);
    system.seeds.push_back(variable);
  }

  for (const auto &[label, pairs] : graph.symbolPairs()) {
    if (!grammar.hasSymbol(label)) {
      throw std::invalid_argument("Graph uses unknown grammar symbol: " +
                                  label);
    }
    for (const auto &[source, target] : pairs) {
      const VariableId left = node_variables.at(source);
      const VariableId right = node_variables.at(target);
      system.con1_by_lhs[left].push_back({left, right});
    }
  }

  for (const auto &[left, productions] : grammar.productions()) {
    (void)left;
    for (const auto &right : productions) {
      if (right.size() == 2) {
        for (std::size_t i = 0; i < graph.vertexCount(); ++i) {
          const VariableId x = node_variables[i];
          const VariableId reached = allocateVariable();
          const VariableId destination = allocateVariable();
          system.pro_by_dependency[x].push_back({reached, x});
          system.pro_by_dependency[reached].push_back({destination, reached});
          system.con1_by_lhs[x].push_back({x, destination});
        }
      } else if (right.size() == 1) {
        for (std::size_t i = 0; i < graph.vertexCount(); ++i) {
          const VariableId x = node_variables[i];
          if (right[0] == Grammar::kEpsilonSymbol) {
            system.con1_by_lhs[x].push_back({x, x});
            continue;
          }

          const VariableId destination = allocateVariable();
          system.con1_by_lhs[x].push_back({x, destination});
          system.pro_by_dependency[x].push_back({destination, x});
        }
      }
    }
  }

  return system;
}

ConstraintGroundingStatistics
ConstraintGroundingSolver::ground(const LabeledGraph &graph,
                                  const Grammar &grammar) const {
  const auto system = buildConstraintSystem(graph, grammar);

  std::vector<VariableId> worklist;
  std::vector<bool> ground(system.con1_by_lhs.size(), false);
  for (VariableId seed : system.seeds) {
    ground[seed] = true;
    worklist.push_back(seed);
  }

  ConstraintGroundingStatistics stats;
  for (const auto &constraints : system.con1_by_lhs) {
    stats.constraint_variables += constraints.empty() ? 0 : 1;
  }
  stats.set_variables = ground.size();

  while (!worklist.empty()) {
    const VariableId variable = worklist.back();
    worklist.pop_back();

    for (const Constraint &constraint : system.con1_by_lhs[variable]) {
      ++stats.processed_constraints;
      if (!ground[constraint.rhs]) {
        ground[constraint.rhs] = true;
        worklist.push_back(constraint.rhs);
      }
    }
    for (const Constraint &constraint : system.pro_by_dependency[variable]) {
      ++stats.processed_constraints;
      if (!ground[constraint.lhs]) {
        ground[constraint.lhs] = true;
        worklist.push_back(constraint.lhs);
      }
    }
  }

  for (bool value : ground) {
    if (value) {
      ++stats.grounded_variables;
    }
  }

  return stats;
}

} // namespace lotus::cfl::classical
