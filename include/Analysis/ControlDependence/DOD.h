//===- DOD.h - Decisive-order dependence ----------------------*- C++ -*-===//

#pragma once

#include "Analysis/ControlDependence/ControlDependenceGraph.h"

#include <functional>

namespace lotus::cd::detail {

DependenceResult computeDOD(Graph &graph);
DependenceResult computeDODRanganath(Graph &graph);
DependenceResult computeDODNTSCD(Graph &graph);

/// Execute baseline DOD preprocessing through projection construction and
/// range-boundary discovery, stopping before endpoint-pair traversal.
size_t preprocessBaselineDOD(Graph &graph);

/// Run the baseline DOD construction and stream exact triples without storing
/// them. This preserves the baseline algorithm while making output-sensitive
/// experiments comparable with compact biclique enumeration.
void forEachBaselineDODPair(
    Graph &graph,
    const std::function<void(GraphNode *decision, GraphNode *first,
                             GraphNode *second)> &callback);

} // namespace lotus::cd::detail
