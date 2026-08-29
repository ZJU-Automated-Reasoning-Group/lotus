#pragma once

#include "Dataflow/APA/Core/AbstractDomain.h"

namespace elimination {

struct ReachabilityDomain {
  using value_type = bool;

  value_type bottom() const { return false; }
  value_type join(value_type Lhs, value_type Rhs) const { return Lhs || Rhs; }
  bool equal(value_type Lhs, value_type Rhs) const { return Lhs == Rhs; }
};

using ReachableFact = ReachabilityDomain::value_type;

} // namespace elimination
