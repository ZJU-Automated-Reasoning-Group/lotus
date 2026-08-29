#pragma once

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

namespace llvm {
class Value;
} // namespace llvm

namespace mono {

struct TaintDomain : UnionDomain<SetContainer<llvm::Value *>> {};

using TaintAnalysisTypes =
    LLVMMonoAnalysisTypes<TaintDomain::value_type, TaintDomain>;

} // namespace mono
