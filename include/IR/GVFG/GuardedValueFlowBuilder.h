#pragma once

// Compatibility shim for older includes. The structural builder pass is
// declared in GuardedValueFlowGraph.h.
#include "IR/GVFG/GuardedValueFlowGraph.h"

namespace lotus {
namespace gvfg {

using GuardedValueFlowBuilderPass = GuardedValueFlowGraphBuilderPass;

} // namespace gvfg
} // namespace lotus
