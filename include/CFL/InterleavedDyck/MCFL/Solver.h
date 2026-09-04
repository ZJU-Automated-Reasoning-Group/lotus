#pragma once

#include "CFL/InterleavedDyck/MCFL/Grammar.h"
#include "CFL/InterleavedDyck/MCFL/Graph.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace lotus::cfl::interleaved_dyck::mcfl {

struct Span {
  Vertex source = 0;
  Vertex target = 0;

  bool operator==(const Span &other) const;
};

struct Fact {
  Grammar::Nonterminal nonterminal = 0;
  std::vector<Span> spans;

  bool operator==(const Fact &other) const;
};

struct SolverStats {
  std::size_t facts = 0;
  std::size_t worklist_pops = 0;
  std::size_t rejected_duplicates = 0;
  std::size_t rejected_unreachable_gaps = 0;
  std::size_t type5_combinations = 0;
};

namespace detail {
struct WitnessData;
} // namespace detail

class ReachabilityResult {
public:
  const PairSet &reachablePairs() const { return reachable_pairs_; }
  const std::vector<Fact> &facts() const;
  const SolverStats &stats() const { return stats_; }

  bool reaches(Vertex source, Vertex target) const;
  std::optional<std::vector<Edge>> witness(Vertex source, Vertex target) const;

private:
  friend class Solver;

  PairSet reachable_pairs_;
  SolverStats stats_;
  std::shared_ptr<const detail::WitnessData> witness_data_;
};

struct SolverOptions {
  /// Reject tuples whose adjacent components cannot be connected in the
  /// underlying graph. This is the paper's generic non-permuting-grammar
  /// optimization.
  bool prune_unreachable_gaps = true;

  /// Optional stronger feasibility relation. If present, this replaces plain
  /// graph reachability for adjacent tuple components.
  std::function<bool(Grammar::Nonterminal, Vertex, Vertex)> gap_reachable;
};

/// Exact reachability for the supplied non-deleting, non-permuting MCFG.
/// Exactness is relative to the grammar and does not imply that an
/// approximating interleaved-Dyck grammar recognizes the full target language.
class Solver {
public:
  ReachabilityResult solve(const Graph &graph, const Grammar &grammar,
                           const SolverOptions &options = {}) const;
};

} // namespace lotus::cfl::interleaved_dyck::mcfl
