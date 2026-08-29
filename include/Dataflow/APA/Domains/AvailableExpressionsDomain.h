#pragma once

#include "Dataflow/APA/Core/AbstractDomain.h"
#include "Dataflow/APA/Domains/ExpressionKey.h"

#include <set>

namespace elimination {

struct AvailableExpressionsDomain
    : IntersectionDomain<std::set<ExpressionKey>> {
  using IntersectionDomain::IntersectionDomain;
};

using AvailableExpressionsFact = AvailableExpressionsDomain::value_type;

} // namespace elimination
