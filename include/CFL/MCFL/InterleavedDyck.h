#pragma once

#include "CFL/InterleavedDyckCore/Graph.h"
#include "CFL/MCFL/Grammar.h"
#include "CFL/MCFL/Graph.h"
#include "CFL/MCFL/Solver.h"

#include <cstddef>
#include <unordered_set>
#include <vector>

namespace lotus::cfl::mcfl {

enum class InterleavedGrammarVariant {
  Simple,
  Full,
};

enum class CondensationExpansionPolicy {
  /// Expand a condensed pair only when the original source reaches the
  /// original target. This avoids reverse pairs introduced by contracting a
  /// one-way neutral edge.
  ReachabilityFiltered,
  /// Reproduce the reference artifact's full cross-product expansion.
  ArtifactCompatible,
};

struct InterleavedAlphabet {
  std::vector<unsigned> parentheses;
  std::vector<unsigned> brackets;
};

struct InterleavedGrammar {
  Grammar grammar;
  std::unordered_set<Grammar::Nonterminal> parenthesis_family;
  std::unordered_set<Grammar::Nonterminal> bracket_family;
};

InterleavedAlphabet discoverInterleavedAlphabet(const Graph &graph);

/// Convert the shared typed interleaved-Dyck graph into the generic
/// string-labeled MCFL graph without reparsing the input dataset.
Graph adaptInterleavedDyckGraph(const interleaved_dyck::Graph &graph);

InterleavedGrammar buildInterleavedDyckGrammar(
    unsigned dimension, const InterleavedAlphabet &alphabet,
    InterleavedGrammarVariant variant = InterleavedGrammarVariant::Full);

struct InterleavedDimensionResult {
  unsigned dimension = 0;
  PairSet reachable_pairs;
  SolverStats stats;
};

struct InterleavedAnalysisResult {
  std::vector<InterleavedDimensionResult> dimensions;

  const PairSet &reachablePairs() const;
};

struct InterleavedOptions {
  unsigned max_dimension = 2;
  InterleavedGrammarVariant variant = InterleavedGrammarVariant::Full;
  bool condense = true;
  CondensationExpansionPolicy expansion_policy =
      CondensationExpansionPolicy::ReachabilityFiltered;
};

/// Dimension-indexed, sound underapproximations of typed interleaved-Dyck
/// reachability. This is distinct from AdaptiveInterleavedDyckSolver, which is
/// exact only for bidirected unary D1-interleaved-D1 after projection.
class InterleavedDyckSolver {
public:
  InterleavedAnalysisResult solve(const Graph &graph,
                                  const InterleavedOptions &options = {}) const;
  InterleavedAnalysisResult solve(const interleaved_dyck::Graph &graph,
                                  const InterleavedOptions &options = {}) const;
};

} // namespace lotus::cfl::mcfl
