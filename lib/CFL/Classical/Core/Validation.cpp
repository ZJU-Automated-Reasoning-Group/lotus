#include "CFL/Classical/Core/Validation.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace lotus::cfl::classical {

std::optional<std::uint32_t> parseAttributeValue(std::string_view value) {
  if (value.empty() ||
      !std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isdigit(character) != 0;
      })) {
    return std::nullopt;
  }
  std::uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error == std::errc::result_out_of_range ||
      parsed > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("Attribute exceeds uint32_t range: " +
                                std::string(value));
  }
  if (error != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(parsed);
}

GrammarParseOptions inferGrammarAttributes(const LabeledGraph &graph) {
  GrammarParseOptions options;
  for (const auto &[label, _] : graph.symbolPairs()) {
    const auto separator = label.find_last_of('_');
    if (separator == std::string::npos || separator + 1 == label.size()) {
      continue;
    }
    const auto attribute =
        parseAttributeValue(std::string_view(label).substr(separator + 1));
    if (!attribute) {
      continue;
    }
    options.symbol_attributes[label.substr(0, separator)].push_back(*attribute);
  }

  for (auto &[_, domain] : options.symbol_attributes) {
    std::sort(domain.begin(), domain.end());
    domain.erase(std::unique(domain.begin(), domain.end()), domain.end());
  }
  return options;
}

std::vector<GrammarIssue> validateGraph(const LabeledGraph &graph,
                                        const Grammar &grammar,
                                        bool terminals_only) {
  std::vector<GrammarIssue> issues = grammar.validate();
  std::unordered_set<std::string> reported;
  for (const auto &[label, pairs] : graph.symbolPairs()) {
    (void)pairs;
    if (!grammar.hasSymbol(label) && reported.insert(label).second) {
      issues.push_back({GrammarIssueSeverity::Error,
                        "Graph label is absent from grammar: " + label});
    } else if (terminals_only && grammar.isNonterminal(label) &&
               reported.insert(label).second) {
      issues.push_back({GrammarIssueSeverity::Warning,
                        "Input graph contains nonterminal edge: " + label});
    }
  }
  return issues;
}

} // namespace lotus::cfl::classical
