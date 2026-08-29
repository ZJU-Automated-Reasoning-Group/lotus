#pragma once

#include "Dataflow/Mono/Domains/FullConstantPropagationDomain.h"

#include <unordered_map>

namespace llvm {
class Function;
class Instruction;
} // namespace llvm

namespace mono {

std::unordered_map<llvm::Instruction *, FullConstantPropagationState>
runIntraMonoFullConstantPropagation(llvm::Function *F);

} // namespace mono
