#pragma once

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

namespace llvm {
class Value;
} // namespace llvm

namespace mono {

struct UninitializedVariablesDomain : UnionDomain<SetContainer<llvm::Value *>> {
};

using UninitializedVariablesAnalysisTypes =
    LLVMMonoAnalysisTypes<UninitializedVariablesDomain::value_type,
                          UninitializedVariablesDomain>;

} // namespace mono
