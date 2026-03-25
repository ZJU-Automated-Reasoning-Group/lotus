/**
 * @file pdg-query.cpp
 * @brief Command-line tool for querying Program Dependence Graphs.
 */

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include "IR/PDG/Analysis/CypherQuery.h"
#include "IR/PDG/Analysis/PDGQuery.h"
#include "IR/PDG/Analysis/PropertyBasedSlicing.h"
#include "IR/PDG/Core/ControlDependencyGraph.h"
#include "IR/PDG/Core/DataDependencyGraph.h"
#include "IR/PDG/Core/ProgramDependencyGraph.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace llvm;
using namespace pdg;

namespace {

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::init("-"),
                                          cl::value_desc("filename"));

static cl::opt<std::string>
    QueryString("query", "q", cl::desc("Execute a single Cypher query"),
                cl::value_desc("cypher_query"));

static cl::opt<std::string>
    QueryFile("query-file", "f", cl::desc("Execute Cypher queries from file"),
              cl::value_desc("filename"));

static cl::opt<bool> Interactive("interactive", "i",
                                 cl::desc("Run in interactive mode"));

static cl::opt<bool> Verbose("verbose", "v", cl::desc("Enable verbose output"));

static cl::opt<bool> Explain("explain", "e",
                             cl::desc("Show query execution plan"));

static cl::opt<bool>
    BuildPDG("build-pdg",
             cl::desc("Build full PDG (adds data/control/param edges)"),
             cl::init(true));

static cl::opt<int>
    ResultLimit("limit",
                cl::desc("Maximum number of results to return (default: 100)"),
                cl::init(100));

static cl::opt<int> UnboundedMaxHops(
    "max-unbounded-hops",
    cl::desc("Default cap for unbounded traversals (e.g. *..), default: 5"),
    cl::init(5));

static cl::opt<std::string>
    Format("format", cl::desc("Output format: text, json, dot"),
           cl::init("text"));

static cl::list<std::string> QueryParams(
    "param",
    cl::desc("Query parameter key=value (repeatable); referenced as $key"),
    cl::ZeroOrMore, cl::value_desc("key=value"));

static cl::opt<std::string> PropertyFile(
    "property-file",
    cl::desc("Resolve criteria from a Symbiotic-style .prp file"),
    cl::value_desc("filename"), cl::init(""));

static cl::opt<std::string>
    SliceDirection("direction",
                   cl::desc("Property slice direction: backward|forward"),
                   cl::init("backward"));

static cl::opt<bool> DumpSlice("dump-slice",
                               cl::desc("Dump selected property slice nodes"),
                               cl::init(false));

static cl::opt<std::string>
    AnalysisName("analysis",
                 cl::desc("Run PDG analysis: slice-forward, slice-backward, "
                          "chop, shortest-path, reaching-defs, live, dead, "
                          "control-region, controllers, diff, summary, "
                          "impact, resource-flow"),
                 cl::init(""));

static cl::opt<std::string>
    CriteriaQuery("criteria-query",
                  cl::desc("Cypher query selecting analysis criteria"),
                  cl::init(""));

static cl::opt<std::string>
    TargetQuery("target-query",
                cl::desc("Cypher query selecting analysis targets"),
                cl::init(""));

static cl::opt<std::string>
    BaselineQuery("baseline-query",
                  cl::desc("Cypher query selecting baseline criteria for "
                           "changed-only impact"),
                  cl::init(""));

static cl::opt<std::string>
    ScopeFunction("scope-function",
                  cl::desc("Restrict analysis scope to one LLVM function"),
                  cl::init(""));

static cl::opt<std::string>
    ScopeQuery("scope-query",
               cl::desc("Cypher query selecting analysis scope"),
               cl::init(""));

static cl::opt<std::string>
    EdgePreset("edge-preset",
               cl::desc("Edge preset: all, data, control, parameter, "
                        "interprocedural, value-flow, transform-legality"),
               cl::init("all"));

