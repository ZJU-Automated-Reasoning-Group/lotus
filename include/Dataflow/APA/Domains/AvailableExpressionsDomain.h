#pragma once

#include "Dataflow/APA/Domains/ExpressionKey.h"

#include <algorithm>
#include <iterator>
#include <set>

namespace elimination {

struct AvailableExpressionsDomain {
  using value_type = std::set<ExpressionKey>;

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

using AvailableExpressionsFact = AvailableExpressionsDomain::value_type;

} // namespace elimination
