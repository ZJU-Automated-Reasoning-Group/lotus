/**
 * @file SummaryQuery.cpp
 * @brief Implementation of function-scoped PDG summaries.
 *
 * This file turns a function-local PDG slice into compact summary buckets such
 * as input-to-return, input-to-callsite, and resource-kind summaries. The
 * implementation stays intentionally lightweight and reuses the shared query
 * services from the core PDG query layer instead of introducing a separate
 * summary engine.
 */

#include "IR/PDG/Analysis/SummaryQuery.h"

#include "IR/PDG/Analysis/PDGQuery.h"

#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <sstream>

using namespace llvm;

namespace pdg {

namespace {

using NodeSet = PDGQueryResult::NodeSet;

/// True when @p node represents a global storage location tracked in the PDG.
static bool isGlobalNode(Node *node) {
  if (node == nullptr)
    return false;
  const GraphNodeType type = node->getNodeType();
  if (node->getValue() == nullptr) {
    return type == GraphNodeType::VAR_STATICALLOCGLOBALSCOPE ||
           type == GraphNodeType::VAR_STATICALLOCMODULESCOPE ||
           type == GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE;
  }
  return type == GraphNodeType::VAR_STATICALLOCGLOBALSCOPE ||
         type == GraphNodeType::VAR_STATICALLOCMODULESCOPE ||
         type == GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE ||
         isa<GlobalValue>(node->getValue());
}

/// Best-effort owner function for a PDG node.
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

static bool isInputNode(Node *node, const Function &function) {
  if (node == nullptr)
    return false;
  if (node->getNodeType() == GraphNodeType::PARAM_FORMALIN ||
      node->getNodeType() == GraphNodeType::PARAM_ACTUALIN)
    return true;
  const Argument *argument = dyn_cast_or_null<Argument>(node->getValue());
  return argument != nullptr && argument->getParent() == &function;
}

static bool isReturnNode(Node *node) {
  return node != nullptr && node->getNodeType() == GraphNodeType::INST_RET;
}

static bool isCallNode(Node *node) {
  return node != nullptr && node->getNodeType() == GraphNodeType::INST_FUNCALL;
}

static bool isControlPredicateNode(Node *node) {
  return node != nullptr &&
         (node->getNodeType() == GraphNodeType::INST_BR ||
          node->getNodeType() == GraphNodeType::FUNC_ENTRY);
}

/// Collect all nodes that belong to @p function plus optionally adjacent globals.
static NodeSet collectFunctionNodes(ProgramGraph &pdg, const Function &function,
                                    bool include_connected_globals) {
  NodeSet nodes;
  for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end(); ++it) {
    Node *node = *it;
    if (node != nullptr && functionForNode(node) == &function)
      nodes.insert(node);
  }
  if (pdg.hasNode(const_cast<Function &>(function)))
    nodes.insert(pdg.getNode(const_cast<Function &>(function)));

  if (!include_connected_globals)
    return nodes;

  for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end(); ++it) {
    Node *node = *it;
    if (!isGlobalNode(node))
      continue;
    bool connected = false;
    for (Node::EdgeSet::const_iterator edge_it = node->getOutEdgeSet().begin();
         edge_it != node->getOutEdgeSet().end() && !connected; ++edge_it) {
      Edge *edge = *edge_it;
      connected = edge != nullptr && functionForNode(edge->getDstNode()) == &function;
    }
    for (Node::EdgeSet::const_iterator edge_it = node->getInEdgeSet().begin();
         edge_it != node->getInEdgeSet().end() && !connected; ++edge_it) {
      Edge *edge = *edge_it;
      connected = edge != nullptr && functionForNode(edge->getSrcNode()) == &function;
    }
    if (connected)
      nodes.insert(node);
  }
  return nodes;
}

static std::string functionSummaryCacheKey(const Function &function,
                                           const SummaryPolicy &policy,
                                           const PDGQueryOptions &options) {
  std::ostringstream os;
  os << function.getName().str() << "|" << static_cast<int>(policy.kind) << "|"
     << policy.max_witnesses_per_bucket << "|"
     << static_cast<int>(options.edge_preset) << "|"
     << static_cast<int>(options.context_mode) << "|"
     << static_cast<int>(options.slice_flavor);
  return os.str();
}

/// Resolve summary criteria to exactly one function, emitting diagnostics on ambiguity.
static bool tryResolveSingleFunction(ProgramGraph &pdg,
                                     const PDGCriteria &criteria,
                                     const PDGQueryOptions &options,
                                     const Module *module,
                                     const Function *&function,
                                     PDGQueryDiagnostics &diagnostics) {
  function = nullptr;
  if (options.scope.kind == PDGQueryScope::Kind::Function &&
      options.scope.function != nullptr) {
    function = options.scope.function;
    return true;
  }
  if (criteria.function_names.size() == 1 && module != nullptr) {
    function = module->getFunction(criteria.function_names.front());
    if (function != nullptr)
      return true;
  }
  PDGCriteriaResolver resolver(pdg);
  PDGQueryResult resolved = resolver.resolve(criteria, options, module);
  std::set<const Function *> functions;
  for (NodeSet::const_iterator it = resolved.nodes.begin(); it != resolved.nodes.end();
       ++it) {
    const Function *candidate = functionForNode(*it);
    if (candidate != nullptr)
      functions.insert(candidate);
  }
  if (functions.size() == 1) {
    function = *functions.begin();
    return true;
  }
  if (functions.empty())
    diagnostics.unresolved_criteria.push_back(
        "summary query did not resolve to any function");
  else
    diagnostics.unresolved_criteria.push_back(
        "summary query resolved to multiple functions");
  return false;
}

