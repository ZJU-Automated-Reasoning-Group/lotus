#pragma once

#include "CFL/InterleavedDyckCore/Graph.h"

#include <cstddef>

namespace lotus::cfl::interleaved_dyck_approximation {

using interleaved_dyck::Edge;
using interleaved_dyck::EdgeHash;
using interleaved_dyck::Graph;
using interleaved_dyck::Label;
using interleaved_dyck::LabelKind;
using interleaved_dyck::Pair;
using interleaved_dyck::PairHash;
using interleaved_dyck::PairSet;
using interleaved_dyck::Vertex;

enum class Alphabet { Parenthesis, Bracket };
enum class GrammarStrength { Classic, Parity };
enum class BenchmarkKind { Taint, ValueFlow };

struct Options {
  /// The prototype and paper use two parity groups. Supported values are 1-4.
  unsigned parity_groups = 2;
  /// On-demand refinement can be expensive because it checks unknown pairs
  /// separately. It is enabled by default to reproduce the full pipeline.
  bool run_on_demand = true;
};

struct ApproximationResult {
  /// Benchmark-specific regular-language candidate filter.
  PairSet regularization;
  /// Overapproximation: each projection may use a different witness path.
  PairSet intersection;
  /// Underapproximation: one union-Dyck witness satisfies both projections.
  PairSet underapproximation;
  /// Overapproximation after classic derivation-tracing refinement.
  PairSet mutual_refinement;
  /// Tighter overapproximation after parity/endpoint refinement.
  PairSet stronger_grammar;
  /// Final overapproximation after pairwise refinement.
  PairSet on_demand;
};

/// Staged lower/upper approximations for typed interleaved-Dyck reachability.
///
/// This solver does not compute the exact general typed relation. A pair in
/// `underapproximation` is definitely reachable; a pair absent from the final
/// `on_demand` overapproximation is definitely unreachable; pairs between
/// those bounds remain unresolved by this pipeline.
class Solver {
public:
  /// Runs one projected Dyck grammar. Edges in the other alphabet are treated
  /// as unconstrained terminals, as in the original approximation.
  PairSet
  projectedReachability(const Graph &graph, Alphabet balanced_alphabet,
                        GrammarStrength strength = GrammarStrength::Classic,
                        unsigned parity_groups = 2) const;

  /// Intersects the two independently witnessed projected reachability sets.
  PairSet intersection(const Graph &graph,
                       GrammarStrength strength = GrammarStrength::Classic,
                       unsigned parity_groups = 2) const;

  /// Recognizes the Dyck language over the union of both alphabets. This is a
  /// sound underapproximation of interleaved-Dyck reachability.
  PairSet underapproximation(const Graph &graph) const;

  /// Alternates the two projected analyses and retains only graph edges used
  /// by their derivations until the edge set stabilizes.
  PairSet
  mutualRefinement(const Graph &graph,
                   GrammarStrength strength = GrammarStrength::Classic,
                   unsigned parity_groups = 2,
                   BenchmarkKind benchmark = BenchmarkKind::Taint) const;

  /// Reproduces the staged benchmark pipeline from the reference artifact.
  ApproximationResult analyze(const Graph &graph,
                              BenchmarkKind benchmark = BenchmarkKind::Taint,
                              const Options &options = {}) const;
};

} // namespace lotus::cfl::interleaved_dyck_approximation
