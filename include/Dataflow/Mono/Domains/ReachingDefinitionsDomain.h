#pragma once

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

namespace llvm {
class Instruction;
} // namespace llvm

namespace mono {

struct ReachingDefinitionsDomain
    : LLVMMonoAnalysisTypes<SetContainer<llvm::Instruction *>> {};

} // namespace mono
