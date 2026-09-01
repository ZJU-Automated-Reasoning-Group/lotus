#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace lotus::cfl::classical {

using NodeId = std::size_t;
using SymbolId = std::uint32_t;

struct RelationEdge {
  SymbolId symbol = 0;
  NodeId source = 0;
  NodeId target = 0;
};

/// Solver-neutral storage for terminal and derived CFL relations.
class Relation {
public:
  virtual ~Relation() = default;

  virtual void ensureNodeCount(std::size_t node_count) = 0;
  virtual bool add(SymbolId symbol, NodeId source, NodeId target) = 0;
  virtual bool contains(SymbolId symbol, NodeId source,
                        NodeId target) const = 0;
  virtual std::vector<NodeId> successors(SymbolId symbol,
                                         NodeId source) const = 0;
  virtual std::vector<NodeId> predecessors(SymbolId symbol,
                                           NodeId target) const = 0;
  virtual std::vector<RelationEdge> edges() const = 0;
  virtual std::size_t edgeCount() const = 0;
  virtual std::size_t approximateMemoryBytes() const = 0;
};

enum class RelationBackend {
  SparseSets,
  SparseBitVectors,
};

std::unique_ptr<Relation> createRelation(RelationBackend backend,
                                         std::size_t node_count);

} // namespace lotus::cfl::classical