static cl::opt<bool>
    ContextSensitive("context-sensitive",
                     cl::desc("Use call/return matching during traversal"),
                     cl::init(false));

static cl::opt<bool> Thin("thin", cl::desc("Use thin slicing semantics"),
                          cl::init(false));

static cl::opt<std::string>
    SummaryKindFlag("summary-kind",
                    cl::desc("Summary bucket: all, input-to-return, "
                             "input-to-global-write, input-to-callsite, "
                             "global-readers, global-writers, "
                             "control-predicates, reachable-calls, "
                             "resource-kinds"),
                    cl::init("all"));

static cl::opt<std::string>
    ResourceKindFlag("resource-kind",
                     cl::desc("Resource family: all, heap, file, fd, dir"),
                     cl::init("all"));

static cl::opt<bool> ShowVersion("show-version",
                                 cl::desc("Show version information"));

static std::string describeEdge(CypherQueryExecutor &executor, Edge *edge) {
  if (!edge)
    return "<null>";
  std::string value;
  value += executor.getEdgePropertyString(edge, "label");
  return value;
}

static void printVersion() {
  outs() << "PDG Query Tool v2.0\n";
  outs() << "Part of the Lotus Program Analysis Framework\n";
}

static void printPDGInfo(ProgramGraph &pdg) {
  outs() << "PDG Information:\n";
  outs() << "  Total nodes: " << pdg.numNode() << "\n";
  outs() << "  Total edges: " << pdg.numEdge() << "\n";
  outs() << "  Functions: " << pdg.getFuncWrapperMap().size() << "\n";
}

static std::string jsonEscape(const std::string &input) {
  std::string output;
  output.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    switch (input[i]) {
    case '\\':
      output += "\\\\";
      break;
    case '"':
      output += "\\\"";
      break;
    case '\n':
      output += "\\n";
      break;
    default:
      output += input[i];
      break;
    }
  }
  return output;
}

static PDGEdgePreset parseEdgePreset() {
  const std::string preset = StringRef(EdgePreset).lower();
  if (preset == "data")
    return PDGEdgePreset::Data;
  if (preset == "control")
    return PDGEdgePreset::Control;
  if (preset == "parameter")
    return PDGEdgePreset::Parameter;
  if (preset == "interprocedural")
    return PDGEdgePreset::Interprocedural;
  if (preset == "value-flow")
    return PDGEdgePreset::ValueFlow;
  if (preset == "transform-legality")
    return PDGEdgePreset::TransformLegality;
  return PDGEdgePreset::All;
}

static SummaryKind parseSummaryKind() {
  const std::string kind = StringRef(SummaryKindFlag).lower();
  if (kind == "input-to-return")
    return SummaryKind::InputToReturn;
  if (kind == "input-to-global-write")
    return SummaryKind::InputToGlobalWrite;
  if (kind == "input-to-callsite")
    return SummaryKind::InputToCallsite;
  if (kind == "global-readers")
    return SummaryKind::GlobalReaders;
  if (kind == "global-writers")
    return SummaryKind::GlobalWriters;
  if (kind == "control-predicates")
    return SummaryKind::ControlPredicates;
  if (kind == "reachable-calls")
    return SummaryKind::ReachableCalls;
  if (kind == "resource-kinds")
    return SummaryKind::ResourceKinds;
  return SummaryKind::All;
}

static ResourceKind parseResourceKind() {
  const std::string kind = StringRef(ResourceKindFlag).lower();
  if (kind == "heap")
    return ResourceKind::Heap;
  if (kind == "file")
    return ResourceKind::File;
  if (kind == "fd")
    return ResourceKind::FileDescriptor;
  if (kind == "dir")
    return ResourceKind::Directory;
  return ResourceKind::Unknown;
}

