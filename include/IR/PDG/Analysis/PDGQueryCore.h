/**
 * @file PDGQueryCore.h
 * @brief Shared types and core PDG query services.
 *
 * This header contains the common result/configuration vocabulary used by all
 * PDG analyses plus the foundational services that other higher-level queries
 * build on:
 * - criteria resolution
 * - slicing
 * - dependence/path queries
 * - dataflow convenience queries
 * - transform legality/scheduling helpers
 * - structural diffing
 *
 * Higher-level analyses such as SummaryQuery, ImpactQuery, and
 * ResourceFlowQuery are declared in their own headers to keep this file focused
 * on reusable query infrastructure.
 */

#pragma once

#include "IR/PDG/Analysis/PropertyBasedSlicing.h"
#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Core/PDGEdge.h"
#include "IR/PDG/Core/PDGEnums.h"
#include "IR/PDG/Core/PDGNode.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace pdg {

class CypherResult;

/// Named dependence-edge presets exposed to users of the query layer.
enum class PDGEdgePreset {
  All,
  Data,
  Control,
  Parameter,
  Interprocedural,
  ValueFlow,
  TransformLegality
};

/// Traversal mode for call/return matching.
enum class PDGContextMode { ContextInsensitive, ContextSensitive };

enum class PDGCachePolicy { Enabled, Disabled };

enum class SliceFlavor { Full, Thin };

enum class PDGWitnessPathKind {
  Slice,
  ShortestPath,
  AllShortestPath,
  Chop,
  TransformBlocker
};

/// Shared traversal guardrails. Zero means "unbounded".
struct PDGTraversalLimits {
  size_t max_depth = 0;
  size_t max_states = 0;
  size_t max_paths = 0;
  size_t max_path_length = 0;
  size_t max_stack_depth = 0;
};

struct PDGSourceLocation {
  std::string file;
  unsigned line = 0;
  unsigned column = 0;
};

/// Diagnostic counters and truncation markers emitted by query execution.
struct PDGQueryDiagnostics {
  bool depth_limit_hit = false;
  bool state_limit_hit = false;
  bool path_limit_hit = false;
  bool path_length_limit_hit = false;
  bool stack_depth_limit_hit = false;
  size_t explored_states = 0;
  size_t max_depth_reached = 0;
  size_t max_stack_depth_reached = 0;
  size_t summary_cache_hits = 0;
  size_t summary_cache_misses = 0;
  size_t closure_cache_hits = 0;
  size_t closure_cache_misses = 0;
  size_t criteria_cache_hits = 0;
  size_t criteria_cache_misses = 0;
  std::vector<std::string> unresolved_criteria;
  std::vector<std::string> notes;
};

/// A concrete explanatory path returned by a query.
struct PDGWitnessPath {
  PDGWitnessPathKind kind = PDGWitnessPathKind::Slice;
  std::vector<Node *> nodes;
  std::vector<EdgeType> edge_types;
  std::vector<Node *> call_stack;
};

/// Common carrier for graph-shaped query results.
struct PDGQueryResult {
  using NodeSet = std::set<Node *>;
  using EdgeSet = std::set<Edge *>;
  using PredecessorMap = std::unordered_map<Node *, std::set<Node *>>;
  using DistanceMap = std::unordered_map<Node *, size_t>;

  NodeSet criteria_nodes;
  NodeSet nodes;
  EdgeSet edges;
  PredecessorMap predecessors;
  DistanceMap distances;
  std::vector<PDGWitnessPath> witness_paths;
  PDGQueryDiagnostics diagnostics;

  bool empty() const { return nodes.empty() && edges.empty(); }
};

/// Scope restriction applied before or during traversal.
struct PDGQueryScope {
  enum class Kind { WholeGraph, NodeSet, Function, QueryResult };

  Kind kind = Kind::WholeGraph;
  PDGQueryResult::NodeSet nodes;
  const llvm::Function *function = nullptr;
  const PDGQueryResult *query_result = nullptr;

  static PDGQueryScope wholeGraph() { return PDGQueryScope{}; }

  static PDGQueryScope nodeSet(const PDGQueryResult::NodeSet &value) {
    PDGQueryScope scope;
    scope.kind = Kind::NodeSet;
    scope.nodes = value;
    return scope;
  }

  static PDGQueryScope functionScope(const llvm::Function &value) {
    PDGQueryScope scope;
    scope.kind = Kind::Function;
    scope.function = &value;
    return scope;
  }

  static PDGQueryScope queryResultScope(const PDGQueryResult &value) {
    PDGQueryScope scope;
    scope.kind = Kind::QueryResult;
    scope.query_result = &value;
    return scope;
  }
};

struct CypherSelection {
  std::string query;
  std::string binding;
};

