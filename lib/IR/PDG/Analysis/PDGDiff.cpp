/**
 * @file PDGDiff.cpp
 * @brief Implementation of structural differencing for PDG sub-graphs.
 *
 * References:
 * - Horwitz, "Identifying the Semantic and Textual Differences Between Two
 *   Versions of a Program", PLDI 1990.
 * - Jackson & Ladd, "Semantic Diff: A Tool for Summarizing the Effects of
 *   Modifications", ICSM 1994.
 */

#include "IR/PDG/Analysis/PDGDiff.h"

#include "IR/PDG/Support/PDGUtils.h"

#include <algorithm>
#include <chrono>

using namespace llvm;

namespace pdg {

// ============================================================================
// PDGDiffResult helpers
// ============================================================================

static size_t countNodeDiffs(const std::vector<NodeDiffEntry> &diffs,
                             DiffKind kind) {
  return std::count_if(
      diffs.begin(), diffs.end(),
      [kind](const NodeDiffEntry &e) { return e.kind == kind; });
}

static size_t countEdgeDiffs(const std::vector<EdgeDiffEntry> &diffs,
                             DiffKind kind) {
  return std::count_if(
      diffs.begin(), diffs.end(),
      [kind](const EdgeDiffEntry &e) { return e.kind == kind; });
}

size_t PDGDiffResult::numAddedNodes() const {
  return countNodeDiffs(node_diffs, DiffKind::ADDED);
}
size_t PDGDiffResult::numRemovedNodes() const {
  return countNodeDiffs(node_diffs, DiffKind::REMOVED);
}
size_t PDGDiffResult::numPreservedNodes() const {
  return countNodeDiffs(node_diffs, DiffKind::PRESERVED);
}
size_t PDGDiffResult::numAddedEdges() const {
  return countEdgeDiffs(edge_diffs, DiffKind::ADDED);
}
size_t PDGDiffResult::numRemovedEdges() const {
  return countEdgeDiffs(edge_diffs, DiffKind::REMOVED);
}
size_t PDGDiffResult::numPreservedEdges() const {
  return countEdgeDiffs(edge_diffs, DiffKind::PRESERVED);
}
bool PDGDiffResult::isIdentical() const {
  return numAddedNodes() == 0 && numRemovedNodes() == 0 &&
         numAddedEdges() == 0 && numRemovedEdges() == 0;
}

// ============================================================================
// PDGDiff implementation
// ============================================================================

namespace {

/// Hash for (Node*, Node*, EdgeType) edge identity.
struct EdgeIdentity {
  Node *src;
  Node *dst;
  EdgeType type;