static bool executeQuery(CypherQueryExecutor &executor,
                         const std::string &query_string) {
  CypherParser parser;
  CypherQueryParameters params;
  for (const auto &kv : QueryParams) {
    size_t eq = kv.find('=');
    if (eq == std::string::npos || eq == 0) {
      errs() << "Invalid --param (expected key=value): " << kv << "\n";
      return false;
    }
    params[kv.substr(0, eq)] = kv.substr(eq + 1);
  }

  std::unique_ptr<CypherQuery> query =
      params.empty() ? parser.parse(query_string)
                     : parser.parse(query_string, params);
  if (!query) {
    errs() << "Parse error: " << parser.getLastError().message << "\n";
    return false;
  }

  if (Explain) {
    outs() << "Plan: " << query->getPatterns().size() << " patterns, "
           << query->getReturnItems().size() << " returns";
    if (query->hasWhere())
      outs() << ", WHERE";
    if (query->hasLimit())
      outs() << ", LIMIT " << query->getLimit();
    outs() << "\n";
  }

  if (!query->hasLimit() && ResultLimit > 0)
    const_cast<CypherQuery *>(query.get())->setLimit(ResultLimit);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  if (!result) {
    errs() << "Error: " << executor.getLastError() << "\n";
    return false;
  }

  outs() << "Result: " << result->toString() << "\n";
  if (result->getType() == CypherResult::ResultType::NODES) {
    size_t limit = ResultLimit > 0
                       ? std::min(result->getNodes().size(),
                                  static_cast<size_t>(ResultLimit))
                       : result->getNodes().size();
    for (size_t i = 0; i < limit; ++i)
      outs() << "  " << describeNode(result->getNodes()[i]) << "\n";
  } else if (result->getType() == CypherResult::ResultType::RELATIONSHIPS) {
    size_t limit = ResultLimit > 0
                       ? std::min(result->getRelationships().size(),
                                  static_cast<size_t>(ResultLimit))
                       : result->getRelationships().size();
    for (size_t i = 0; i < limit; ++i)
      outs() << "  "
             << describeEdge(executor, result->getRelationships()[i]) << "\n";
  }

  return true;
}

static void runInteractiveMode(CypherQueryExecutor &executor) {
  outs() << "PDG Query (type 'help' or 'quit')\n> ";

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      outs() << "> ";
      continue;
    }
    if (line == "quit" || line == "exit")
      break;
    if (line == "info")
      printPDGInfo(executor.getPDG());
    else if (line == "help")
      outs() << "Commands: help, quit, info\n";
    else
      executeQuery(executor, line);
    outs() << "> ";
  }
}

static void runBatchMode(CypherQueryExecutor &executor,
                         const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    errs() << "Error: Could not open file " << filename << "\n";
    return;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    executeQuery(executor, line);
  }
}

static bool selectNodesWithCypher(CypherQueryExecutor &executor,
                                  const std::string &query_string,
                                  std::set<Node *> &nodes) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query = parser.parse(query_string);
  if (!query) {
    errs() << "Parse error: " << parser.getLastError().message << "\n";
    return false;
  }
  std::unique_ptr<CypherResult> result = executor.execute(*query);
  if (!result) {
    errs() << "Query error: " << executor.getLastError() << "\n";
    return false;
  }
  nodes.insert(result->getNodes().begin(), result->getNodes().end());
  return true;
}

static PDGQueryOptions buildAnalysisOptions(ProgramGraph &pdg, const Module &module,
                                            CypherQueryExecutor &executor) {
  PDGQueryOptions options;
  options.edge_preset = parseEdgePreset();
  options.context_mode = ContextSensitive ? PDGContextMode::ContextSensitive
                                          : PDGContextMode::ContextInsensitive;
  options.slice_flavor = Thin ? SliceFlavor::Thin : SliceFlavor::Full;
  options.explain = true;
  if (!ScopeFunction.empty()) {
    if (Function *function = module.getFunction(ScopeFunction))
      options.scope = PDGQueryScope::functionScope(*function);
  } else if (!ScopeQuery.empty()) {
    std::set<Node *> nodes;
    if (selectNodesWithCypher(executor, ScopeQuery, nodes))
      options.scope = PDGQueryScope::nodeSet(nodes);
  } else {
    options.scope = PDGQueryScope::wholeGraph();
  }
  (void)pdg;
  return options;
}

