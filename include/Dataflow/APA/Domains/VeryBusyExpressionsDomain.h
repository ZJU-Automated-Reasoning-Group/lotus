#pragma once

#include "Dataflow/APA/Core/AbstractDomain.h"
#include "Dataflow/APA/Domains/ExpressionKey.h"

#include <set>

namespace elimination {

struct VeryBusyExpressionsDomain : IntersectionDomain<std::set<ExpressionKey>> {
  using IntersectionDomain::IntersectionDomain;
};

using VeryBusyExpressionsFact = VeryBusyExpressionsDomain::value_type;

} // namespace elimination
