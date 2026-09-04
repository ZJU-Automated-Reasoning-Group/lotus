#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"

namespace lotus::cfl::classical::engines {

/// The exact production tables embedded by POCR's two analysis clients.
enum class PocrClientGrammar {
  StandardAlias,
  RewrittenAlias,
  StandardValueFlow,
  RewrittenValueFlow,
};

/// Build a POCR client grammar and instantiate `_i` from graph labels.
Grammar buildPocrClientGrammar(PocrClientGrammar grammar,
                               const LabeledGraph &graph);

} // namespace lotus::cfl::classical::engines
