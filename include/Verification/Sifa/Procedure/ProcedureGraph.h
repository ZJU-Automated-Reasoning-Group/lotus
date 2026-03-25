//===-- Verification/Sifa/Procedure/ProcedureGraph.h ----------------------===//
//
// Intraprocedural CFG wrapper as a labeled graph suitable for path expressions.
//
// Nodes: explicit program points within basic blocks (plus a nullptr EXIT sink
// used as a sentinel). Labels: Transition (instruction segments, markers, or
// synthetic call/return summary edges).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDUREGRAPH_H
#define LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDUREGRAPH_H

#include "Utils/Algorithms/PathExpressions/LabeledGraph.h"
#include "Verification/Sifa/Cfg/Transition.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace llvm {
class BasicBlock;
class CallBase;
class Function;
class Instruction;
} // namespace llvm

namespace lotus {
namespace sifa {

/// A program point inside a basic block used as a path-expression node.
///
/// `ordinal == 0` denotes the block-entry program point (after PHIs, before the
/// first non-PHI instruction). Higher ordinals denote intra-block points that
/// occur immediately after an implemented direct call.
struct ProgramPoint {
  llvm::BasicBlock *block = nullptr;
  std::uint32_t ordinal = 0;
};

/// Side-table information for a Transition id.
///
/// The ProcedureGraph assigns a dense id to every labeled edge it creates.
/// That id is stored in Transition::id and indexes into this vector, allowing
/// other components (e.g. debug/logging/analysis) to recover the underlying
/// CFG endpoints and, for synthetic interprocedural edges, the callee/call
/// site.
struct TransitionInfo {
  llvm::BasicBlock *source = nullptr;
  llvm::BasicBlock *target = nullptr;
  std::uint32_t sourceOrdinal = 0;
  std::uint32_t targetOrdinal = 0;
  llvm::Function *callee = nullptr; // non-null for ReturnSummary / EnterCall
  const llvm::Instruction *segmentStart = nullptr;
  const llvm::Instruction *stopBefore = nullptr;
  const llvm::CallBase *call = nullptr;
};

/// Intraprocedural procedure graph used as input to the path-expression engine.
///
/// This is a thin wrapper around an LLVM function's CFG that:
/// - assigns stable, dense edge ids (used as the alphabet for regex/DAG),
/// - represents all "returns" using one synthetic EXIT sink node (`nullptr`),
/// - optionally includes synthetic call/return summary edges used by
///   interprocedural interpretation.
///
/// Notes on the EXIT sentinel:
/// - The graph's node type is `ProgramPoint*`; we additionally allow `nullptr`
///   to appear as a *target* to represent procedure exit.
/// - `nullptr` is *not* added as a node via addNode(); it is only used as an
///   edge target and as `exitNode_`.
class ProcedureGraph {
public:
  using Node = ProgramPoint *;
  using Label = Transition;
  using Graph = lotus::pathexpressions::GenericLabeledGraph<Node, Label>;

  /// Build a procedure graph from an LLVM function's full CFG.
  explicit ProcedureGraph(const llvm::Function &F);

  /// Build an empty procedure graph. Use getOrCreateBlockEntryNode()/addEdge()
  /// (or ProcedureGraphBuilder) to add the rest.
  ProcedureGraph() = default;

  Node getEntryNode() const;
  Node getExitNode() const;
  void setEntryNode(Node n);

  Node getBlockEntryNode(const llvm::BasicBlock &bb) const;
  Node getOrCreateBlockEntryNode(llvm::BasicBlock *bb);
  Node createInternalNode(llvm::BasicBlock *bb);
  void addNode(Node n);
  void addEdge(Node src, Node dst,
               const llvm::Instruction *segmentStart = nullptr,
               const llvm::Instruction *stopBefore = nullptr);

  /// Adds a ReturnSummary edge from \p src to \p dst for call to \p callee.
  /// Used for interprocedural Sifa (path expression can include call/return).
  void addReturnSummaryEdge(Node src, Node dst, const llvm::Function *callee,
                            const llvm::CallBase *callSite = nullptr);
  /// Adds an EnterCall edge from \p src to \p dst (callee entry) for \p callee.
  void addEnterCallEdge(Node src, Node dst, const llvm::Function *callee,
                        const llvm::CallBase *callSite = nullptr);

  const Graph &graph() const;
  const std::vector<TransitionInfo> &transitions() const;

private:
  Node entryNode_ = nullptr;
  /// EXIT sentinel. For graphs built from an LLVM function this is always null,
  /// and procedure exits are represented by edges `bb -> nullptr`.
  Node exitNode_ = nullptr;
  std::vector<std::unique_ptr<ProgramPoint>> ownedNodes_;
  std::unordered_map<const llvm::BasicBlock *, Node> blockEntryNodes_;
  std::unordered_map<const llvm::BasicBlock *, std::uint32_t> nextOrdinal_;
  Graph graph_;
  std::vector<TransitionInfo> transitions_;
  struct NodePairHash {
    std::size_t operator()(const std::pair<Node, Node> &p) const;
  };

  std::unordered_map<std::pair<Node, Node>, std::uint32_t, NodePairHash>
      edgeToId_;

  Node createNode(llvm::BasicBlock *bb, std::uint32_t ordinal);
  Transition addTransition(Node src, Node dst,
                           const llvm::Instruction *segmentStart,
                           const llvm::Instruction *stopBefore);
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDUREGRAPH_H
