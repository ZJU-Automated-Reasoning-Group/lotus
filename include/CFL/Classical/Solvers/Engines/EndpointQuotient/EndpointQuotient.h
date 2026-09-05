#ifndef LOTUS_CFL_CLASSICAL_ENDPOINT_QUOTIENT_H
#define LOTUS_CFL_CLASSICAL_ENDPOINT_QUOTIENT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace lotus {
namespace cfl {
namespace endpoint {

using Id = std::size_t;
using Count = std::uint64_t;

struct Edge {
  Id source;
  Id symbol;
  Id target;
};

// Normalized productions. Symbols may also label input edges: such edges are
// axioms. Conventional terminal/nonterminal grammars are a special case.
struct Rule {
  enum class Kind { Epsilon, Unary, Binary };
  Kind kind;
  Id lhs;
  Id left = 0;
  Id right = 0;

  static Rule epsilon(Id lhs) { return {Kind::Epsilon, lhs, 0, 0}; }
  static Rule unary(Id lhs, Id rhs) { return {Kind::Unary, lhs, rhs, 0}; }
  static Rule binary(Id lhs, Id left, Id right) {
    return {Kind::Binary, lhs, left, right};
  }
};

struct Problem {
  Id nodes = 0;
  Id symbols = 0;
  std::vector<Edge> edges;
  std::vector<Rule> rules;

  // Throws std::invalid_argument for out-of-range IDs or an invalid rule kind.
  void validate() const;
};

enum class PartitionMode {
  Grammar,   // Separate FIRST/LAST-derived partitions for every symbol.
  Global,    // Ablation: every symbol observes every edge label.
  Singleton // Ablation: no endpoint compression.
};

struct Options {
  PartitionMode partitions = PartitionMode::Grammar;
};

struct SymbolStatistics {
  Count source_classes = 0;
  Count target_classes = 0;
  Count positive_cells = 0;
  Count positive_facts = 0;
};

struct Statistics {
  Count input_edges = 0;
  Count seed_cells = 0;
  Count seed_facts = 0;
  Count cells = 0;
  Count nullable_symbols = 0;
  Count logical_facts = 0; // Includes axioms and epsilon; no duplicates.
  Count inferred_facts = 0;
  Count insert_attempts = 0;
  Count duplicate_inserts = 0;
  Count worklist_pushes = 0;
  Count worklist_pops = 0;
  Count peak_worklist = 0;
  Count binary_joins = 0; // Compatible ordered pairs of positive cells.
  Count unary_propagations = 0; // Candidate parent cells after refinement.
  Count binary_propagations = 0;
  Count successful_unary_propagations = 0;
  Count successful_binary_propagations = 0;
  Count bridge_pairs = 0; // Sum over distinct normalized binary productions.
  double preprocess_ms = 0;
  double saturation_ms = 0;
  double count_ms = 0;
  std::vector<SymbolStatistics> per_symbol;
};

// This is a self-contained research API, NOT a claimed adapter to Lotus's
// existing Grammar/LabeledGraph/CFLSolver interfaces. See the artifact README.
// All IDs are dense in [0,nodes) or [0,symbols). The problem is owned by value.
class Solver {
public:
  explicit Solver(Problem problem, Options options = {});
  ~Solver();
  Solver(Solver &&) noexcept;
  Solver &operator=(Solver &&) noexcept;
  Solver(const Solver &) = delete;
  Solver &operator=(const Solver &) = delete;

  // Computes the exact least fixed point. A repeated call is a no-op.
  void solve();
  bool contains(Id symbol, Id source, Id target) const;
  bool isNullable(Id symbol) const;
  const Statistics &statistics() const;

  // Visits each logical fact exactly once. Expansion is output-sensitive and
  // is intentionally NOT performed by solve() or statistics().
  using FactVisitor = std::function<void(Id, Id, Id)>;
  void forEachFact(const FactVisitor &visitor) const;

  // Visits disjoint rectangles of POSITIVE-LENGTH reachability only. Epsilon
  // diagonals are separate, accessible through isNullable()/contains().
  using RectangleVisitor = std::function<void(
      Id, const std::vector<Id> &, const std::vector<Id> &)>;
  void forEachPositiveRectangle(const RectangleVisitor &visitor) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace endpoint
} // namespace cfl
} // namespace lotus

#endif
