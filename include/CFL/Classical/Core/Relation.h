#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <llvm/ADT/STLFunctionalExtras.h>

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
  virtual void
  forEachSuccessor(SymbolId symbol, NodeId source,
                   llvm::function_ref<void(NodeId)> visitor) const = 0;
  virtual void
  forEachPredecessor(SymbolId symbol, NodeId target,
                     llvm::function_ref<void(NodeId)> visitor) const = 0;
  virtual std::vector<RelationEdge> edges() const = 0;
  virtual std::vector<RelationEdge> edges(SymbolId symbol) const = 0;
  virtual std::size_t edgeCount() const = 0;
  virtual std::size_t edgeCount(SymbolId symbol) const = 0;
  virtual std::size_t approximateMemoryBytes() const = 0;
};

enum class RelationBackend {
  SparseSets,
  SparseBitVectors,
};

std::unique_ptr<Relation> createRelation(RelationBackend backend,
                                         std::size_t node_count);

} // namespace lotus::cfl::classical
