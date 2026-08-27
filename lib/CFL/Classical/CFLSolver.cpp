#include "CFL/Classical/CFLSolver.h"

#include <vector>

namespace lotus::cfl::classical {

ReachabilityStats CFLSolver::solve(LabeledGraph &graph,
                                   const Grammar &grammar) const {
  ReachabilityStats stats;
  auto worklist = graph.edges();

  for (const auto &nullable_symbol : grammar.nullableSymbols()) {
    for (std::size_t node = 0; node < graph.vertexCount(); ++node) {
      if (!graph.addEdge(node, node, nullable_symbol)) {
        continue;
      }

      worklist.push_back({nullable_symbol, node, node});
      ++stats.added_edges;
    }
  }

  while (!worklist.empty()) {
    const auto selected = worklist.back();
    worklist.pop_back();

    if (const auto unary_it = grammar.unaryByRhs().find(selected.label);
        unary_it != grammar.unaryByRhs().end()) {
      for (const auto &lhs : unary_it->second) {
        if (!graph.addEdge(selected.source, selected.target, lhs)) {
          continue;
        }

        worklist.push_back({lhs, selected.source, selected.target});
        ++stats.added_edges;
      }
    }

    if (const auto binary_it = grammar.binaryByFirst().find(selected.label);
        binary_it != grammar.binaryByFirst().end()) {
      for (const auto &rule : binary_it->second) {
        const auto pairs = graph.edgesForLabelCopy(rule.second);
        std::size_t iteration_count = 0;
        std::size_t solution_count = 0;

        for (const auto &[middle, target] : pairs) {
          ++iteration_count;
          if (middle != selected.target) {
            continue;
          }

          if (!graph.addEdge(selected.source, target, rule.lhs)) {
            continue;
          }

          worklist.push_back({rule.lhs, selected.source, target});
          ++solution_count;
          ++stats.added_edges;
        }

        stats.classical_iterations += iteration_count;
        (void)solution_count;
      }
    }

    if (const auto binary_it = grammar.binaryBySecond().find(selected.label);
        binary_it != grammar.binaryBySecond().end()) {
      for (const auto &rule : binary_it->second) {
        const auto pairs = graph.edgesForLabelCopy(rule.first);
        std::size_t iteration_count = 0;
        std::size_t solution_count = 0;

        for (const auto &[source, middle] : pairs) {
          ++iteration_count;
          if (middle != selected.source) {
            continue;
          }

          if (!graph.addEdge(source, selected.target, rule.lhs)) {
            continue;
          }

          worklist.push_back({rule.lhs, source, selected.target});
          ++solution_count;
          ++stats.added_edges;
        }

        stats.classical_iterations += iteration_count;
        (void)solution_count;
      }
    }
  }

  return stats;
}

} // namespace lotus::cfl::classical
