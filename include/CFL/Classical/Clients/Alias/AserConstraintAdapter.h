#pragma once

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Graph/ConstraintGraph/ConstraintGraph.h"
#include "CFL/Classical/Clients/Alias/AliasClient.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lotus::cfl::classical {

/// Convert an AserPTA graph while allowing the client to recover field offsets
/// that are not represented by Aser's Constraints enum itself.
template <typename Context, typename OffsetResolver>
AliasConstraintGraph
encodeAserConstraintGraph(const aser::ConstraintGraph<Context> &source,
                          OffsetResolver resolve_offset) {
  AliasConstraintGraph result;
  for (const auto *node : source) {
    result.addNode(std::to_string(node->getNodeID()));
  }

  for (const auto *node : source) {
    for (auto it = node->succ_edge_begin(); it != node->succ_edge_end(); ++it) {
      const auto edge = *it;
      AliasConstraintEdgeKind kind = AliasConstraintEdgeKind::Copy;
      std::optional<std::uint32_t> attribute;
      switch (edge.first) {
      case aser::Constraints::load:
        kind = AliasConstraintEdgeKind::Load;
        break;
      case aser::Constraints::store:
        kind = AliasConstraintEdgeKind::Store;
        break;
      case aser::Constraints::copy:
        kind = AliasConstraintEdgeKind::Copy;
        break;
      case aser::Constraints::addr_of:
        kind = AliasConstraintEdgeKind::Addr;
        break;
      case aser::Constraints::offset:
        attribute = resolve_offset(*node, *edge.second);
        kind = attribute ? AliasConstraintEdgeKind::NormalGep
                         : AliasConstraintEdgeKind::VariantGep;
        break;
      }
      result.addEdge(node->getNodeID(), edge.second->getNodeID(), kind,
                     attribute);
    }
  }
  return result;
}

/// Convert an AserPTA graph without external field-layout metadata. Offset
/// edges remain explicit variant GEPs rather than being silently treated as
/// copies.
template <typename Context>
AliasConstraintGraph
encodeAserConstraintGraph(const aser::ConstraintGraph<Context> &source) {
  return encodeAserConstraintGraph(
      source, [](const auto &, const auto &) -> std::optional<std::uint32_t> {
        return std::nullopt;
      });
}

template <typename Context>
AliasClient makeAliasClient(const aser::ConstraintGraph<Context> &source,
                            AliasEncodingMode mode = AliasEncodingMode::PAG) {
  return AliasClient::fromConstraintGraph(encodeAserConstraintGraph(source),
                                          mode);
}

template <typename Context, typename OffsetResolver>
AliasClient makeAliasClient(const aser::ConstraintGraph<Context> &source,
                            OffsetResolver resolve_offset,
                            AliasEncodingMode mode = AliasEncodingMode::PAG) {
  return AliasClient::fromConstraintGraph(
      encodeAserConstraintGraph(source, std::move(resolve_offset)), mode);
}

template <typename Solver>
AliasClient
makeAliasClientFromSolver(const Solver &solver,
                          AliasEncodingMode mode = AliasEncodingMode::PAG) {
  const auto *graph = solver.getConsGraph();
  if (!graph) {
    throw std::invalid_argument("Aser solver has no constructed graph");
  }
  return makeAliasClient(*graph, mode);
}

template <typename Solver, typename OffsetResolver>
AliasClient
makeAliasClientFromSolver(const Solver &solver, OffsetResolver resolve_offset,
                          AliasEncodingMode mode = AliasEncodingMode::PAG) {
  const auto *graph = solver.getConsGraph();
  if (!graph) {
    throw std::invalid_argument("Aser solver has no constructed graph");
  }
  return makeAliasClient(*graph, std::move(resolve_offset), mode);
}

struct NoAserOffsetResolver {
  template <typename Node>
  std::optional<std::uint32_t> operator()(const Node &, const Node &) const {
    return std::nullopt;
  }
};

