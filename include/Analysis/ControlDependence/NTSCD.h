//===- NTSCD.h - Non-termination-sensitive control dependence -*- C++ -*-===//

#pragma once

#include "Analysis/ControlDependence/ControlDependenceGraph.h"

namespace lotus::cd::detail {

DependenceResult computeNTSCD(Graph &graph);
DependenceResult computeNTSCD2(Graph &graph);
DependenceResult computeNTSCDRanganath(Graph &graph, bool fixed);

} // namespace lotus::cd::detail
