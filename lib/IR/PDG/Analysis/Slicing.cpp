/**
 * @file Slicing.cpp
 * @brief Implementation of slicing primitives for the Program Dependency Graph
 *
 * This file implements forward slicing, backward slicing, and program chopping
 * primitives over the PDG. These primitives enable various program analysis
 * tasks such as impact analysis, debugging, and program understanding.
 *
 * Features:
 * - Forward slicing: Find all nodes reachable from a given set of criteria
 * - Backward slicing: Find all nodes that can reach a given set of criteria
 * - Program chopping: Find all nodes on paths between source and sink nodes
 * - Configurable edge type filtering
 * - Depth-limited traversal
 * - Path enumeration for chopping
 */

#include "IR/PDG/Analysis/Slicing.h"

#include "IR/PDG/Support/PDGUtils.h"

#include <algorithm>
#include <queue>

using namespace llvm;

namespace pdg {

// ==================== Common Helper Functions ====================

namespace {
/**
 * @brief Common BFS traversal template for forward and backward slicing
 *
 * This template function provides a unified BFS traversal implementation that
 * can be used for both forward and backward slicing by parameterizing the edge
 * access and neighbor selection functions.
 *
 * @tparam GetEdgesFunc Function type that takes a Node* and returns edge
 * collection
 * @tparam GetNeighborFunc Function type that takes an Edge* and returns
 * neighbor Node*
 * @param start_nodes Set of starting nodes for the traversal
 * @param edge_types Set of allowed edge types (empty means all types allowed)
 * @param get_edges Function to get edges from a node (e.g., getOutEdgeSet or
 * getInEdgeSet)
 * @param get_neighbor Function to get neighbor from an edge (e.g., getDstNode
 * or getSrcNode)
 * @param max_depth Maximum traversal depth (SIZE_MAX means unlimited)
 * @return Set of nodes reachable from start_nodes following the specified
 * constraints
 */
template <typename GetEdgesFunc, typename GetNeighborFunc>
ForwardSlicing::NodeSet
traverseBFS(const ForwardSlicing::NodeSet &start_nodes,
            const std::set<EdgeType> &edge_types, GetEdgesFunc get_edges,
            GetNeighborFunc get_neighbor, size_t max_depth = SIZE_MAX) {
  ForwardSlicing::NodeSet slice;
  ForwardSlicing::VisitedSet visited;
  std::queue<std::pair<Node *, size_t>> worklist; // <node, current_depth>

  // Initialize worklist with starting nodes at depth 0
  for (auto *node : start_nodes)
    if (node != nullptr) {
      worklist.push({node, 0});
      slice.insert(node);
    }

  // BFS traversal; visited set prevents infinite loops on cyclic graphs.
  while (!worklist.empty()) {
    auto current_pair = worklist.front();
    Node *current = current_pair.first;
    size_t depth = current_pair.second;
    worklist.pop();

    // Skip if already visited or exceeded depth limit
    if (visited.find(current) != visited.end() || depth >= max_depth)
      continue;

    visited.insert(current);

    // Explore all edges from current node
    for (auto *edge : get_edges(current)) {
      // Skip null edges or edges not in allowed types
      if (edge == nullptr ||
          (!edge_types.empty() &&
           edge_types.find(edge->getEdgeType()) == edge_types.end()))
        continue;

      Node *neighbor = get_neighbor(edge);
      // Skip null neighbors or already visited neighbors
      if (neighbor == nullptr || visited.find(neighbor) != visited.end())
        continue;

      // Add neighbor to slice and queue for further exploration
      slice.insert(neighbor);
      worklist.push({neighbor, depth + 1});
    }
  }

  return slice;
}

/**
 * @brief Edge type filtering helper function
 *
 * Determines whether an edge type should be included in the traversal based on
 * the set of allowed edge types. If the allowed_types set is empty, all edge
 * types are considered allowed.
 *
 * @param edge_type The edge type to check
 * @param allowed_types Set of allowed edge types (empty means all types
 * allowed)
 * @return True if the edge type should be included in the traversal
 */
bool isEdgeTypeAllowed(EdgeType edge_type,
                       const std::set<EdgeType> &allowed_types) {
  return allowed_types.empty() ||
         allowed_types.find(edge_type) != allowed_types.end();
}
} // namespace

// ==================== ForwardSlicing Implementation ====================

ForwardSlicing::NodeSet
ForwardSlicing::computeSlice(Node &start_node,
                             const std::set<EdgeType> &edge_types) {
  return computeSlice({&start_node}, edge_types);
}

ForwardSlicing::NodeSet
ForwardSlicing::computeSlice(const NodeSet &start_nodes,
                             const std::set<EdgeType> &edge_types) {
  return traverseBFS(
      start_nodes, edge_types, [](Node *n) { return n->getOutEdgeSet(); },
      [](Edge *e) { return e->getDstNode(); });
}

ForwardSlicing::NodeSet
ForwardSlicing::computeSliceWithDepth(Node &start_node, size_t max_depth,
                                      const std::set<EdgeType> &edge_types) {
  if (max_depth == 0)
    max_depth = SIZE_MAX;
  return traverseBFS(
      {&start_node}, edge_types, [](Node *n) { return n->getOutEdgeSet(); },
      [](Edge *e) { return e->getDstNode(); }, max_depth);
}

bool ForwardSlicing::isEdgeTypeAllowed(
    EdgeType edge_type, const std::set<EdgeType> &allowed_types) const {
  return ::pdg::isEdgeTypeAllowed(edge_type, allowed_types);
}

// ==================== BackwardSlicing Implementation ====================

BackwardSlicing::NodeSet
BackwardSlicing::computeSlice(Node &end_node,
                              const std::set<EdgeType> &edge_types) {
  return computeSlice({&end_node}, edge_types);
}

BackwardSlicing::NodeSet
BackwardSlicing::computeSlice(const NodeSet &end_nodes,
                              const std::set<EdgeType> &edge_types) {
  return traverseBFS(
      end_nodes, edge_types, [](Node *n) { return n->getInEdgeSet(); },
      [](Edge *e) { return e->getSrcNode(); });
}

BackwardSlicing::NodeSet
BackwardSlicing::computeSliceWithDepth(Node &end_node, size_t max_depth,
                                       const std::set<EdgeType> &edge_types) {
  if (max_depth == 0)
    max_depth = SIZE_MAX;
  return traverseBFS(
      {&end_node}, edge_types, [](Node *n) { return n->getInEdgeSet(); },
      [](Edge *e) { return e->getSrcNode(); }, max_depth);
}

bool BackwardSlicing::isEdgeTypeAllowed(
    EdgeType edge_type, const std::set<EdgeType> &allowed_types) const {
  return ::pdg::isEdgeTypeAllowed(edge_type, allowed_types);
}

// ==================== ProgramChopping Implementation ====================

ProgramChopping::NodeSet ProgramChopping::computeChop(
    const NodeSet &source_nodes, const NodeSet &sink_nodes,
    const std::set<EdgeType> &edge_types, size_t max_paths,
    size_t max_path_length, ChopDiagnostics *diagnostics) {
  NodeSet chop;
  if (diagnostics != nullptr)
    *diagnostics = ChopDiagnostics{};
  auto mergeDiagnostics = [](ChopDiagnostics &dst, const ChopDiagnostics &src) {
    dst.depth_limit_hit |= src.depth_limit_hit;
    dst.path_limit_hit |= src.path_limit_hit;
    dst.path_length_limit_hit |= src.path_length_limit_hit;
    dst.max_depth_reached =
        std::max(dst.max_depth_reached, src.max_depth_reached);
    dst.paths_found += src.paths_found;
  };

  // Fast path: when no caps are requested, compute chop via reachability
  // intersection.
  if (max_paths == 0 && max_path_length == 0) {
    ForwardSlicing forward(_pdg);
    BackwardSlicing backward(_pdg);
    auto forward_slice = forward.computeSlice(source_nodes, edge_types);
    auto backward_slice = backward.computeSlice(sink_nodes, edge_types);
    for (auto *node : forward_slice) {
      if (backward_slice.count(node))
        chop.insert(node);
    }
    return chop;
  }

  // For each source-sink pair, find all nodes on paths between them
  for (auto *source : source_nodes)
    for (auto *sink : sink_nodes) {
      // Skip null nodes
      if (source == nullptr || sink == nullptr)
        continue;

      // Find all paths from source to sink; caps are optional guardrails.
      ChopDiagnostics local_diag;
      ChopDiagnostics *diag_ptr =
          diagnostics != nullptr ? &local_diag : nullptr;
      auto paths = findAllPaths(*source, *sink, max_paths, edge_types, 0,
                                max_path_length, diag_ptr);
      if (diagnostics != nullptr)
        mergeDiagnostics(*diagnostics, local_diag);

      // Add all nodes on these paths to the chop
      for (const auto &path : paths)
        for (auto *node : path)
          chop.insert(node);
    }

  return chop;
}

ProgramChopping::NodeSet ProgramChopping::computeChop(
    Node &source_node, Node &sink_node, const std::set<EdgeType> &edge_types,
    size_t max_paths, size_t max_path_length, ChopDiagnostics *diagnostics) {
  return computeChop({&source_node}, {&sink_node}, edge_types, max_paths,
                     max_path_length, diagnostics);
}

ProgramChopping::NodeSet ProgramChopping::computeChopWithDepth(
    const NodeSet &source_nodes, const NodeSet &sink_nodes, size_t max_depth,
    const std::set<EdgeType> &edge_types, size_t max_paths,
    size_t max_path_length, ChopDiagnostics *diagnostics) {
  NodeSet chop;
  if (diagnostics != nullptr)
    *diagnostics = ChopDiagnostics{};
  auto mergeDiagnostics = [](ChopDiagnostics &dst, const ChopDiagnostics &src) {
    dst.depth_limit_hit |= src.depth_limit_hit;
    dst.path_limit_hit |= src.path_limit_hit;
    dst.path_length_limit_hit |= src.path_length_limit_hit;
    dst.max_depth_reached =
        std::max(dst.max_depth_reached, src.max_depth_reached);
    dst.paths_found += src.paths_found;
  };

  // Fast path: when traversal is unlimited, avoid path enumeration.
  if (max_depth == 0 && max_paths == 0 && max_path_length == 0) {
    ForwardSlicing forward(_pdg);
    BackwardSlicing backward(_pdg);
    auto forward_slice = forward.computeSlice(source_nodes, edge_types);
    auto backward_slice = backward.computeSlice(sink_nodes, edge_types);
    for (auto *node : forward_slice) {
      if (backward_slice.count(node))
        chop.insert(node);
    }
    return chop;
  }

  // For each source-sink pair, find all nodes on paths between them
  for (auto *source : source_nodes)
    for (auto *sink : sink_nodes) {
      if (source == nullptr || sink == nullptr)
        continue;

      // Find all paths from source to sink with depth limit; caps are optional
      // guardrails.
      ChopDiagnostics local_diag;
      ChopDiagnostics *diag_ptr =
          diagnostics != nullptr ? &local_diag : nullptr;
      auto paths = findAllPaths(*source, *sink, max_paths, edge_types,
                                max_depth, max_path_length, diag_ptr);
      if (diagnostics != nullptr)
        mergeDiagnostics(*diagnostics, local_diag);

      // Add all nodes on these paths to the chop
      for (const auto &path : paths)
        for (auto *node : path)
          chop.insert(node);
    }

  return chop;
}

bool ProgramChopping::hasPath(Node &source_node, Node &sink_node,
                              const std::set<EdgeType> &edge_types) {
  // Convert edge_types to exclude set for canReach
  std::set<EdgeType> exclude_edges;
  if (!edge_types.empty()) {
    // If specific edge types are requested, exclude all others
    // This is a simplified approach - in practice, canReach might need
    // modification to support inclusion-based filtering rather than
    // exclusion-based
    for (int i = 0; i < static_cast<int>(EdgeType::TYPE_OTHEREDGE); ++i) {
      EdgeType et = static_cast<EdgeType>(i);
      if (edge_types.find(et) == edge_types.end())
        exclude_edges.insert(et);
    }
  }

  return _pdg.canReach(source_node, sink_node, exclude_edges);
}

std::vector<std::vector<Node *>> ProgramChopping::findAllPaths(
    Node &source_node, Node &sink_node, size_t max_paths,
    const std::set<EdgeType> &edge_types, size_t max_depth,
    size_t max_path_length, ChopDiagnostics *diagnostics) {
  std::vector<std::vector<Node *>> all_paths;
  std::vector<Node *> current_path;
  VisitedSet visited;
  findPathsDFS(source_node, sink_node, visited, current_path, all_paths, 0,
               max_paths, max_depth, max_path_length, edge_types, diagnostics);
  if (diagnostics != nullptr)
    diagnostics->paths_found = all_paths.size();
  return all_paths;
}

void ProgramChopping::findPathsDFS(Node &current, Node &sink,
                                   VisitedSet &visited,
                                   std::vector<Node *> &current_path,
                                   std::vector<std::vector<Node *>> &all_paths,
                                   size_t depth, size_t max_paths,
                                   size_t max_depth, size_t max_path_length,
                                   const std::set<EdgeType> &edge_types,
                                   ChopDiagnostics *diagnostics) {
  // Add current node to path and mark as visited to prevent cycles
  current_path.push_back(&current);
  visited.insert(&current);
  if (diagnostics != nullptr)
    diagnostics->max_depth_reached =
        std::max(diagnostics->max_depth_reached, depth);

  // Guardrails: stop if we hit configured caps to avoid unbounded path
  // explosion.
  if (max_path_length > 0 && current_path.size() > max_path_length) {
    if (diagnostics != nullptr)
      diagnostics->path_length_limit_hit = true;
    current_path.pop_back();
    visited.erase(&current);
    return;
  }
  if (max_paths > 0 && all_paths.size() >= max_paths) {
    if (diagnostics != nullptr)
      diagnostics->path_limit_hit = true;
    current_path.pop_back();
    visited.erase(&current);
    return;
  }

  // If we reached the sink, add this path to results
  if (&current == &sink) {
    all_paths.push_back(current_path);
  } else if (max_depth == 0 || depth < max_depth) {
    // Continue searching if we haven't exceeded the depth cap.
    for (auto *edge : current.getOutEdgeSet()) {
      if (edge == nullptr)
        continue;
      // Only follow edges of allowed types
      if (::pdg::isEdgeTypeAllowed(edge->getEdgeType(), edge_types)) {
        Node *neighbor = edge->getDstNode();
        // Skip null neighbors or already visited neighbors (cycle prevention)
        if (neighbor != nullptr && visited.find(neighbor) == visited.end()) {
          size_t next_depth = depth + 1;
          if (max_depth > 0 && next_depth > max_depth) {
            if (diagnostics != nullptr)
              diagnostics->depth_limit_hit = true;
            continue;
          }
          if (max_path_length > 0 &&
              current_path.size() + 1 > max_path_length) {
            if (diagnostics != nullptr)
              diagnostics->path_length_limit_hit = true;
            continue;
          }
          findPathsDFS(*neighbor, sink, visited, current_path, all_paths,
                       next_depth, max_paths, max_depth, max_path_length,
                       edge_types, diagnostics);
        }
      }
    }
  } else if (diagnostics != nullptr) {
    diagnostics->depth_limit_hit = true;
  }

  // Backtrack: remove current node from path and unmark as visited
  current_path.pop_back();
  visited.erase(&current);
}

bool ProgramChopping::isEdgeTypeAllowed(
    EdgeType edge_type, const std::set<EdgeType> &allowed_types) const {
  return ::pdg::isEdgeTypeAllowed(edge_type, allowed_types);
}

// ==================== SlicingUtils Implementation ====================

std::set<EdgeType> SlicingUtils::getDataDependencyEdges() {
  return {EdgeType::DATA_DEF_USE,  EdgeType::DATA_RAW,
          EdgeType::DATA_READ,     EdgeType::DATA_ALIAS,
          EdgeType::DATA_RET,      EdgeType::PARAMETER_IN,
          EdgeType::PARAMETER_OUT, EdgeType::PARAMETER_FIELD,
          EdgeType::VAL_DEP};
}

std::set<EdgeType> SlicingUtils::getControlDependencyEdges() {
  return {EdgeType::CONTROLDEP_CALLINV, EdgeType::CONTROLDEP_CALLRET,
          EdgeType::CONTROLDEP_ENTRY, EdgeType::CONTROLDEP_BR,
          EdgeType::CONTROLDEP_IND_BR};
}

std::set<EdgeType> SlicingUtils::getParameterDependencyEdges() {
  return {EdgeType::PARAMETER_IN, EdgeType::PARAMETER_OUT,
          EdgeType::PARAMETER_FIELD};
}

void SlicingUtils::printSlice(const NodeSet &slice,
                              const std::string &slice_name) {
  errs() << "=============== " << slice_name << " ===============\n";
  errs() << "Slice size: " << slice.size() << " nodes\n";

  for (auto *node : slice) {
    if (node == nullptr)
      continue;

    std::string str;
    raw_string_ostream OS(str);
    Value *val = node->getValue();

    if (val != nullptr) {
      if (Function *f = dyn_cast<Function>(val)) {
        OS << f->getName().str();
      } else if (Instruction *inst = dyn_cast<Instruction>(val)) {
        try {
          OS << *inst;
        } catch (...) {
          OS << "<invalid instruction>";
        }
      } else if (GlobalVariable *gv = dyn_cast<GlobalVariable>(val)) {
        OS << gv->getName().str();
      } else {
        try {
          OS << *val;
        } catch (...) {
          OS << "<invalid value>";
        }
      }
      errs() << "node: " << node << " - " << pdgutils::rtrim(OS.str()) << " - "
             << pdgutils::getNodeTypeStr(node->getNodeType()) << "\n";
    } else {
      errs() << "node: " << node << " - "
             << pdgutils::getNodeTypeStr(node->getNodeType()) << "\n";
    }
  }
  errs() << "==========================================\n";
}

std::unordered_map<std::string, size_t>
SlicingUtils::getSliceStatistics(const NodeSet &slice) {
  std::unordered_map<std::string, size_t> stats;
  std::unordered_map<GraphNodeType, size_t> node_type_counts;
  std::unordered_map<EdgeType, size_t> edge_type_counts;

  stats["total_nodes"] = slice.size();

  for (auto *node : slice) {
    if (node == nullptr)
      continue;

    node_type_counts[node->getNodeType()]++;

    for (auto *edge : node->getInEdgeSet())
      edge_type_counts[edge->getEdgeType()]++;
    for (auto *edge : node->getOutEdgeSet())
      edge_type_counts[edge->getEdgeType()]++;
  }

  for (const auto &pair : node_type_counts)
    stats["node_type_" + pdgutils::getNodeTypeStr(pair.first)] = pair.second;

  for (const auto &pair : edge_type_counts)
    stats["edge_type_" + pdgutils::getEdgeTypeStr(pair.first)] = pair.second;

  return stats;
}

} // namespace pdg
