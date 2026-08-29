#pragma once

namespace elimination {

struct ReachabilityDomain {
  using value_type = bool;

  static value_type meet(value_type Lhs, value_type Rhs) { return Lhs || Rhs; }
  static bool equal(value_type Lhs, value_type Rhs) { return Lhs == Rhs; }
  static value_type meetIdentity() { return false; }
};

using ReachableFact = ReachabilityDomain::value_type;

} // namespace elimination
