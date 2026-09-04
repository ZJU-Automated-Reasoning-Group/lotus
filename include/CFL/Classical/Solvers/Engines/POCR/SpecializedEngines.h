#pragma once

#include "CFL/Classical/Core/Graph.h"
#include "CFL/Classical/Core/Relation.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace lotus::cfl::classical::engines {

enum class SpecializedPocrBackend {
  Pocr,
  Focr,
};

struct SpecializedPocrStatistics {
  std::size_t graph_nodes = 0;
  std::size_t graph_edges = 0;
  std::size_t processed_items = 0;
  std::size_t queued_items = 0;
  std::size_t duplicate_items = 0;
  std::size_t reachability_checks = 0;
  std::size_t reachability_pairs = 0;
  std::size_t value_or_flow_pairs = 0;
  std::size_t matched_pairs = 0;
  std::size_t critical_edges = 0;
  std::size_t cycle_simplifications = 0;
};

/// POCR's hand-specialized field-sensitive alias propagation engine.
class PocrAliasEngine {
public:
  explicit PocrAliasEngine(const LabeledGraph &graph);
  ~PocrAliasEngine();
  PocrAliasEngine(PocrAliasEngine &&) noexcept;
  PocrAliasEngine &operator=(PocrAliasEngine &&) noexcept;
  PocrAliasEngine(const PocrAliasEngine &) = delete;
  PocrAliasEngine &operator=(const PocrAliasEngine &) = delete;

  SpecializedPocrStatistics solve();
  bool mayAlias(NodeId first, NodeId second) const;
  bool assignmentReachable(NodeId source, NodeId target) const;
  std::vector<std::pair<NodeId, NodeId>> valuePairs() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// FOCR's hand-specialized field-sensitive alias propagation engine.
class FocrAliasEngine {
public:
  explicit FocrAliasEngine(const LabeledGraph &graph,
                           bool simplify_cycles = false);
  ~FocrAliasEngine();
  FocrAliasEngine(FocrAliasEngine &&) noexcept;
  FocrAliasEngine &operator=(FocrAliasEngine &&) noexcept;
  FocrAliasEngine(const FocrAliasEngine &) = delete;
  FocrAliasEngine &operator=(const FocrAliasEngine &) = delete;

  SpecializedPocrStatistics solve();
  bool mayAlias(NodeId first, NodeId second) const;
  bool assignmentReachable(NodeId source, NodeId target) const;
  std::vector<std::pair<NodeId, NodeId>> valuePairs() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// POCR's hand-specialized context-sensitive value-flow propagation engine.
class PocrValueFlowEngine {
public:
  explicit PocrValueFlowEngine(const LabeledGraph &graph);
  ~PocrValueFlowEngine();
  PocrValueFlowEngine(PocrValueFlowEngine &&) noexcept;
  PocrValueFlowEngine &operator=(PocrValueFlowEngine &&) noexcept;
  PocrValueFlowEngine(const PocrValueFlowEngine &) = delete;
  PocrValueFlowEngine &operator=(const PocrValueFlowEngine &) = delete;

  SpecializedPocrStatistics solve();
  bool hasFlow(NodeId source, NodeId target) const;
  std::vector<std::pair<NodeId, NodeId>> flowPairs() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// FOCR's hand-specialized context-sensitive value-flow propagation engine.
class FocrValueFlowEngine {
public:
  explicit FocrValueFlowEngine(const LabeledGraph &graph,
                               bool simplify_cycles = false);
  ~FocrValueFlowEngine();
  FocrValueFlowEngine(FocrValueFlowEngine &&) noexcept;
  FocrValueFlowEngine &operator=(FocrValueFlowEngine &&) noexcept;
  FocrValueFlowEngine(const FocrValueFlowEngine &) = delete;
  FocrValueFlowEngine &operator=(const FocrValueFlowEngine &) = delete;

  SpecializedPocrStatistics solve();
  bool hasFlow(NodeId source, NodeId target) const;
  std::vector<std::pair<NodeId, NodeId>> flowPairs() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical::engines
