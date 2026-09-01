#include "CFL/Classical/Validation.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace lotus::cfl::classical {

GrammarParseOptions inferGrammarAttributes(const LabeledGraph &graph) {
  GrammarParseOptions options;
  for (const auto &[label, _] : graph.symbolPairs()) {
    const auto separator = label.find_last_of('_');
    if (separator == std::string::npos || separator + 1 == label.size()) {
      continue;
    }
    const std::string value = label.substr(separator + 1);
    if (!std::all_of(value.begin(), value.end(), [](unsigned char character) {
          return std::isdigit(character) != 0;
        })) {
      continue;
    }
    const std::uint32_t attribute =
        static_cast<std::uint32_t>(std::stoul(value));
    options.symbol_attributes[label.substr(0, separator)].push_back(attribute);
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
