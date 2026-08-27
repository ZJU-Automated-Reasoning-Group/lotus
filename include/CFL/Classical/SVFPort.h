#pragma once

#include "CFL/Classical/CFLSolver.h"
#include "IR/SVFG/SVFG.h"

#include <cstddef>
#include <cstdint>
#include <optional>
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

LabeledGraph encodeBigraph(const AliasConstraintGraph &graph);
LabeledGraph encodeBiPEGGraph(const AliasConstraintGraph &graph);
Grammar buildPagGrammar(const AliasConstraintGraph &graph);
Grammar buildPegGrammar(const AliasConstraintGraph &graph);

class AliasClient {
public:
  static AliasClient
  fromConstraintGraph(const AliasConstraintGraph &graph,
                      AliasEncodingMode mode = AliasEncodingMode::PAG);

  ReachabilityStats solve();
  bool mayAlias(std::size_t lhs, std::size_t rhs) const;
  std::vector<std::size_t> pointsTo(std::size_t ptr) const;

  const LabeledGraph &graph() const { return graph_; }
  const Grammar &grammar() const { return grammar_; }

private:
  AliasClient(LabeledGraph graph, Grammar grammar)
      : graph_(std::move(graph)), grammar_(std::move(grammar)) {}

  LabeledGraph graph_;
  Grammar grammar_;
};

LabeledGraph encodeSVFG(const lotus::analysis::SVFG &svfg);
Grammar buildVfgGrammar(const lotus::analysis::SVFG &svfg);

class ValueFlowClient {
public:
  static ValueFlowClient fromSVFG(const lotus::analysis::SVFG &svfg);

  ReachabilityStats solve();
  bool hasFlow(std::uint32_t source_node, std::uint32_t target_node) const;
  std::vector<std::uint32_t> reachableFrom(std::uint32_t source_node) const;

  const LabeledGraph &graph() const { return graph_; }
  const Grammar &grammar() const { return grammar_; }

private:
  ValueFlowClient(LabeledGraph graph, Grammar grammar,
                  std::unordered_map<std::uint32_t, std::size_t> node_to_vertex)
      : graph_(std::move(graph)), grammar_(std::move(grammar)),
        node_to_vertex_(std::move(node_to_vertex)) {}

  LabeledGraph graph_;
  Grammar grammar_;
  std::unordered_map<std::uint32_t, std::size_t> node_to_vertex_;
};

} // namespace lotus::cfl::classical
