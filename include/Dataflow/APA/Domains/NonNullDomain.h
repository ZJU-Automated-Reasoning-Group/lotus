#pragma once

#include <algorithm>
#include <iterator>
#include <set>

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

struct NonNullDomain {
  using value_type = std::set<const llvm::Value *>;

  static value_type meet(const value_type &Lhs, const value_type &Rhs) {
    value_type Out;
    std::set_intersection(Lhs.begin(), Lhs.end(), Rhs.begin(), Rhs.end(),
                          std::inserter(Out, Out.begin()));
    return Out;
  }

  static bool equal(const value_type &Lhs, const value_type &Rhs) {
    return Lhs == Rhs;
  }
};

using NonNullFact = NonNullDomain::value_type;

} // namespace elimination
