/**
 * @file SummaryQuery.h
 * @brief Function-scoped PDG summaries.
 *
 * SummaryQuery extracts compact interprocedural facts from the PDG for a
 * single function. These summaries are consumed directly by users and also act
 * as a reusable foundation for higher-level analyses such as ImpactQuery and
 * ResourceFlowQuery.
 */

#pragma once

#include "IR/PDG/Analysis/PDGQueryCore.h"

namespace pdg {

/// Individual summary buckets that can be requested or filtered in the CLI.
enum class SummaryKind {
  All,
  InputToReturn,
  InputToGlobalWrite,
  InputToCallsite,
  GlobalReaders,
  GlobalWriters,
  ControlPredicates,
  ReachableCalls,
  ResourceKinds
};

/// Controls summary extraction density and bucket filtering.
struct SummaryPolicy {
  SummaryKind kind = SummaryKind::All;
  size_t max_witnesses_per_bucket = 8;
};

/// One summarized source/target relationship plus its witness paths.
struct SummaryBucketEntry {
  Node *source = nullptr;
  Node *target = nullptr;
  std::vector<PDGWitnessPath> witness_paths;
};

/// Summary facts computed for a single function.
struct FunctionSummary {
  const llvm::Function *function = nullptr;
  std::vector<SummaryBucketEntry> input_to_return;
  std::vector<SummaryBucketEntry> input_to_global_write;
  std::vector<SummaryBucketEntry> input_to_callsite;
  std::vector<SummaryBucketEntry> global_readers;
  std::vector<SummaryBucketEntry> global_writers;
  std::vector<SummaryBucketEntry> control_predicates;
  std::vector<SummaryBucketEntry> reachable_calls;
  std::set<ResourceKind> may_allocate_resource_kinds;
  std::set<ResourceKind> may_release_resource_kinds;
};

/// Result wrapper for SummaryQuery.
struct SummaryQueryResult {
  FunctionSummary summary;
  PDGQueryDiagnostics diagnostics;

  bool empty() const { return summary.function == nullptr; }
};

/// Builds function-scoped summaries from the PDG.
class SummaryQuery {
public:
  explicit SummaryQuery(ProgramGraph &pdg) : pdg_(pdg) {}

  SummaryQueryResult summarize(
      const PDGCriteria &criteria, const SummaryPolicy &policy = SummaryPolicy(),
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

  SummaryQueryResult summarize(
      const llvm::Function &function,
      const SummaryPolicy &policy = SummaryPolicy(),
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

private:
  ProgramGraph &pdg_;
  mutable std::unordered_map<std::string, SummaryQueryResult> summary_cache_;
  mutable unsigned long long cache_epoch_ = 0;
};

} // namespace pdg
