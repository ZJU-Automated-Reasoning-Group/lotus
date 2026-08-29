#pragma once

#include "Dataflow/APA/Core/AbstractDomain.h"

#include <set>

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

struct LiveVariablesDomain : UnionDomain<std::set<const llvm::Value *>> {};

using LiveVariablesFact = LiveVariablesDomain::value_type;

} // namespace elimination
