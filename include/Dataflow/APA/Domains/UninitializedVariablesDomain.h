#pragma once

#include "Dataflow/APA/Core/AbstractDomain.h"

#include <set>

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

struct UninitializedVariablesDomain : UnionDomain<std::set<llvm::Value *>> {};

using UninitVariablesFact = UninitializedVariablesDomain::value_type;

} // namespace elimination
