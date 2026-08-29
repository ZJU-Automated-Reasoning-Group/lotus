#pragma once

#include "Dataflow/Mono/Domains/ConstantPropagationDomain.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"
#include "Dataflow/Mono/Support/MonoDebug.h"

#include <unordered_map>

namespace llvm {
class Function;
class Instruction;
} // namespace llvm

namespace mono {

using ConstantPropagationSolver = IntraMonoSolver<ConstantPropagationAnalysisTypes>;

std::unordered_map<llvm::Instruction *, ConstantPropagationMap>
runIntraMonoConstantPropagation(llvm::Function *F,
                                const DebugConfig &DebugCfg = DebugConfig{});

} // namespace mono
