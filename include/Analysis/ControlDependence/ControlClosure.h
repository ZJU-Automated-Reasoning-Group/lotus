//===- ControlClosure.h - Strong control closure --------------*- C++ -*-===//

#pragma once

#include "Analysis/ControlDependence/ControlDependenceGraph.h"

namespace lotus::cd::detail {

NodeSet computeStrongControlClosure(Graph &graph, const NodeSet &nodes);

} // namespace lotus::cd::detail