static void printResultText(const PDGQueryResult &result) {
  outs() << "criteria nodes: " << result.criteria_nodes.size() << "\n";
  outs() << "result nodes: " << result.nodes.size() << "\n";
  outs() << "result edges: " << result.edges.size() << "\n";
  if (!result.diagnostics.unresolved_criteria.empty()) {
    outs() << "unresolved criteria:\n";
    for (size_t i = 0; i < result.diagnostics.unresolved_criteria.size(); ++i)
      outs() << "  - " << result.diagnostics.unresolved_criteria[i] << "\n";
  }
  if (!result.witness_paths.empty()) {
    outs() << "witness paths:\n";
    for (size_t i = 0; i < result.witness_paths.size(); ++i) {
      outs() << "  path " << i << ":";
      for (size_t j = 0; j < result.witness_paths[i].nodes.size(); ++j)
        outs() << " " << stableNodeKey(result.witness_paths[i].nodes[j]);
      outs() << "\n";
    }
  }
  if (DumpSlice) {
    for (PDGQueryResult::NodeSet::const_iterator it = result.nodes.begin();
         it != result.nodes.end(); ++it)
      outs() << "  " << describeNode(*it) << "\n";
  }
}

static void printResultJson(const PDGQueryResult &result) {
  outs() << "{";
  outs() << "\"criteria_nodes\":" << result.criteria_nodes.size() << ",";
  outs() << "\"nodes\":[";
  bool first = true;
  for (PDGQueryResult::NodeSet::const_iterator it = result.nodes.begin();
       it != result.nodes.end(); ++it) {
    if (!first)
      outs() << ",";
    first = false;
    outs() << "\"" << jsonEscape(stableNodeKey(*it)) << "\"";
  }
  outs() << "],";
  outs() << "\"witness_paths\":[";
  for (size_t i = 0; i < result.witness_paths.size(); ++i) {
    if (i != 0)
      outs() << ",";
    outs() << "[";
    for (size_t j = 0; j < result.witness_paths[i].nodes.size(); ++j) {
      if (j != 0)
        outs() << ",";
      outs() << "\"" << jsonEscape(stableNodeKey(result.witness_paths[i].nodes[j]))
             << "\"";
    }
    outs() << "]";
  }
  outs() << "]";
  outs() << "}\n";
}

static void printResultDot(const PDGQueryResult &result) {
  outs() << "digraph PDGQuery {\n";
  for (PDGQueryResult::NodeSet::const_iterator it = result.nodes.begin();
       it != result.nodes.end(); ++it) {
    outs() << "  \"" << stableNodeKey(*it) << "\";\n";
  }
  for (PDGQueryResult::EdgeSet::const_iterator it = result.edges.begin();
       it != result.edges.end(); ++it) {
    Edge *edge = *it;
    outs() << "  \"" << stableNodeKey(edge->getSrcNode()) << "\" -> \""
           << stableNodeKey(edge->getDstNode()) << "\""
           << " [label=\"" << pdgutils::getEdgeTypeStr(edge->getEdgeType())
           << "\"];\n";
  }
  outs() << "}\n";
}

static void printDiff(const DiffQueryResult &result) {
  if (Format == "json") {
    outs() << "{"
           << "\"added_nodes\":";
    size_t added = 0;
    size_t removed = 0;
    for (size_t i = 0; i < result.node_diffs.size(); ++i) {
      added += result.node_diffs[i].kind == DiffKind::Added;
      removed += result.node_diffs[i].kind == DiffKind::Removed;
    }
    outs() << added << ",\"removed_nodes\":" << removed << "}\n";
    return;
  }

  outs() << "node diffs: " << result.node_diffs.size() << "\n";
  outs() << "edge diffs: " << result.edge_diffs.size() << "\n";
  if (!result.impact_summary.functions.empty()) {
    outs() << "functions:\n";
    for (std::unordered_map<std::string, size_t>::const_iterator it =
             result.impact_summary.functions.begin();
         it != result.impact_summary.functions.end(); ++it)
      outs() << "  " << it->first << ": " << it->second << "\n";
  }
}

