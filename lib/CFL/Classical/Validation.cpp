#include "CFL/Classical/Validation.h"

#include <unordered_set>

namespace lotus::cfl::classical {

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
