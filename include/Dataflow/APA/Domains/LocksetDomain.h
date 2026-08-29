#pragma once

#include "Dataflow/APA/Core/AbstractDomain.h"

#include <set>

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

struct LocksetDomain : UnionDomain<std::set<const llvm::Value *>> {};

using LocksetFact = LocksetDomain::value_type;

} // namespace elimination