static std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

static std::string calleeName(Node *node) {
  if (!isCallNode(node))
    return "";
  const CallBase *call = dyn_cast_or_null<CallBase>(node->getValue());
  if (call == nullptr || call->getCalledFunction() == nullptr)
    return "";
  return call->getCalledFunction()->getName().str();
}

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

static bool summaryKindEnabled(SummaryKind selected, SummaryKind candidate) {
  return selected == SummaryKind::All || selected == candidate;
}

static bool containsBucketEntry(const std::vector<SummaryBucketEntry> &entries,
                                Node *source, Node *target) {
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].source == source && entries[i].target == target)
      return true;
  }
  return false;
}

static void appendBucketEntry(std::vector<SummaryBucketEntry> &entries,
                              Node *source, Node *target,
                              const std::vector<PDGWitnessPath> &witnesses) {
  if (containsBucketEntry(entries, source, target))
    return;
  SummaryBucketEntry entry;
  entry.source = source;
  entry.target = target;
  entry.witness_paths = witnesses;
  entries.push_back(entry);
}

} // namespace

SummaryQueryResult SummaryQuery::summarize(const Function &function,
                                           const SummaryPolicy &policy,
                                           const PDGQueryOptions &options,
                                           const Module *module) const {
  // Function summaries are stable until the PDG graph epoch changes.
  if (cache_epoch_ != pdg_.getEpoch()) {
    cache_epoch_ = pdg_.getEpoch();
    summary_cache_.clear();
  }
  const std::string cache_key =
      functionSummaryCacheKey(function, policy, options);
  std::unordered_map<std::string, SummaryQueryResult>::const_iterator cached =
      summary_cache_.find(cache_key);
  if (options.cache_policy == PDGCachePolicy::Enabled &&
      cached != summary_cache_.end()) {
    SummaryQueryResult result = cached->second;
    result.diagnostics.summary_cache_hits++;
    return result;
  }

  SummaryQueryResult result;
  result.summary.function = &function;
  const NodeSet function_nodes = collectFunctionNodes(pdg_, function, true);

  PDGQueryOptions scoped_options = options;
  scoped_options.scope = PDGQueryScope::nodeSet(function_nodes);

  std::vector<Node *> input_nodes;
  std::vector<Node *> return_nodes;
  std::vector<Node *> call_nodes;
  std::vector<Node *> control_nodes;
  std::vector<Node *> global_nodes;

  for (NodeSet::const_iterator it = function_nodes.begin(); it != function_nodes.end();
       ++it) {
    Node *node = *it;
    if (isInputNode(node, function))
      input_nodes.push_back(node);
    if (isReturnNode(node))
      return_nodes.push_back(node);
    if (isCallNode(node))
      call_nodes.push_back(node);
    if (isControlPredicateNode(node))
      control_nodes.push_back(node);
    if (isGlobalNode(node))
      global_nodes.push_back(node);
  }

  SliceQuery slice_query(pdg_);
  DependenceQuery dependence_query(pdg_);
  for (size_t i = 0; i < input_nodes.size(); ++i) {
    PDGCriteria seed;
    seed.nodes.insert(input_nodes[i]);
    PDGQueryResult slice = slice_query.forward(seed, scoped_options, module);

    if (summaryKindEnabled(policy.kind, SummaryKind::InputToReturn)) {
      for (size_t j = 0; j < return_nodes.size(); ++j) {
        if (slice.nodes.count(return_nodes[j]) == 0)
          continue;
        PDGCriteria target_criteria;
        target_criteria.nodes.insert(return_nodes[j]);
        std::vector<PDGWitnessPath> witnesses =
            dependence_query.allShortestPaths(seed, target_criteria,
                                             scoped_options, module);
        if (policy.max_witnesses_per_bucket > 0 &&
            witnesses.size() > policy.max_witnesses_per_bucket)
          witnesses.resize(policy.max_witnesses_per_bucket);
        appendBucketEntry(result.summary.input_to_return, input_nodes[i],
                          return_nodes[j], witnesses);
      }
    }

    if (summaryKindEnabled(policy.kind, SummaryKind::InputToCallsite)) {
      for (size_t j = 0; j < call_nodes.size(); ++j) {
        if (slice.nodes.count(call_nodes[j]) == 0)
          continue;
        PDGCriteria target_criteria;
        target_criteria.nodes.insert(call_nodes[j]);
        std::vector<PDGWitnessPath> witnesses =
            dependence_query.allShortestPaths(seed, target_criteria,
                                             scoped_options, module);
        if (policy.max_witnesses_per_bucket > 0 &&
            witnesses.size() > policy.max_witnesses_per_bucket)
          witnesses.resize(policy.max_witnesses_per_bucket);
        appendBucketEntry(result.summary.input_to_callsite, input_nodes[i],
                          call_nodes[j], witnesses);
      }
    }

    if (summaryKindEnabled(policy.kind, SummaryKind::InputToGlobalWrite)) {
      for (size_t j = 0; j < global_nodes.size(); ++j) {
        if (slice.nodes.count(global_nodes[j]) == 0)
          continue;
        PDGCriteria target_criteria;
        target_criteria.nodes.insert(global_nodes[j]);
        std::vector<PDGWitnessPath> witnesses =
            dependence_query.allShortestPaths(seed, target_criteria,
                                             scoped_options, module);
        if (policy.max_witnesses_per_bucket > 0 &&
            witnesses.size() > policy.max_witnesses_per_bucket)
          witnesses.resize(policy.max_witnesses_per_bucket);
        appendBucketEntry(result.summary.input_to_global_write, input_nodes[i],
                          global_nodes[j], witnesses);
      }
    }
  }

  if (summaryKindEnabled(policy.kind, SummaryKind::GlobalReaders) ||
      summaryKindEnabled(policy.kind, SummaryKind::GlobalWriters)) {
    for (size_t i = 0; i < global_nodes.size(); ++i) {
      for (Node::EdgeSet::const_iterator edge_it =
               global_nodes[i]->getOutEdgeSet().begin();
           edge_it != global_nodes[i]->getOutEdgeSet().end(); ++edge_it) {
        Edge *edge = *edge_it;
        Node *reader = edge != nullptr ? edge->getDstNode() : nullptr;
        if (reader != nullptr && functionForNode(reader) == &function)
          appendBucketEntry(result.summary.global_readers, global_nodes[i], reader,
                            std::vector<PDGWitnessPath>());
      }
      for (Node::EdgeSet::const_iterator edge_it =
               global_nodes[i]->getInEdgeSet().begin();
           edge_it != global_nodes[i]->getInEdgeSet().end(); ++edge_it) {
        Edge *edge = *edge_it;
        Node *writer = edge != nullptr ? edge->getSrcNode() : nullptr;
        if (writer != nullptr && functionForNode(writer) == &function)
          appendBucketEntry(result.summary.global_writers, global_nodes[i], writer,
                            std::vector<PDGWitnessPath>());
      }
    }
  }

  if (summaryKindEnabled(policy.kind, SummaryKind::ControlPredicates)) {
    for (size_t i = 0; i < control_nodes.size(); ++i) {
      PDGCriteria seed;
      seed.nodes.insert(control_nodes[i]);
      PDGQueryOptions control_options = scoped_options;
      control_options.edge_preset = PDGEdgePreset::Control;
      PDGQueryResult region = slice_query.forward(seed, control_options, module);
      for (NodeSet::const_iterator it = region.nodes.begin(); it != region.nodes.end();
           ++it) {
        if (*it != control_nodes[i])
          appendBucketEntry(result.summary.control_predicates, control_nodes[i], *it,
                            std::vector<PDGWitnessPath>());
      }
    }
  }

  if (summaryKindEnabled(policy.kind, SummaryKind::ReachableCalls)) {
    for (size_t i = 0; i < call_nodes.size(); ++i)
      appendBucketEntry(result.summary.reachable_calls, nullptr, call_nodes[i],
                        std::vector<PDGWitnessPath>());
  }

  if (summaryKindEnabled(policy.kind, SummaryKind::ResourceKinds) ||
      policy.kind == SummaryKind::All) {
    for (size_t i = 0; i < call_nodes.size(); ++i) {
      const ResourceKind acquire_kind = resourceKindForNode(call_nodes[i], false);
      const ResourceKind release_kind = resourceKindForNode(call_nodes[i], true);
      if (acquire_kind != ResourceKind::Unknown)
        result.summary.may_allocate_resource_kinds.insert(acquire_kind);
      if (release_kind != ResourceKind::Unknown)
        result.summary.may_release_resource_kinds.insert(release_kind);
    }
  }

  if (options.cache_policy == PDGCachePolicy::Enabled)
    summary_cache_[cache_key] = result;
  return result;
}

SummaryQueryResult SummaryQuery::summarize(const PDGCriteria &criteria,
                                           const SummaryPolicy &policy,
                                           const PDGQueryOptions &options,
                                           const Module *module) const {
  SummaryQueryResult result;
  const Function *function = nullptr;
  if (!tryResolveSingleFunction(pdg_, criteria, options, module, function,
                                result.diagnostics) ||
      function == nullptr)
    return result;
  return summarize(*function, policy, options, module);
}

} // namespace pdg