/// Seed specification accepted by the query layer.
struct PDGCriteria {
  using NodeSet = PDGQueryResult::NodeSet;

  NodeSet nodes;
  std::vector<llvm::Value *> values;
  std::vector<std::string> function_names;
  std::vector<std::string> callee_names;
  std::vector<PDGSourceLocation> source_locations;
  std::vector<PropertySpec> property_specs;
  std::vector<CypherSelection> cypher_selections;

  bool empty() const {
    return nodes.empty() && values.empty() && function_names.empty() &&
           callee_names.empty() && source_locations.empty() &&
           property_specs.empty() && cypher_selections.empty();
  }
};

/// Execution options shared by all PDG query services.
struct PDGQueryOptions {
  PDGEdgePreset edge_preset = PDGEdgePreset::All;
  PDGQueryScope scope = PDGQueryScope::wholeGraph();
  PDGContextMode context_mode = PDGContextMode::ContextInsensitive;
  PDGTraversalLimits limits;
  PDGCachePolicy cache_policy = PDGCachePolicy::Enabled;
  bool explain = true;
  SliceFlavor slice_flavor = SliceFlavor::Full;
};

enum class ResourceKind {
  Unknown,
  Heap,
  File,
  FileDescriptor,
  Directory
};

/// Resolves high-level criteria into concrete PDG seed nodes.
class PDGCriteriaResolver {
public:
  explicit PDGCriteriaResolver(ProgramGraph &pdg) : pdg_(pdg) {}

  PDGQueryResult resolve(const PDGCriteria &criteria,
                         const PDGQueryOptions &options,
                         const llvm::Module *module = nullptr) const;

private:
  ProgramGraph &pdg_;
};

/// Forward/backward slicing and chopping over the PDG.
class SliceQuery {
public:
  explicit SliceQuery(ProgramGraph &pdg);

  PDGQueryResult forward(const PDGCriteria &criteria,
                         const PDGQueryOptions &options = PDGQueryOptions(),
                         const llvm::Module *module = nullptr) const;

  PDGQueryResult backward(const PDGCriteria &criteria,
                          const PDGQueryOptions &options = PDGQueryOptions(),
                          const llvm::Module *module = nullptr) const;

  PDGQueryResult chop(const PDGCriteria &sources, const PDGCriteria &targets,
                      const PDGQueryOptions &options = PDGQueryOptions(),
                      const llvm::Module *module = nullptr) const;

private:
  ProgramGraph &pdg_;
  mutable std::unordered_map<std::string, PDGQueryResult> result_cache_;
  mutable std::unordered_map<std::string, PDGQueryResult::NodeSet>
      criteria_cache_;
  mutable unsigned long long cache_epoch_ = 0;
};

/// Reachability and shortest-path style dependence queries.
class DependenceQuery {
public:
  explicit DependenceQuery(ProgramGraph &pdg);