static void printSummaryText(const SummaryQueryResult &result) {
  outs() << "function: "
         << (result.summary.function ? result.summary.function->getName() : "<none>")
         << "\n";
  outs() << "input_to_return: " << result.summary.input_to_return.size() << "\n";
  outs() << "input_to_global_write: "
         << result.summary.input_to_global_write.size() << "\n";
  outs() << "input_to_callsite: " << result.summary.input_to_callsite.size()
         << "\n";
  outs() << "global_readers: " << result.summary.global_readers.size() << "\n";
  outs() << "global_writers: " << result.summary.global_writers.size() << "\n";
  outs() << "control_predicates: " << result.summary.control_predicates.size()
         << "\n";
  outs() << "reachable_calls: " << result.summary.reachable_calls.size() << "\n";
  if (!result.summary.may_allocate_resource_kinds.empty()) {
    outs() << "may_allocate_resource_kinds:";
    for (std::set<ResourceKind>::const_iterator it =
             result.summary.may_allocate_resource_kinds.begin();
         it != result.summary.may_allocate_resource_kinds.end(); ++it)
      outs() << " " << resourceKindName(*it);
    outs() << "\n";
  }
  if (!result.summary.may_release_resource_kinds.empty()) {
    outs() << "may_release_resource_kinds:";
    for (std::set<ResourceKind>::const_iterator it =
             result.summary.may_release_resource_kinds.begin();
         it != result.summary.may_release_resource_kinds.end(); ++it)
      outs() << " " << resourceKindName(*it);
    outs() << "\n";
  }
}

static void printSummaryJson(const SummaryQueryResult &result) {
  outs() << "{";
  outs() << "\"function\":\""
         << jsonEscape(result.summary.function
                            ? result.summary.function->getName().str()
                            : "")
         << "\",";
  outs() << "\"input_to_return\":" << result.summary.input_to_return.size() << ",";
  outs() << "\"input_to_global_write\":"
         << result.summary.input_to_global_write.size() << ",";
  outs() << "\"input_to_callsite\":" << result.summary.input_to_callsite.size()
         << ",";
  outs() << "\"global_readers\":" << result.summary.global_readers.size() << ",";
  outs() << "\"global_writers\":" << result.summary.global_writers.size() << ",";
  outs() << "\"control_predicates\":"
         << result.summary.control_predicates.size() << ",";
  outs() << "\"reachable_calls\":" << result.summary.reachable_calls.size() << ",";
  outs() << "\"may_allocate_resource_kinds\":[";
  bool first = true;
  for (std::set<ResourceKind>::const_iterator it =
           result.summary.may_allocate_resource_kinds.begin();
       it != result.summary.may_allocate_resource_kinds.end(); ++it) {
    if (!first)
      outs() << ",";
    first = false;
    outs() << "\"" << jsonEscape(resourceKindName(*it)) << "\"";
  }
  outs() << "],\"may_release_resource_kinds\":[";
  first = true;
  for (std::set<ResourceKind>::const_iterator it =
           result.summary.may_release_resource_kinds.begin();
       it != result.summary.may_release_resource_kinds.end(); ++it) {
    if (!first)
      outs() << ",";
    first = false;
    outs() << "\"" << jsonEscape(resourceKindName(*it)) << "\"";
  }
  outs() << "]}\n";
}

