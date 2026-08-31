/**
 * @file FlowSensitivePTA.h
 * @brief General sparse flow-sensitive inclusion-based pointer analysis.
 */
#pragma once

#include "Alias/Infrastructure/PtsSet/HashConsedPointsToSet.h"
#include "IR/GraphView.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::alias {

class FlowSensitivePTA {
public:
  using ObjectID = std::uint32_t;
  using NodeID = std::uint32_t;
  using PointsToSet = lotus::analysis::SVFGNodeBS;

  struct Config {
    using IndirectCallConnector =
        std::function<bool(const llvm::CallBase *, const llvm::Function *)>;
    PointsToSetBackend setBackend = PointsToSetBackend::Mutable;
    const lotus::analysis::FilteredSVFGView *scope = nullptr;
    IndirectCallConnector connectIndirectCall;
  };

  struct Statistics {
    std::size_t nodes = 0;
    std::size_t sccs = 0;
    std::size_t maxSccSize = 0;
    std::size_t nodeProcesses = 0;
    std::size_t topLevelFacts = 0;
    std::size_t memoryInFacts = 0;
    std::size_t memoryOutFacts = 0;
    std::size_t strongUpdates = 0;
    std::size_t weakUpdates = 0;
    std::size_t indirectCallEdges = 0;
    std::size_t hashConsedUniqueSets = 0;
    std::size_t hashConsedUnionCacheHits = 0;
  };

  explicit FlowSensitivePTA(const lotus::analysis::SVFG &graph);
  FlowSensitivePTA(const lotus::analysis::SVFG &graph, Config config);

  const Statistics &solve();
  const PointsToSet &pointsTo(const lotus::analysis::SVFGNode *node) const;
  std::optional<PointsToSet> pointsTo(const llvm::Value *value) const;
  const PointsToSet &memoryIn(const lotus::analysis::SVFGNode *node,
                              ObjectID object) const;
  const PointsToSet &memoryOut(const lotus::analysis::SVFGNode *node,
                               ObjectID object) const;
  std::optional<bool> mayAlias(const llvm::Value *lhs,
                               const llvm::Value *rhs) const;

  const Statistics &statistics() const { return stats_; }
  PointsToSetBackend setBackend() const { return config_.setBackend; }

private:
  struct StoredSet {
    PointsToSet mutableSet;
    HashConsedPointsToSetArena::SetID interned =
        HashConsedPointsToSetArena::EmptySet;
  };
  using MemoryState = std::unordered_map<ObjectID, StoredSet>;
  struct SCCInfo {
    std::vector<std::vector<const lotus::analysis::SVFGNode *>> components;
    std::unordered_map<NodeID, std::size_t> nodeToComponent;
    std::vector<std::vector<std::size_t>> successors;
  };

  bool inScope(const lotus::analysis::SVFGNode *node) const;
  StoredSet singleton(ObjectID object);
  const PointsToSet &materialize(const StoredSet &set) const;
  bool merge(StoredSet &destination, const StoredSet &source);
  bool assign(StoredSet &destination, const StoredSet &source);
  bool mergeState(MemoryState &destination, const MemoryState &source);
  bool assignState(MemoryState &destination, const MemoryState &source);
  const StoredSet &topSet(const lotus::analysis::SVFGNode *node) const;
  const MemoryState &outState(const lotus::analysis::SVFGNode *node) const;
  const MemoryState &inState(const lotus::analysis::SVFGNode *node) const;
  PointsToSet expandIndirectObjects(const PointsToSet &objects) const;
  void initializeRecursiveFunctions();
  void initializeGlobalMemory();
  StoredSet constantPointsTo(const llvm::Constant *constant);
  SCCInfo computeSCCs() const;
  bool transfer(const lotus::analysis::SVFGNode &node);
  StoredSet directInput(const lotus::analysis::SVFGNode &node);
  StoredSet pointerTargets(const llvm::Value *pointer);
  PointsToSet selectAccessTargets(const StoredSet &flowSensitiveTargets,
                                  const PointsToSet &preAnalysisTargets) const;
  bool isStrongUpdate(const PointsToSet &targets) const;
  bool resolveIndirectCalls(const lotus::analysis::SVFGNode &node,
                            const StoredSet &pointsTo);
  static const llvm::Value *accessPointer(const llvm::Instruction *instruction);

  const lotus::analysis::SVFG *graph_;
  Config config_;
  HashConsedPointsToSetArena arena_;
  std::unordered_map<NodeID, StoredSet> topLevelPointsTo_;
  std::unordered_map<NodeID, MemoryState> dfIn_;
  std::unordered_map<NodeID, MemoryState> dfOut_;
  MemoryState initialMemory_;
  MemoryState fallbackStoreFacts_;
  std::unordered_set<const llvm::Function *> recursiveFunctions_;
  bool topologyChanged_ = false;
  Statistics stats_;
};

} // namespace lotus::alias
