#pragma once

#include "IR/PDG/Analysis/QueryCore.h"

namespace pdg::query_detail {

using NodeSet = PDGQueryResult::NodeSet;
using EdgeSet = PDGQueryResult::EdgeSet;

struct TraversalOutcome {
  NodeSet nodes;
  EdgeSet edges;
  std::unordered_map<Node *, std::set<Node *>> predecessors;
  std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>>
      predecessor_edges;
  std::unordered_map<Node *, size_t> distances;
};

std::string toLower(std::string value);
bool isEdgeAllowed(EdgeType type, const std::set<EdgeType> &allowed);
std::string pointerKey(const void *value);
bool sourceLocationMatches(const PDGSourceLocation &wanted,
                           const llvm::Instruction &inst);
NodeSet scopeNodes(ProgramGraph &pdg, const PDGQueryScope &scope);
EdgeSet collectInducedEdges(const NodeSet &nodes,
                            const std::set<EdgeType> &edge_types);
std::string criteriaCacheKey(const PDGCriteria &criteria,
                             const llvm::Module *module);
std::string optionsCacheKey(const PDGQueryOptions &options);
std::string pathKey(const PDGWitnessPath &path);
std::string functionNameForNode(Node *node);
std::string sourceKeyForNode(Node *node);
std::string stringifyValue(const llvm::Value *value);
NodeSet resolvePropertyCriteria(ProgramGraph &pdg, const llvm::Module &module,
                                const PropertySpec &spec);
TraversalOutcome traverseGraph(ProgramGraph &pdg, const NodeSet &start_nodes,
                               const std::set<EdgeType> &edge_types,
                               const PDGQueryOptions &options, bool forward,
                               PDGQueryDiagnostics &diagnostics);
PDGQueryResult resultFromTraversal(const NodeSet &criteria_nodes,
                                   const TraversalOutcome &outcome,
                                   PDGQueryDiagnostics diagnostics,
                                   bool explain);
void syncCacheEpoch(
    ProgramGraph &pdg, unsigned long long &cache_epoch,
    std::unordered_map<std::string, PDGQueryResult> &result_cache,
    std::unordered_map<std::string, NodeSet> *criteria_cache,
    std::unordered_map<std::string,
                       std::unordered_map<Node *, std::set<Node *>>>
        *closure_cache);
EdgeType edgeBetween(Node *from, Node *to);

} // namespace pdg::query_detail
