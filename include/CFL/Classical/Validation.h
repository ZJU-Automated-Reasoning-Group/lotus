#pragma once

#include "CFL/Classical/Grammar.h"
#include "CFL/Classical/Graph.h"

#include <vector>

namespace lotus::cfl::classical {

GrammarParseOptions inferGrammarAttributes(const LabeledGraph &graph);

std::vector<GrammarIssue> validateGraph(const LabeledGraph &graph,
                                        const Grammar &grammar,
                                        bool terminals_only = true);

} // namespace lotus::cfl::classical
