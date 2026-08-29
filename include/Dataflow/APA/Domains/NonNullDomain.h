#pragma once

#include "Dataflow/APA/Core/AbstractDomain.h"

#include <set>

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

struct NonNullDomain : IntersectionDomain<std::set<const llvm::Value *>> {
  using IntersectionDomain::IntersectionDomain;
};

using NonNullFact = NonNullDomain::value_type;

} // namespace elimination
