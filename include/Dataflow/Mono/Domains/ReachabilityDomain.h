#pragma once

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

namespace llvm {
class Value;
} // namespace llvm

namespace mono {

struct ReachabilityDomain : UnionDomain<SetContainer<llvm::Value *>> {};

using ReachabilityAnalysisTypes =
    LLVMMonoAnalysisTypes<ReachabilityDomain::value_type, ReachabilityDomain>;

} // namespace mono
