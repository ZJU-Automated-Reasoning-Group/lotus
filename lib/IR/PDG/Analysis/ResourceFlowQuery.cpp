/**
 * @file ResourceFlowQuery.cpp
 * @brief Implementation of built-in resource-flow analysis over the PDG.
 *
 * This file implements a conservative resource analysis for a fixed set of
 * common acquire/release APIs. It uses PDG value-flow for local matching and
 * SummaryQuery to reuse interprocedural resource behavior.
 */

#include "IR/PDG/Analysis/ResourceFlowQuery.h"

#include "IR/PDG/Analysis/PDGQuery.h"
#include "IR/PDG/Analysis/SummaryQuery.h"

#include "llvm/IR/Instructions.h"

#include <algorithm>

using namespace llvm;

namespace pdg {

namespace {

typedef PDGQueryResult::NodeSet NodeSet;

static bool isCallNode(Node *node) {
  return node != nullptr && node->getNodeType() == GraphNodeType::INST_FUNCALL;
}

static const Function *functionForNode(Node *node) {
  if (node == nullptr)
    return nullptr;
  if (node->getFunc() != nullptr)
    return node->getFunc();
  const Value *value = node->getValue();
  if (const Argument *argument = dyn_cast_or_null<Argument>(value))
    return argument->getParent();
  return dyn_cast_or_null<Function>(value);
}

static std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

/// Called function name for a callsite node, or empty when unavailable.
static std::string calleeName(Node *node) {
  if (!isCallNode(node))
    return "";
  const CallBase *call = dyn_cast_or_null<CallBase>(node->getValue());
  if (call == nullptr || call->getCalledFunction() == nullptr)
    return "";
  return call->getCalledFunction()->getName().str();
}

/// Built-in acquire API classification.
static ResourceKind resourceKindForAcquireName(const std::string &api_name) {
  const std::string lower = toLower(api_name);
  if (lower == "malloc" || lower == "calloc" || lower == "realloc")
    return ResourceKind::Heap;
  if (lower == "fopen")
    return ResourceKind::File;
  if (lower == "open" || lower == "socket")
    return ResourceKind::FileDescriptor;
  if (lower == "opendir")
    return ResourceKind::Directory;
  return ResourceKind::Unknown;
}

/// Built-in release API classification.
static ResourceKind resourceKindForReleaseName(const std::string &api_name) {
  const std::string lower = toLower(api_name);
  if (lower == "free")
    return ResourceKind::Heap;
  if (lower == "fclose")
    return ResourceKind::File;
  if (lower == "close")
    return ResourceKind::FileDescriptor;
  if (lower == "closedir")
    return ResourceKind::Directory;
  return ResourceKind::Unknown;
}

static ResourceKind resourceKindForNode(Node *node, bool release) {
  const std::string name = calleeName(node);
  return release ? resourceKindForReleaseName(name)
                 : resourceKindForAcquireName(name);
}

static bool resourceKindMatches(ResourceKind lhs, ResourceKind filter) {
  return filter == ResourceKind::Unknown || lhs == filter;
}

static NodeSet allGraphNodes(ProgramGraph &pdg) {
  NodeSet nodes;
  for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end(); ++it)
    if (*it != nullptr)
      nodes.insert(*it);
  return nodes;
}

static NodeSet scopeNodes(ProgramGraph &pdg, const PDGQueryScope &scope) {
  NodeSet nodes;
  if (scope.kind == PDGQueryScope::Kind::WholeGraph)
    return allGraphNodes(pdg);
  if (scope.kind == PDGQueryScope::Kind::NodeSet)
    return scope.nodes;
  if (scope.kind == PDGQueryScope::Kind::QueryResult && scope.query_result)
    return scope.query_result->nodes;
  if (scope.kind == PDGQueryScope::Kind::Function && scope.function) {
    for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end(); ++it) {
      Node *node = *it;
      if (node != nullptr && node->getFunc() == scope.function)
        nodes.insert(node);
    }
  }
  return nodes;
}

} // namespace