  PDGQueryResult reachability(
      const PDGCriteria &sources,
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

  PDGQueryResult shortestPath(
      const PDGCriteria &sources, const PDGCriteria &targets,
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

  std::vector<PDGWitnessPath> allShortestPaths(
      const PDGCriteria &sources, const PDGCriteria &targets,
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

  size_t distance(const PDGCriteria &sources, const PDGCriteria &targets,
                  const PDGQueryOptions &options = PDGQueryOptions(),
                  const llvm::Module *module = nullptr) const;

private:
  ProgramGraph &pdg_;
  mutable std::unordered_map<std::string,
                             std::unordered_map<Node *, std::set<Node *>>>
      closure_cache_;
  mutable unsigned long long cache_epoch_ = 0;
};

struct DefUseLink {
  Node *from = nullptr;
  Node *to = nullptr;
  EdgeType edge_type = EdgeType::DATA_DEF_USE;
};

struct ControllingCondition {
  Node *predicate = nullptr;
  EdgeType edge_type = EdgeType::CONTROLDEP_BR;
};

/// Dataflow-flavored queries built on top of PDG traversal.
class DataFlowQuery {
public:
  explicit DataFlowQuery(ProgramGraph &pdg) : pdg_(pdg) {}

  PDGQueryResult reachingDefinitions(
      const PDGCriteria &uses,
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

  std::vector<DefUseLink>
  defUseChain(Node &definition,
              const PDGQueryOptions &options = PDGQueryOptions()) const;

  std::vector<DefUseLink>
  useDefChain(Node &use,
              const PDGQueryOptions &options = PDGQueryOptions()) const;

  PDGQueryResult liveNodes(
      const PDGQueryOptions &options = PDGQueryOptions()) const;

  PDGQueryResult deadNodes(
      const PDGQueryOptions &options = PDGQueryOptions()) const;

  std::vector<ControllingCondition> immediateControllers(Node &node) const;

  PDGQueryResult allControllers(
      const PDGCriteria &criteria,
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

  PDGQueryResult controlRegion(
      const PDGCriteria &criteria,
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

private:
  ProgramGraph &pdg_;
};

/// Optional LLVM analyses used by transform-oriented PDG queries.
struct LLVMQueryContext {
  llvm::Function *function = nullptr;
  llvm::DominatorTree *dominator_tree = nullptr;
  llvm::PostDominatorTree *post_dominator_tree = nullptr;
  llvm::LoopInfo *loop_info = nullptr;
  llvm::MemorySSA *memory_ssa = nullptr;
};

struct MotionCheckResult {
  bool legal = false;
  Node *moving_node = nullptr;
  Node *anchor_node = nullptr;
  std::string reason;
  std::vector<Node *> blocking_path;
  std::vector<EdgeType> blocking_edge_types;
  PDGQueryDiagnostics diagnostics;
};

struct IndependenceCheckResult {
  bool independent = false;
  std::vector<Node *> witness_path_ab;
  std::vector<EdgeType> witness_edge_types_ab;
  std::vector<Node *> witness_path_ba;
  std::vector<EdgeType> witness_edge_types_ba;
  PDGQueryDiagnostics diagnostics;
};

/// Dependence-aware transform legality and scheduling helpers.
class TransformQuery {
public:
  explicit TransformQuery(ProgramGraph &pdg) : pdg_(pdg) {}

  MotionCheckResult canMoveEarlier(Node &moving_node, Node &anchor_node,
                                   const LLVMQueryContext &llvm_context,
                                   const PDGQueryOptions &options =
                                       PDGQueryOptions()) const;

  MotionCheckResult canMoveLater(Node &moving_node, Node &anchor_node,
                                 const LLVMQueryContext &llvm_context,
                                 const PDGQueryOptions &options =
                                     PDGQueryOptions()) const;

  IndependenceCheckResult independent(
      Node &a, Node &b, const LLVMQueryContext &llvm_context,
      const PDGQueryOptions &options = PDGQueryOptions()) const;

  PDGQueryResult readySet(const PDGQueryScope &scope,
                          const PDGQueryResult::NodeSet &scheduled,
                          const LLVMQueryContext &llvm_context,
                          const PDGQueryOptions &options =
                              PDGQueryOptions()) const;

  std::vector<PDGQueryResult::NodeSet>
  topologicalLevels(const PDGQueryScope &scope,
                    const LLVMQueryContext &llvm_context,
                    const PDGQueryOptions &options =
                        PDGQueryOptions()) const;

  std::vector<PDGQueryResult::NodeSet>
  stronglyConnectedComponents(const PDGQueryScope &scope,
                              const LLVMQueryContext &llvm_context,
                              const PDGQueryOptions &options =
                                  PDGQueryOptions()) const;

  size_t criticalPathLength(const PDGQueryScope &scope,
                            const LLVMQueryContext &llvm_context,
                            const PDGQueryOptions &options =
                                PDGQueryOptions()) const;

private:
  ProgramGraph &pdg_;
};

enum class DiffKind { Added, Removed, Preserved };

enum class NodeMatchStrategy { PointerIdentity, CanonicalSource };

struct NodeDiffEntry {
  Node *node = nullptr;
  DiffKind kind = DiffKind::Preserved;
};

struct EdgeDiffEntry {
  Edge *edge = nullptr;
  DiffKind kind = DiffKind::Preserved;
};

struct DiffImpactSummary {
  std::unordered_map<std::string, size_t> functions;
  std::unordered_map<std::string, size_t> source_locations;
};

/// Structural diff result between two PDG subgraphs.
struct DiffQueryResult {
  std::vector<NodeDiffEntry> node_diffs;
  std::vector<EdgeDiffEntry> edge_diffs;
  DiffImpactSummary impact_summary;
  PDGQueryDiagnostics diagnostics;

  bool isIdentical() const;
};

/// Structural differencing for PDG query results or explicit scopes.
class DiffQuery {
public:
  explicit DiffQuery(
      ProgramGraph &pdg,
      NodeMatchStrategy strategy = NodeMatchStrategy::PointerIdentity)
      : pdg_(pdg), strategy_(strategy) {}

  DiffQueryResult diff(const PDGQueryResult &before, const PDGQueryResult &after,
                       const PDGQueryOptions &options = PDGQueryOptions()) const;

  DiffQueryResult diff(const PDGQueryScope &before, const PDGQueryScope &after,
                       const PDGQueryOptions &options = PDGQueryOptions()) const;

private:
  ProgramGraph &pdg_;
  NodeMatchStrategy strategy_;
};

std::set<EdgeType> edgeTypesForPreset(PDGEdgePreset preset);

std::string describeNode(Node *node);

std::string stableNodeKey(Node *node);

std::string resourceKindName(ResourceKind kind);

} // namespace pdg
