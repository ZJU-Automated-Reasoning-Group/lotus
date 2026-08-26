//===- SCD.h - Standard control dependence --------------------*- C++ -*-===//

#pragma once

#include "llvm/ADT/DenseMap.h"

#include "Analysis/ControlDependence/ControlDependenceGraph.h"

namespace llvm {
class BasicBlock;
class Function;
} // namespace llvm

namespace lotus::cd::detail {

DependenceResult computeSCD(
    llvm::Function &function,
    const llvm::DenseMap<const llvm::BasicBlock *, GraphNode *> &blockToNode);

} // namespace lotus::cd::detail
