#include "IR/PDG/Analysis/SliceQuery.h"

#include "IR/PDG/Analysis/Internal/QuerySupport.h"
#include "IR/PDG/Analysis/Query.h"

namespace pdg {
using namespace llvm;
using namespace query_detail;

SliceQuery::SliceQuery(ProgramGraph &pdg) : pdg_(pdg) {}

PDGQueryResult SliceQuery::forward(const PDGCriteria &criteria,
                                   const PDGQueryOptions &options,
                                   const Module *module) const {
  syncCacheEpoch(pdg_, cache_epoch_, result_cache_, &criteria_cache_, nullptr);
  const std::string criteria_key = criteriaCacheKey(criteria, module);
  const std::string cache_key =
      "forward|" + criteria_key + "|" + optionsCacheKey(options);

  if (options.cache_policy == PDGCachePolicy::Enabled) {
    std::unordered_map<std::string, PDGQueryResult>::const_iterator cached =
        result_cache_.find(cache_key);
    if (cached != result_cache_.end()) {
      PDGQueryResult result = cached->second;
      result.diagnostics.summary_cache_hits++;
      return result;
    }
  }

  PDGCriteriaResolver resolver(pdg_);
  PDGQueryResult resolved = resolver.resolve(criteria, options, module);
  NodeSet criteria_nodes = resolved.nodes;
  TraversalOutcome outcome = traverseGraph(
      pdg_, criteria_nodes, edgeTypesForPreset(options.edge_preset), options,
      true, resolved.diagnostics);
  PDGQueryResult result = resultFromTraversal(
      criteria_nodes, outcome, resolved.diagnostics, options.explain);

  if (options.cache_policy == PDGCachePolicy::Enabled)
    result_cache_[cache_key] = result;
  return result;
}

PDGQueryResult SliceQuery::backward(const PDGCriteria &criteria,
                                    const PDGQueryOptions &options,
                                    const Module *module) const {
  syncCacheEpoch(pdg_, cache_epoch_, result_cache_, &criteria_cache_, nullptr);
  const std::string criteria_key = criteriaCacheKey(criteria, module);
  const std::string cache_key =
      "backward|" + criteria_key + "|" + optionsCacheKey(options);

  if (options.cache_policy == PDGCachePolicy::Enabled) {
    std::unordered_map<std::string, PDGQueryResult>::const_iterator cached =
        result_cache_.find(cache_key);
    if (cached != result_cache_.end()) {
      PDGQueryResult result = cached->second;
      result.diagnostics.summary_cache_hits++;
      return result;
    }
  }

  PDGCriteriaResolver resolver(pdg_);
  PDGQueryResult resolved = resolver.resolve(criteria, options, module);
  NodeSet criteria_nodes = resolved.nodes;
  TraversalOutcome outcome = traverseGraph(
      pdg_, criteria_nodes, edgeTypesForPreset(options.edge_preset), options,
      false, resolved.diagnostics);
  PDGQueryResult result = resultFromTraversal(
      criteria_nodes, outcome, resolved.diagnostics, options.explain);

  if (options.cache_policy == PDGCachePolicy::Enabled)
    result_cache_[cache_key] = result;
  return result;
}

PDGQueryResult SliceQuery::chop(const PDGCriteria &sources,
                                const PDGCriteria &targets,
                                const PDGQueryOptions &options,
                                const Module *module) const {
  PDGQueryResult source_slice = forward(sources, options, module);
  PDGQueryResult target_slice = backward(targets, options, module);

  PDGQueryResult result;
  result.criteria_nodes = source_slice.criteria_nodes;
  result.criteria_nodes.insert(target_slice.criteria_nodes.begin(),
                               target_slice.criteria_nodes.end());

  for (NodeSet::const_iterator it = source_slice.nodes.begin();
       it != source_slice.nodes.end(); ++it) {
    if (target_slice.nodes.count(*it) != 0)
      result.nodes.insert(*it);
  }

  result.edges = collectInducedEdges(result.nodes,
                                     edgeTypesForPreset(options.edge_preset));
  result.predecessors = source_slice.predecessors;
  result.distances = source_slice.distances;
  result.diagnostics = source_slice.diagnostics;
  if (options.explain) {
    DependenceQuery dep(pdg_);
    std::vector<PDGWitnessPath> paths =
        dep.allShortestPaths(sources, targets, options, module);
    for (size_t i = 0; i < paths.size(); ++i) {
      paths[i].kind = PDGWitnessPathKind::Chop;
      result.witness_paths.push_back(paths[i]);
    }
  }
  return result;
}

} // namespace pdg
