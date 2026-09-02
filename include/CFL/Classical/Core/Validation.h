#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace lotus::cfl::classical {

GrammarParseOptions inferGrammarAttributes(const LabeledGraph &graph);

/// Parse an unsigned 32-bit attribute. Non-decimal text returns nullopt;
/// decimal overflow is rejected rather than truncated.
std::optional<std::uint32_t> parseAttributeValue(std::string_view value);

std::vector<GrammarIssue> validateGraph(const LabeledGraph &graph,
                                        const Grammar &grammar,
                                        bool terminals_only = true);

} // namespace lotus::cfl::classical