static void printImpactText(const ImpactQueryResult &result) {
  outs() << "directly_impacted_nodes: "
         << result.directly_impacted_nodes.nodes.size() << "\n";
  outs() << "transitively_impacted_nodes: "
         << result.transitively_impacted_nodes.nodes.size() << "\n";
  outs() << "impacted_functions:";
  for (std::set<std::string>::const_iterator it = result.impacted_functions.begin();
       it != result.impacted_functions.end(); ++it)
    outs() << " " << *it;
  outs() << "\n";
  outs() << "impacted_source_locations: "
         << result.impacted_source_locations.size() << "\n";
  outs() << "boundary_crossings:";
  for (std::unordered_map<std::string, size_t>::const_iterator it =
           result.boundary_crossings.begin();
       it != result.boundary_crossings.end(); ++it)
    outs() << " " << it->first << "=" << it->second;
  outs() << "\n";
  outs() << "ranked_impacts:\n";
  for (size_t i = 0; i < result.ranked_impacts.size(); ++i) {
    outs() << "  " << result.ranked_impacts[i].stable_key
           << " dist=" << result.ranked_impacts[i].shortest_distance
           << " crossings=" << result.ranked_impacts[i].interprocedural_crossings
           << " paths=" << result.ranked_impacts[i].path_count << "\n";
  }
}

static void printImpactJson(const ImpactQueryResult &result) {
  outs() << "{";
  outs() << "\"directly_impacted_nodes\":"
         << result.directly_impacted_nodes.nodes.size() << ",";
  outs() << "\"transitively_impacted_nodes\":"
         << result.transitively_impacted_nodes.nodes.size() << ",";
  outs() << "\"impacted_functions\":[";
  bool first = true;
  for (std::set<std::string>::const_iterator it = result.impacted_functions.begin();
       it != result.impacted_functions.end(); ++it) {
    if (!first)
      outs() << ",";
    first = false;
    outs() << "\"" << jsonEscape(*it) << "\"";
  }
  outs() << "],\"boundary_crossings\":{";
  first = true;
  for (std::unordered_map<std::string, size_t>::const_iterator it =
           result.boundary_crossings.begin();
       it != result.boundary_crossings.end(); ++it) {
    if (!first)
      outs() << ",";
    first = false;
    outs() << "\"" << jsonEscape(it->first) << "\":" << it->second;
  }
  outs() << "},\"ranked_impacts\":[";
  for (size_t i = 0; i < result.ranked_impacts.size(); ++i) {
    if (i != 0)
      outs() << ",";
    outs() << "{\"node\":\""
           << jsonEscape(result.ranked_impacts[i].stable_key) << "\","
           << "\"distance\":" << result.ranked_impacts[i].shortest_distance
           << ",\"crossings\":"
           << result.ranked_impacts[i].interprocedural_crossings
           << ",\"path_count\":" << result.ranked_impacts[i].path_count << "}";
  }
  outs() << "]}\n";
}

static void printResourceFlowText(const ResourceFlowQueryResult &result) {
  outs() << "acquire_sites: " << result.acquire_sites.size() << "\n";
  outs() << "release_sites: " << result.release_sites.size() << "\n";
  outs() << "orphaned_resources: " << result.orphaned_resources.size() << "\n";
  outs() << "double_release_candidates: "
         << result.double_release_candidates.size() << "\n";
  outs() << "resource_kind_counts:";
  for (std::map<ResourceKind, size_t>::const_iterator it =
           result.resource_kind_counts.begin();
       it != result.resource_kind_counts.end(); ++it)
    outs() << " " << resourceKindName(it->first) << "=" << it->second;
  outs() << "\n";
}

static void printResourceFlowJson(const ResourceFlowQueryResult &result) {
  outs() << "{";
  outs() << "\"acquire_sites\":" << result.acquire_sites.size() << ",";
  outs() << "\"release_sites\":" << result.release_sites.size() << ",";
  outs() << "\"orphaned_resources\":" << result.orphaned_resources.size() << ",";
  outs() << "\"double_release_candidates\":"
         << result.double_release_candidates.size() << ",";
  outs() << "\"resource_kind_counts\":{";
  bool first = true;
  for (std::map<ResourceKind, size_t>::const_iterator it =
           result.resource_kind_counts.begin();
       it != result.resource_kind_counts.end(); ++it) {
    if (!first)
      outs() << ",";
    first = false;
    outs() << "\"" << jsonEscape(resourceKindName(it->first)) << "\":"
           << it->second;
  }
  outs() << "}}\n";
}

