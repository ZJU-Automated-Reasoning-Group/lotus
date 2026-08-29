#pragma once

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

namespace llvm {
class Value;
} // namespace llvm

namespace mono {

struct UninitializedVariablesDomain
    : LLVMMonoAnalysisTypes<SetContainer<llvm::Value *>> {};

} // namespace mono
