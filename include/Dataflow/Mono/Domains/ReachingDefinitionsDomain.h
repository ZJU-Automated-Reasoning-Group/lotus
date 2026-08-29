#pragma once

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

namespace llvm {
class Instruction;
} // namespace llvm

namespace mono {

struct ReachingDefinitionsDomain
    : UnionDomain<SetContainer<llvm::Instruction *>> {};

using ReachingDefinitionsAnalysisTypes =
    LLVMMonoAnalysisTypes<ReachingDefinitionsDomain::value_type,
                          ReachingDefinitionsDomain>;

} // namespace mono