  bool operator==(const EdgeIdentity &o) const {
    return src == o.src && dst == o.dst && type == o.type;
  }
};

struct EdgeIdentityHash {
  size_t operator()(const EdgeIdentity &e) const {
    size_t h = std::hash<Node *>{}(e.src);
    h ^= std::hash<Node *>{}(e.dst) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(static_cast<int>(e.type)) + 0x9e3779b9 + (h << 6) +
         (h >> 2);
    return h;
  }
};

} // namespace

std::set<Edge *>
PDGDiff::collectInducedEdges(const NodeSet &nodes,
                             const std::set<EdgeType> &edge_types) {
  std::set<Edge *> edges;
  for (Node *n : nodes) {
    if (n == nullptr)
      continue;
    for (Edge *e : n->getOutEdgeSet()) {
      if (e == nullptr)
        continue;
      // Both endpoints must be in the node set.
      if (nodes.count(e->getDstNode()) == 0)
        continue;
      if (!edge_types.empty() && edge_types.count(e->getEdgeType()) == 0)
        continue;
      edges.insert(e);
    }
  }
  return edges;
}

PDGDiffResult PDGDiff::diff(const NodeSet &old_nodes, const NodeSet &new_nodes,
                            PDGDiffDiagnostics *diagnostics) {
  return diff(old_nodes, new_nodes, {}, diagnostics);
}

PDGDiffResult PDGDiff::diff(const NodeSet &old_nodes, const NodeSet &new_nodes,
                            const std::set<EdgeType> &edge_types,
                            PDGDiffDiagnostics *diagnostics) {
  auto t0 = std::chrono::steady_clock::now();

  PDGDiffResult result;

  // --- Node diff ---
  // Determine which matcher to use.
  bool use_pointer_eq = !_matcher;

  std::unordered_map<Node *, Node *> old_to_new;

  if (use_pointer_eq) {
    // Fast path: pointer equality.
    for (Node *n : old_nodes) {
      if (new_nodes.count(n))
        result.node_diffs.push_back({n, DiffKind::PRESERVED});
      else
        result.node_diffs.push_back({n, DiffKind::REMOVED});
    }
    for (Node *n : new_nodes) {
      if (old_nodes.count(n) == 0)
        result.node_diffs.push_back({n, DiffKind::ADDED});
    }
  } else {
    // Custom matcher: build a consistent old->new node matching and reuse it
    // for both node and edge diff.
    std::unordered_set<Node *> matched_new;
    for (Node *o : old_nodes) {
      bool found = false;
      for (Node *n : new_nodes) {
        if (matched_new.count(n))
          continue;
        if (_matcher(o, n)) {
          result.node_diffs.push_back({o, DiffKind::PRESERVED});
          old_to_new[o] = n;
          matched_new.insert(n);
          found = true;
          break;
        }
      }
      if (!found)
        result.node_diffs.push_back({o, DiffKind::REMOVED});
    }
    for (Node *n : new_nodes) {
      if (matched_new.count(n) == 0)
        result.node_diffs.push_back({n, DiffKind::ADDED});
    }
  }

  // --- Edge diff ---
  auto old_edges = collectInducedEdges(old_nodes, edge_types);
  auto new_edges = collectInducedEdges(new_nodes, edge_types);

  if (use_pointer_eq) {
    // Compare edges by (src,dst,type) identity, preserving multiplicity.
    std::unordered_map<EdgeIdentity, size_t, EdgeIdentityHash> old_counts;
    std::unordered_map<EdgeIdentity, size_t, EdgeIdentityHash> new_counts;
    for (Edge *e : old_edges)
      old_counts[{e->getSrcNode(), e->getDstNode(), e->getEdgeType()}]++;
    for (Edge *e : new_edges)
      new_counts[{e->getSrcNode(), e->getDstNode(), e->getEdgeType()}]++;

    for (Edge *e : old_edges) {
      EdgeIdentity eid{e->getSrcNode(), e->getDstNode(), e->getEdgeType()};
      auto it = new_counts.find(eid);
      if (it != new_counts.end() && it->second > 0) {
        result.edge_diffs.push_back({e, DiffKind::PRESERVED});
        it->second--;
      } else {
        result.edge_diffs.push_back({e, DiffKind::REMOVED});
      }
    }
    for (Edge *e : new_edges) {
      EdgeIdentity eid{e->getSrcNode(), e->getDstNode(), e->getEdgeType()};
      auto it = old_counts.find(eid);
      if (it != old_counts.end() && it->second > 0) {
        it->second--;
        continue;
      }
      {
        result.edge_diffs.push_back({e, DiffKind::ADDED});
      }
    }
  } else {
    // With custom matcher, treat an old edge as PRESERVED if its endpoints are
    // matched and there exists a corresponding new edge with the same type.
    std::set<Edge *> matched_new_edges;
    for (Edge *oe : old_edges) {
      auto src_it = old_to_new.find(oe->getSrcNode());
      auto dst_it = old_to_new.find(oe->getDstNode());
      if (src_it == old_to_new.end() || dst_it == old_to_new.end()) {
        result.edge_diffs.push_back({oe, DiffKind::REMOVED});
        continue;
      }
      // Find matching new edge.
      bool found = false;
      for (Edge *ne : new_edges) {
        if (matched_new_edges.count(ne))
          continue;
        if (ne->getSrcNode() == src_it->second &&
            ne->getDstNode() == dst_it->second &&
            ne->getEdgeType() == oe->getEdgeType()) {
          result.edge_diffs.push_back({oe, DiffKind::PRESERVED});
          matched_new_edges.insert(ne);
          found = true;
          break;
        }
      }
      if (!found)
        result.edge_diffs.push_back({oe, DiffKind::REMOVED});
    }
    for (Edge *ne : new_edges) {
      if (matched_new_edges.count(ne) == 0)
        result.edge_diffs.push_back({ne, DiffKind::ADDED});
    }
  }

  if (diagnostics) {
    auto t1 = std::chrono::steady_clock::now();
    diagnostics->old_nodes = old_nodes.size();
    diagnostics->new_nodes = new_nodes.size();
    diagnostics->old_edges = old_edges.size();
    diagnostics->new_edges = new_edges.size();
    diagnostics->diff_time_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
  }

  return result;
}

// ============================================================================
// Matchers
// ============================================================================

bool PDGDiff::instructionStringMatcher(Node *a, Node *b) {
  if (a == nullptr || b == nullptr)
    return false;
  if (a->getNodeType() != b->getNodeType())
    return false;

  Value *va = a->getValue();
  Value *vb = b->getValue();

  if (va == nullptr && vb == nullptr)
    return true; // both are entry/annotation nodes of same type
  if (va == nullptr || vb == nullptr)
    return false;

  std::string sa, sb;
  raw_string_ostream osa(sa), osb(sb);
  try {
    osa << *va;
    osb << *vb;
  } catch (...) {
    return false;
  }
  return osa.str() == osb.str();
}

// ============================================================================
// Printing and statistics
// ============================================================================

void PDGDiff::printDiffSummary(const PDGDiffResult &result,
                               const std::string &label) {
  errs() << "=============== " << label << " ===============\n";
  errs() << "Nodes:  +" << result.numAddedNodes() << "  -"
         << result.numRemovedNodes() << "  =" << result.numPreservedNodes()
         << "\n";
  errs() << "Edges:  +" << result.numAddedEdges() << "  -"
         << result.numRemovedEdges() << "  =" << result.numPreservedEdges()
         << "\n";
  errs() << "Identical: " << (result.isIdentical() ? "yes" : "no") << "\n";

  // Print changed nodes.
  for (const auto &nd : result.node_diffs) {
    if (nd.kind == DiffKind::PRESERVED)
      continue;
    const char *prefix = (nd.kind == DiffKind::ADDED) ? "[+] " : "[-] ";
    std::string str;
    raw_string_ostream OS(str);
    Value *val = nd.node ? nd.node->getValue() : nullptr;
    if (val) {
      try {
        OS << *val;
      } catch (...) {
        OS << "<invalid>";
      }
    } else {
      OS << "<no-value>";
    }
    errs() << prefix << pdgutils::rtrim(OS.str()) << "\n";
  }

  errs() << "==========================================\n";
}

std::unordered_map<std::string, size_t>
PDGDiff::getDiffStatistics(const PDGDiffResult &result) {
  std::unordered_map<std::string, size_t> stats;
  stats["added_nodes"] = result.numAddedNodes();
  stats["removed_nodes"] = result.numRemovedNodes();
  stats["preserved_nodes"] = result.numPreservedNodes();
  stats["added_edges"] = result.numAddedEdges();
  stats["removed_edges"] = result.numRemovedEdges();
  stats["preserved_edges"] = result.numPreservedEdges();
  stats["total_node_diffs"] = result.node_diffs.size();
  stats["total_edge_diffs"] = result.edge_diffs.size();
  stats["is_identical"] = result.isIdentical() ? 1 : 0;
  return stats;
}

} // namespace pdg
