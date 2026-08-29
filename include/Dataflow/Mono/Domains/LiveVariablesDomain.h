#pragma once

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

namespace llvm {
class Value;
} // namespace llvm

namespace mono {

struct LiveVariablesDomain : UnionDomain<SetContainer<llvm::Value *>> {};

using LiveVariablesAnalysisTypes =
    LLVMMonoAnalysisTypes<LiveVariablesDomain::value_type, LiveVariablesDomain>;

} // namespace mono
