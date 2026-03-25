/**
 * @file ImpactQuery.h
 * @brief Direct and transitive impact analysis over the PDG.
 *
 * ImpactQuery answers "what is affected by these criteria?" and ranks the
 * affected nodes/functions using existing PDG reachability, path, and diff
 * services.
 */

#pragma once

#include "IR/PDG/Analysis/PDGQueryCore.h"

namespace pdg {

/// Ranking and diff behavior for impact analysis.
struct ImpactPolicy {
  bool changed_only = false;
  size_t max_ranked_impacts = 0;
};

/// One ranked impacted node with ordering metadata.
struct ImpactRankItem {
  Node *node = nullptr;
  size_t shortest_distance = 0;
  size_t interprocedural_crossings = 0;
  size_t path_count = 0;
  std::string stable_key;
};

/// High-level impact report derived from PDG traversal.
struct ImpactQueryResult {
  PDGQueryResult directly_impacted_nodes;
  PDGQueryResult transitively_impacted_nodes;
  std::set<std::string> impacted_functions;
  std::set<std::string> impacted_source_locations;
  std::unordered_map<std::string, size_t> boundary_crossings;
  std::vector<ImpactRankItem> ranked_impacts;
  std::unordered_map<std::string, std::vector<PDGWitnessPath>>
      function_explanations;
  DiffQueryResult changed_only_diff;
  PDGQueryDiagnostics diagnostics;
};

/// Computes direct/transitive impact and optional changed-only impact diffs.
class ImpactQuery {
public:
  explicit ImpactQuery(ProgramGraph &pdg) : pdg_(pdg) {}

  ImpactQueryResult analyze(
      const PDGCriteria &criteria, const ImpactPolicy &policy = ImpactPolicy(),
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

  ImpactQueryResult analyzeAgainstBaseline(
      const PDGCriteria &criteria, const PDGCriteria &baseline_criteria,
      const ImpactPolicy &policy = ImpactPolicy(),
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

private:
  ProgramGraph &pdg_;
};

} // namespace pdg
