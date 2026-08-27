//===- CompactControlDependence.h - Compact strong CD ----------*- C++ -*-===//
//
// Implements all-target inevitability, multiway NTSCD, canonical DOD
// bicliques, and incidence-based NTSCD-DOD closure.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SparseBitVector.h"

#include "Analysis/ControlDependence/ControlDependenceGraph.h"

#include <functional>
#include <map>
#include <vector>

namespace lotus::cd::detail {

class Inevitability {
public:
  explicit Inevitability(size_t nodeCount = 0) : m_rows(nodeCount) {}

  bool contains(const GraphNode *source, const GraphNode *target) const;
  const llvm::SparseBitVector<> &row(const GraphNode *source) const;
  size_t size() const { return m_rows.size(); }

private:
  friend Inevitability computeInevitability(Graph &graph);
  std::vector<llvm::SparseBitVector<>> m_rows;
};

struct DODBiclique {
  GraphNode *decision{nullptr};
  llvm::SparseBitVector<> left;
  llvm::SparseBitVector<> right;
  std::vector<GraphNode *> cycle;
  llvm::SparseBitVector<> branch1Entries;
  llvm::SparseBitVector<> branch2Entries;

  bool contains(const GraphNode *first, const GraphNode *second) const;
  size_t pairCount() const;
};

using DODBicliqueMap = std::map<GraphNode *, DODBiclique>;

Inevitability computeInevitability(Graph &graph);

DependenceResult computeCompactNTSCD(Graph &graph,
                                     const Inevitability &inevitability);

DODBicliqueMap computeCompactDOD(Graph &graph,
                                 const Inevitability &inevitability);

/// Ablation variant that propagates complete first-hit sets over the SCC
/// condensation instead of capped cardinalities.
DODBicliqueMap computeCompactDODExactSets(Graph &graph,
                                          const Inevitability &inevitability);

/// Materialize dg-compatible binary dependence edges without enumerating the
/// Cartesian product of each biclique.
DependenceResult
materializeCompactDODDependencies(Graph &graph,
                                  const DODBicliqueMap &bicliques);

/// Enumerate exact DOD triples in output-sensitive time.
void forEachDODPair(
    const Graph &graph, const DODBicliqueMap &bicliques,
    const std::function<void(GraphNode *decision, GraphNode *first,
                             GraphNode *second)> &callback);

NodeSet computeCompactDependencyClosure(Graph &graph, const NodeSet &seed,
                                        const DependenceResult &ntscd,
                                        const DODBicliqueMap &bicliques);

/// Ablation variant that expands and indexes every DOD pair before closure.
NodeSet computeEagerPairDependencyClosure(Graph &graph, const NodeSet &seed,
                                          const DependenceResult &ntscd,
                                          const DODBicliqueMap &bicliques);

} // namespace lotus::cd::detail
