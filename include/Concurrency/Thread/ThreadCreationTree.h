/**
 * @file ThreadCreationTree.h
 * @brief Context-bounded thread call graph and thread creation tree.
 */
#pragma once

#include "Concurrency/MHP/IMHPAnalysis.h"

#include <cstddef>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llvm {
class CallBase;
class Function;
class Instruction;
class Module;
class Value;
class raw_ostream;
} // namespace llvm

class ThreadAPI;

namespace lotus::analysis {

using InstructionScope = std::unordered_set<const llvm::Instruction *>;

class ThreadCallGraph {
public:
  enum class EdgeKind { Call, Fork };
  struct Edge {
    const llvm::Function *source = nullptr;
    const llvm::Function *target = nullptr;
    const llvm::CallBase *callSite = nullptr;
    EdgeKind kind = EdgeKind::Call;
  };

  ThreadCallGraph(llvm::Module &module, ThreadAPI &threadAPI,
                  const InstructionScope *scope = nullptr);

  const std::vector<Edge> &outgoing(const llvm::Function *function) const;
  const std::vector<Edge> &edges() const { return edges_; }
  const std::vector<const llvm::CallBase *> &joinSites() const {
    return joinSites_;
  }

private:
  bool retained(const llvm::Instruction *instruction) const;
  void build();

  llvm::Module *module_;
  ThreadAPI *threadAPI_;
  const InstructionScope *scope_;
  std::vector<Edge> edges_;
  std::unordered_map<const llvm::Function *, std::vector<Edge>> outgoing_;
  std::vector<const llvm::CallBase *> joinSites_;
};

class ThreadCreationTree {
public:
  using ThreadID = std::size_t;
  static constexpr ThreadID InvalidThread =
      std::numeric_limits<ThreadID>::max();

  struct Node {
    ThreadID id = InvalidThread;
    ThreadID parent = InvalidThread;
    const llvm::Function *entry = nullptr;
    const llvm::CallBase *forkSite = nullptr;
    std::vector<const llvm::CallBase *> context;
    std::unordered_set<const llvm::Function *> reachableFunctions;
    bool multiInstance = false;
    bool inCycle = false;
  };

  struct ForkRelation {
    ThreadID parent = InvalidThread;
    ThreadID child = InvalidThread;
    const llvm::CallBase *site = nullptr;
    const llvm::Function *target = nullptr;
  };

  struct Statistics {
    std::size_t nodes = 0;
    std::size_t forkRelations = 0;
    std::size_t joinSites = 0;
    std::size_t resolvedJoins = 0;
    std::size_t multiInstanceNodes = 0;
    std::size_t cyclicNodes = 0;
  };

  ThreadCreationTree(llvm::Module &module, ThreadAPI &threadAPI,
                     std::size_t contextLimit = 2,
                     const InstructionScope *scope = nullptr);

  const std::vector<Node> &nodes() const { return nodes_; }
  const std::vector<ForkRelation> &forkRelations() const {
    return forkRelations_;
  }
  const ThreadCallGraph &threadCallGraph() const { return callGraph_; }
  const Statistics &statistics() const { return stats_; }

  std::vector<ThreadID>
  instancesForFunction(const llvm::Function *function) const;
  std::vector<ThreadID> joinedThreads(const llvm::CallBase *joinSite) const;
  std::vector<const llvm::Function *>
  joinedFunctions(const llvm::CallBase *joinSite) const;

  bool mayOverlap(const llvm::Instruction *lhs,
                  const llvm::Instruction *rhs) const;
  ThreadID uniqueThreadFor(const llvm::Instruction *instruction) const;

private:
  void build();
  void expandThread(ThreadID id,
                    std::unordered_set<const llvm::Function *> ancestors);
  void walkCalls(ThreadID id, const llvm::Function *function,
                 std::vector<const llvm::CallBase *> context,
                 std::unordered_set<const llvm::Function *> &activeCalls);
  ThreadID addChild(ThreadID parent, const ThreadCallGraph::Edge &edge,
                    const std::vector<const llvm::CallBase *> &context,
                    bool repeatedContext);
  void resolveJoins();
  const llvm::Value *canonicalThreadHandle(const llvm::Value *value) const;
  bool forkMayRepeat(const llvm::CallBase *forkSite) const;
  bool joinedBefore(ThreadID child, const llvm::Instruction *instruction) const;

  llvm::Module *module_;
  ThreadAPI *threadAPI_;
  std::size_t contextLimit_;
  const InstructionScope *scope_;
  ThreadCallGraph callGraph_;
  std::vector<Node> nodes_;
  std::vector<ForkRelation> forkRelations_;
  std::unordered_map<const llvm::Function *, std::vector<ThreadID>>
      functionInstances_;
  std::unordered_map<const llvm::Value *, std::vector<ThreadID>> handleThreads_;
  std::unordered_map<const llvm::CallBase *, std::vector<ThreadID>>
      joinThreads_;
  Statistics stats_;
};

/// Main-phase MHP view backed by a rebuilt sliced TCT and the conservative
/// whole-program synchronization oracle.
class TCTMHPAnalysis final : public mhp::IMHPAnalysis {
public:
  TCTMHPAnalysis(const mhp::IMHPAnalysis &base, const ThreadCreationTree &tree,
                 const InstructionScope *scope = nullptr)
      : base_(&base), tree_(&tree), scope_(scope) {}

  void analyze() override {}
  bool mayHappenInParallel(const llvm::Instruction *lhs,
                           const llvm::Instruction *rhs) const override;
  bool isPrecomputedMHP(const llvm::Instruction *lhs,
                        const llvm::Instruction *rhs) const override;
  mhp::InstructionSet
  getParallelInstructions(const llvm::Instruction *instruction) const override;
  bool mustBeSequential(const llvm::Instruction *lhs,
                        const llvm::Instruction *rhs) const override;
  mhp::ThreadID
  getThreadID(const llvm::Instruction *instruction) const override;
  mhp::InstructionSet getInstructionsInThread(mhp::ThreadID id) const override;
  std::size_t getMhpPairCount() const override;
  void printStatistics(llvm::raw_ostream &os) const override;
  void printResults(llvm::raw_ostream &os) const override;

private:
  bool retained(const llvm::Instruction *instruction) const;

  const mhp::IMHPAnalysis *base_;
  const ThreadCreationTree *tree_;
  const InstructionScope *scope_;
};

} // namespace lotus::analysis