/// Monotonically synchronizes constraints and newly created nodes from a live
/// Aser graph into an AliasClient between solveToFixedPoint rounds. The
/// explicit node map remains correct even when PEG conversion inserts
/// synthetic graph nodes.
template <typename Context, typename OffsetResolver = NoAserOffsetResolver>
class AserAliasSynchronizer {
public:
  using Graph = aser::ConstraintGraph<Context>;
  using Node = aser::CGNodeBase<Context>;

  AserAliasSynchronizer(const Graph &source, AliasClient &client,
                        OffsetResolver resolve_offset = {})
      : source_(source), client_(client),
        resolve_offset_(std::move(resolve_offset)) {
    for (const Node *node : source_) {
      node_map_.emplace(node->getNodeID(), node->getNodeID());
      reverse_node_map_.emplace(node->getNodeID(), node->getNodeID());
      for (auto it = node->succ_edge_begin(); it != node->succ_edge_end();
           ++it) {
        const auto edge = *it;
        seen_.insert({node->getNodeID(), edge.second->getNodeID(), edge.first});
      }
    }
  }

  bool synchronize() {
    bool changed = false;
    for (const Node *node : source_) {
      if (node_map_.count(node->getNodeID()) == 0) {
        const std::size_t mapped =
            client_.addNode(std::to_string(node->getNodeID()));
        node_map_.emplace(node->getNodeID(), mapped);
        reverse_node_map_.emplace(mapped, node->getNodeID());
        changed = true;
      }
    }

    for (const Node *node : source_) {
      for (auto it = node->succ_edge_begin(); it != node->succ_edge_end();
           ++it) {
        const auto edge = *it;
        const EdgeKey key{node->getNodeID(), edge.second->getNodeID(),
                          edge.first};
        if (!seen_.insert(key).second) {
          continue;
        }
        const auto [kind, attribute] =
            classify(*node, *edge.second, edge.first);
        changed =
            client_.addConstraint(mappedNode(key.source),
                                  mappedNode(key.target), kind, attribute) ||
            changed;
      }
    }
    return changed;
  }

  std::size_t mappedNode(aser::NodeID source_node) const {
    const auto it = node_map_.find(source_node);
    if (it == node_map_.end()) {
      throw std::out_of_range("Aser node has not been synchronized");
    }
    return it->second;
  }

  std::optional<aser::NodeID> sourceNode(std::size_t mapped_node) const {
    const auto it = reverse_node_map_.find(mapped_node);
    if (it == reverse_node_map_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

private:
  struct EdgeKey {
    aser::NodeID source = 0;
    aser::NodeID target = 0;
    aser::Constraints kind = aser::Constraints::copy;

    bool operator==(const EdgeKey &other) const {
      return source == other.source && target == other.target &&
             kind == other.kind;
    }
  };

  struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey &edge) const {
      const auto kind = static_cast<std::uint8_t>(edge.kind);
      return static_cast<std::size_t>(edge.source ^ (edge.target << 1U) ^
                                      (static_cast<aser::NodeID>(kind) << 3U));
    }
  };

  std::pair<AliasConstraintEdgeKind, std::optional<std::uint32_t>>
  classify(const Node &source, const Node &target,
           aser::Constraints constraint) const {
    switch (constraint) {
    case aser::Constraints::load:
      return {AliasConstraintEdgeKind::Load, std::nullopt};
    case aser::Constraints::store:
      return {AliasConstraintEdgeKind::Store, std::nullopt};
    case aser::Constraints::copy:
      return {AliasConstraintEdgeKind::Copy, std::nullopt};
    case aser::Constraints::addr_of:
      return {AliasConstraintEdgeKind::Addr, std::nullopt};
    case aser::Constraints::offset: {
      auto attribute = resolve_offset_(source, target);
      return {attribute ? AliasConstraintEdgeKind::NormalGep
                        : AliasConstraintEdgeKind::VariantGep,
              attribute};
    }
    }
    throw std::logic_error("Unknown Aser constraint kind");
  }

  const Graph &source_;
  AliasClient &client_;
  OffsetResolver resolve_offset_;
  std::unordered_map<aser::NodeID, std::size_t> node_map_;
  std::unordered_map<std::size_t, aser::NodeID> reverse_node_map_;
  std::unordered_set<EdgeKey, EdgeKeyHash> seen_;
};

} // namespace lotus::cfl::classical