static bool executeAnalysis(ProgramGraph &pdg, const Module &module,
                            CypherQueryExecutor &executor) {
  PDGQueryOptions options = buildAnalysisOptions(pdg, module, executor);
  PDGCriteria criteria;
  PDGCriteria targets;
  PDGCriteria baseline;

  if (!CriteriaQuery.empty())
    criteria.cypher_selections.push_back(
        CypherSelection{CriteriaQuery.getValue(), ""});
  if (!TargetQuery.empty())
    targets.cypher_selections.push_back(
        CypherSelection{TargetQuery.getValue(), ""});
  if (!BaselineQuery.empty())
    baseline.cypher_selections.push_back(
        CypherSelection{BaselineQuery.getValue(), ""});

  if (!PropertyFile.empty()) {
    PropertySpec spec;
    std::string error;
    if (!PropertySpec::parseFromFile(PropertyFile, spec, error)) {
      errs() << "error: " << error << "\n";
      return false;
    }
    criteria.property_specs.push_back(spec);
    if (AnalysisName.empty()) {
      if (SliceDirection == "forward")
        const_cast<cl::opt<std::string> &>(AnalysisName).setValue("slice-forward");
      else
        const_cast<cl::opt<std::string> &>(AnalysisName).setValue("slice-backward");
    }
  }

  if (AnalysisName.empty()) {
    errs() << "No mode specified. Use -q, -i, -f, or --analysis\n";
    return false;
  }

  if (criteria.empty() &&
      (AnalysisName != "live" && AnalysisName != "dead" &&
       AnalysisName != "resource-flow" &&
       !(AnalysisName == "summary" && options.scope.kind == PDGQueryScope::Kind::Function))) {
    errs() << "Analysis requires criteria. Use --criteria-query or --property-file\n";
    return false;
  }

  SliceQuery slice_query(pdg);
  DependenceQuery dependence_query(pdg);
  DataFlowQuery dataflow_query(pdg);
  DiffQuery diff_query(pdg);
  SummaryQuery summary_query(pdg);
  ImpactQuery impact_query(pdg);
  ResourceFlowQuery resource_query(pdg);

  if (AnalysisName == "slice-forward") {
    PDGQueryResult result = slice_query.forward(criteria, options, &module);
    if (Format == "json")
      printResultJson(result);
    else if (Format == "dot")
      printResultDot(result);
    else
      printResultText(result);
    return true;
  }

  if (AnalysisName == "slice-backward") {
    PDGQueryResult result = slice_query.backward(criteria, options, &module);
    if (Format == "json")
      printResultJson(result);
    else if (Format == "dot")
      printResultDot(result);
    else
      printResultText(result);
    return true;
  }

  if (AnalysisName == "chop") {
    if (targets.empty()) {
      errs() << "chop requires --target-query\n";
      return false;
    }
    PDGQueryResult result = slice_query.chop(criteria, targets, options, &module);
    if (Format == "json")
      printResultJson(result);
    else if (Format == "dot")
      printResultDot(result);
    else
      printResultText(result);
    return true;
  }

  if (AnalysisName == "shortest-path") {
    if (targets.empty()) {
      errs() << "shortest-path requires --target-query\n";
      return false;
    }
    PDGQueryResult result =
        dependence_query.shortestPath(criteria, targets, options, &module);
    if (Format == "json")
      printResultJson(result);
    else if (Format == "dot")
      printResultDot(result);
    else
      printResultText(result);
    return true;
  }

  if (AnalysisName == "reaching-defs") {
    PDGQueryResult result =
        dataflow_query.reachingDefinitions(criteria, options, &module);
    if (Format == "json")
      printResultJson(result);
    else if (Format == "dot")
      printResultDot(result);
    else
      printResultText(result);
    return true;
  }

  if (AnalysisName == "control-region") {
    PDGQueryResult result =
        dataflow_query.controlRegion(criteria, options, &module);
    if (Format == "json")
      printResultJson(result);
    else if (Format == "dot")
      printResultDot(result);
    else
      printResultText(result);
    return true;
  }

  if (AnalysisName == "controllers") {
    PDGQueryResult result =
        dataflow_query.allControllers(criteria, options, &module);
    if (Format == "json")
      printResultJson(result);
    else if (Format == "dot")
      printResultDot(result);
    else
      printResultText(result);
    return true;
  }

  if (AnalysisName == "live") {
    PDGQueryResult result = dataflow_query.liveNodes(options);
    if (Format == "json")
      printResultJson(result);
    else if (Format == "dot")
      printResultDot(result);
    else
      printResultText(result);
    return true;
  }

  if (AnalysisName == "dead") {
    PDGQueryResult result = dataflow_query.deadNodes(options);
    if (Format == "json")
      printResultJson(result);
    else if (Format == "dot")
      printResultDot(result);
    else
      printResultText(result);
    return true;
  }

  if (AnalysisName == "diff") {
    if (targets.empty()) {
      errs() << "diff requires --target-query\n";
      return false;
    }
    PDGQueryResult before = slice_query.forward(criteria, options, &module);
    PDGQueryResult after = slice_query.forward(targets, options, &module);
    DiffQueryResult result = diff_query.diff(before, after, options);
    printDiff(result);
    return true;
  }

  if (AnalysisName == "summary") {
    SummaryPolicy policy;
    policy.kind = parseSummaryKind();
    SummaryQueryResult result =
        summary_query.summarize(criteria, policy, options, &module);
    if (Format == "json")
      printSummaryJson(result);
    else
      printSummaryText(result);
    return true;
  }

  if (AnalysisName == "impact") {
    ImpactPolicy policy;
    policy.changed_only = !baseline.empty();
    ImpactQueryResult result =
        baseline.empty() ? impact_query.analyze(criteria, policy, options, &module)
                         : impact_query.analyzeAgainstBaseline(
                               criteria, baseline, policy, options, &module);
    if (Format == "json")
      printImpactJson(result);
    else
      printImpactText(result);
    return true;
  }

  if (AnalysisName == "resource-flow") {
    ResourcePolicy policy;
    policy.resource_kind = parseResourceKind();
    ResourceFlowQueryResult result =
        resource_query.analyze(criteria, policy, options, &module);
    if (Format == "json")
      printResourceFlowJson(result);
    else
      printResourceFlowText(result);
    return true;
  }

  errs() << "Unsupported analysis: " << AnalysisName << "\n";
  return false;
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "PDG Query Tool\n");

  if (ShowVersion) {
    printVersion();
    return 0;
  }

  LLVMContext context;
  SMDiagnostic error;
  std::unique_ptr<Module> module = parseIRFile(InputFilename, error, context);
  if (!module) {
    error.print(argv[0], errs());
    return 1;
  }

  ProgramGraph &pdg = ProgramGraph::getInstance();
  if (BuildPDG) {
    auto &registry = *PassRegistry::getPassRegistry();
    initializeCore(registry);
    initializeAnalysis(registry);
    initializeTransformUtils(registry);

    legacy::PassManager pm;
    pm.add(new DataDependencyGraph());
    pm.add(new ControlDependencyGraph());
    pm.add(new ProgramDependencyGraph());
    pm.run(*module);
  } else {
    pdg.reset();
    pdg.build(*module);
    pdg.bindDITypeToNodes(*module);
  }

  if (Verbose)
    printPDGInfo(pdg);

  CypherQueryExecutor executor(pdg);
  executor.setUnboundedMaxHops(UnboundedMaxHops);

  if (!AnalysisName.empty() || !PropertyFile.empty())
    return executeAnalysis(pdg, *module, executor) ? 0 : 1;

  if (Interactive) {
    runInteractiveMode(executor);
    return 0;
  }
  if (!QueryString.empty())
    return executeQuery(executor, QueryString) ? 0 : 1;
  if (!QueryFile.empty()) {
    runBatchMode(executor, QueryFile);
    return 0;
  }

  errs() << "No mode specified. Use -q, -i, -f, or --analysis\n";
  return 1;
}
