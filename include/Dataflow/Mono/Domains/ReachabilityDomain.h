#pragma once

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

namespace llvm {
class Value;
} // namespace llvm

namespace mono {

struct ReachabilityDomain
    : LLVMMonoAnalysisTypes<SetContainer<llvm::Value *>> {};

} // namespace mono
