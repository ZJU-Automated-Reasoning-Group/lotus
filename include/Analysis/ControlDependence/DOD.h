//===- DOD.h - Decisive-order dependence ----------------------*- C++ -*-===//

#pragma once

#include "Analysis/ControlDependence/ControlDependenceGraph.h"

namespace lotus::cd::detail {

DependenceResult computeDOD(Graph &graph);
DependenceResult computeDODRanganath(Graph &graph);
DependenceResult computeDODNTSCD(Graph &graph);

} // namespace lotus::cd::detail