ResourceFlowQueryResult
ResourceFlowQuery::analyze(const PDGCriteria &criteria,
                           const ResourcePolicy &policy,
                           const PDGQueryOptions &options,
                           const Module *module) const {
  // If the caller did not provide criteria, the analysis scans the current
  // scope (or the whole graph) for built-in acquire/release APIs.
  ResourceFlowQueryResult result;
  PDGQueryOptions local_options = options;
  if (local_options.edge_preset == PDGEdgePreset::All)
    local_options.edge_preset = PDGEdgePreset::ValueFlow;

  NodeSet region;
  if (!criteria.empty()) {
    SliceQuery slice_query(pdg_);
    region = slice_query.forward(criteria, local_options, module).nodes;
  } else {
    region = scopeNodes(pdg_, local_options.scope);
  }
  if (region.empty())
    region = allGraphNodes(pdg_);

  SummaryQuery summary_query(pdg_);
  std::map<ResourceKind, std::vector<Node *>> acquires_by_kind;
  std::map<ResourceKind, std::vector<Node *>> releases_by_kind;

  for (NodeSet::const_iterator it = region.begin(); it != region.end(); ++it) {
    Node *node = *it;
    if (!isCallNode(node))
      continue;
    const ResourceKind acquire_kind = resourceKindForNode(node, false);
    const ResourceKind release_kind = resourceKindForNode(node, true);
    if (acquire_kind != ResourceKind::Unknown &&
        resourceKindMatches(acquire_kind, policy.resource_kind)) {
      acquires_by_kind[acquire_kind].push_back(node);
      result.acquire_sites.push_back(ResourceEvent{
          acquire_kind, ResourceEventKind::Acquire, node, calleeName(node)});
      result.resource_kind_counts[acquire_kind]++;
    }
    if (release_kind != ResourceKind::Unknown &&
        resourceKindMatches(release_kind, policy.resource_kind)) {
      releases_by_kind[release_kind].push_back(node);
      result.release_sites.push_back(ResourceEvent{
          release_kind, ResourceEventKind::Release, node, calleeName(node)});
    }
  }

  DependenceQuery dependence_query(pdg_);
  for (std::map<ResourceKind, std::vector<Node *>>::const_iterator acquire_it =
           acquires_by_kind.begin();
       acquire_it != acquires_by_kind.end(); ++acquire_it) {
    const ResourceKind kind = acquire_it->first;
    for (size_t i = 0; i < acquire_it->second.size(); ++i) {
      Node *acquire = acquire_it->second[i];
      std::vector<Node *> candidate_releases;

      if (releases_by_kind.count(kind) != 0) {
        for (size_t j = 0; j < releases_by_kind.find(kind)->second.size(); ++j) {
          Node *release = releases_by_kind.find(kind)->second[j];
          PDGCriteria source;
          source.nodes.insert(acquire);
          PDGCriteria target;
          target.nodes.insert(release);
          PDGQueryResult path =
              dependence_query.shortestPath(source, target, local_options, module);
          if (!path.witness_paths.empty())
            candidate_releases.push_back(release);
        }
      }

      if (candidate_releases.empty() && policy.include_interprocedural &&
          module != nullptr) {
        const Function *owner = functionForNode(acquire);
        if (owner != nullptr) {
          for (Module::const_iterator fit = module->begin(); fit != module->end();
               ++fit) {
            if (fit->isDeclaration())
              continue;
            SummaryQueryResult summary =
                summary_query.summarize(*fit, SummaryPolicy(), local_options, module);
            if (summary.summary.may_release_resource_kinds.count(kind) == 0)
              continue;
            for (size_t j = 0; j < summary.summary.reachable_calls.size(); ++j) {
              Node *callsite = summary.summary.reachable_calls[j].target;
              if (calleeName(callsite) == owner->getName().str() &&
                  releases_by_kind.count(kind) != 0) {
                candidate_releases.insert(candidate_releases.end(),
                    releases_by_kind.find(kind)->second.begin(),
                    releases_by_kind.find(kind)->second.end());
                break;
              }
            }
          }
        }
      }

      if (candidate_releases.empty()) {
        ResourcePath path;
        path.resource_kind = kind;
        path.acquire_site = acquire;
        path.orphaned = true;
        path.released = false;
        PDGWitnessPath witness;
        witness.kind = PDGWitnessPathKind::Slice;
        witness.nodes.push_back(acquire);
        path.witness_paths.push_back(witness);
        result.orphaned_resources.push_back(path);
        result.resource_paths.push_back(path);
        result.diagnostics.notes.push_back("Detected orphaned resource");
        continue;
      }

      if (candidate_releases.size() > 1)
        result.diagnostics.notes.push_back(
            "Multiple candidate releases observed for resource");

      for (size_t j = 0; j < candidate_releases.size(); ++j) {
        PDGCriteria source;
        source.nodes.insert(acquire);
        PDGCriteria target;
        target.nodes.insert(candidate_releases[j]);
        PDGQueryResult path_result =
            dependence_query.shortestPath(source, target, local_options, module);
        ResourcePath path;
        path.resource_kind = kind;
        path.acquire_site = acquire;
        path.release_site = candidate_releases[j];
        path.released = true;
        path.double_release_candidate = candidate_releases.size() > 1;
        path.witness_paths = path_result.witness_paths;
        result.resource_paths.push_back(path);
        if (path.double_release_candidate)
          result.double_release_candidates.push_back(path);
      }
    }
  }

  return result;
}

} // namespace pdg
