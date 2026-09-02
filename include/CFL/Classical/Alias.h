#pragma once

#include "CFL/Classical/Solver.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace lotus::cfl::classical {

enum class AliasConstraintEdgeKind {
  Addr,
  Copy,
  Store,
  Load,
  NormalGep,
  VariantGep,
};

struct AliasConstraintEdge {
  std::size_t source = 0;
  std::size_t target = 0;
  AliasConstraintEdgeKind kind = AliasConstraintEdgeKind::Copy;
  std::optional<std::uint32_t> attribute;
};

class AliasConstraintGraph {
public:
  std::size_t addNode(const std::string &name);
  void addEdge(std::size_t source, std::size_t target,
               AliasConstraintEdgeKind kind,
               std::optional<std::uint32_t> attribute = std::nullopt);

  const std::vector<std::string> &nodeNames() const { return node_names_; }
  const std::vector<AliasConstraintEdge> &edges() const { return edges_; }

private:
  std::vector<std::string> node_names_;
  std::vector<AliasConstraintEdge> edges_;
};

enum class AliasEncodingMode {
  PAG,
  PEG,
};

LabeledGraph encodePagGraph(const AliasConstraintGraph &graph);
LabeledGraph encodePegGraph(const AliasConstraintGraph &graph);
Grammar buildPagGrammar(const AliasConstraintGraph &graph);
Grammar buildPegGrammar(const AliasConstraintGraph &graph);

class AliasClient {
public:
  ~AliasClient();
  AliasClient(AliasClient &&other) noexcept;
  AliasClient &operator=(AliasClient &&other) noexcept;
  AliasClient(const AliasClient &) = delete;
  AliasClient &operator=(const AliasClient &) = delete;

  static AliasClient
  fromConstraintGraph(const AliasConstraintGraph &graph,
                      AliasEncodingMode mode = AliasEncodingMode::PAG);

  ReachabilityStats solve(SolverBackend backend = SolverBackend::Baseline);
  /// Alternate solving with a client-supplied discovery policy. The callback
  /// may add nodes and constraints and returns true when it changed the input.
  ReachabilityStats solveToFixedPoint(
      SolverBackend backend,
      const std::function<bool(AliasClient &)> &discover_constraints,
      std::size_t max_rounds = 64);
  std::size_t addNode(const std::string &name);
  /// Add a constraint after construction. If solving has started, the same
  /// solver session is resumed on the next solve() call.
  bool addConstraint(std::size_t source, std::size_t target,
                     AliasConstraintEdgeKind kind,
                     std::optional<std::uint32_t> attribute = std::nullopt);
  bool mayAlias(std::size_t lhs, std::size_t rhs) const;
  std::vector<std::size_t> pointsTo(std::size_t ptr) const;

  const LabeledGraph &graph() const;
  const Grammar &grammar() const;

private:
  AliasClient(LabeledGraph graph, Grammar grammar, AliasEncodingMode mode);

  bool addEncodedEdge(std::size_t source, std::size_t target,
                      const std::string &forward, const std::string &reverse);
  const std::vector<std::size_t> &ensurePegDereferences(std::size_t pointer);
  void initializePegDereferences();
  void initializeGepAttributes();
  void rebuildGrammar();

  struct State;
  std::unique_ptr<State> state_;
  AliasEncodingMode mode_ = AliasEncodingMode::PAG;
  std::unordered_map<std::size_t, std::vector<std::size_t>> peg_dereferences_;
  std::size_t next_synthetic_dereference_ = 0;
  std::set<std::uint32_t> gep_attributes_;
  std::unique_ptr<SolverSession> session_;
  std::optional<SolverBackend> backend_;
};

} // namespace lotus::cfl::classical
