/**
 * @file ResourceFlowQuery.h
 * @brief Built-in resource acquire/release tracking over the PDG.
 *
 * ResourceFlowQuery provides a conservative v1 resource analysis for common
 * C-style APIs such as malloc/free, fopen/fclose, and open/close. It uses PDG
 * value-flow and function summaries rather than ownership-generic inference.
 */

#pragma once

#include "IR/PDG/Analysis/PDGQueryCore.h"

namespace pdg {

/// Role of a program point along a resource lifecycle.
enum class ResourceEventKind { Acquire, Use, Transfer, Release, Exit };

/// Filtering options for built-in resource families.
struct ResourcePolicy {
  ResourceKind resource_kind = ResourceKind::Unknown;
  bool include_interprocedural = true;
};

/// A single acquire/use/transfer/release event discovered by the analysis.
struct ResourceEvent {
  ResourceKind resource_kind = ResourceKind::Unknown;
  ResourceEventKind event_kind = ResourceEventKind::Use;
  Node *site = nullptr;
  std::string api_name;
};

/// Witness path describing one resource lifecycle.
struct ResourcePath {
  ResourceKind resource_kind = ResourceKind::Unknown;
  Node *acquire_site = nullptr;
  Node *release_site = nullptr;
  bool released = false;
  bool orphaned = false;
  bool double_release_candidate = false;
  std::vector<PDGWitnessPath> witness_paths;
};

/// Aggregate result of resource-flow analysis.
struct ResourceFlowQueryResult {
  std::vector<ResourceEvent> acquire_sites;
  std::vector<ResourceEvent> release_sites;
  std::vector<ResourcePath> orphaned_resources;
  std::vector<ResourcePath> double_release_candidates;
  std::vector<ResourcePath> resource_paths;
  std::map<ResourceKind, size_t> resource_kind_counts;
  PDGQueryDiagnostics diagnostics;
};

/// Tracks built-in resource families through PDG value-flow edges.
class ResourceFlowQuery {
public:
  explicit ResourceFlowQuery(ProgramGraph &pdg) : pdg_(pdg) {}

  ResourceFlowQueryResult analyze(
      const PDGCriteria &criteria = PDGCriteria(),
      const ResourcePolicy &policy = ResourcePolicy(),
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

private:
  ProgramGraph &pdg_;
};

} // namespace pdg
