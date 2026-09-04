#include "CFL/Classical/Solvers/Engines/POCR/ClientGrammars.h"

#include "CFL/Classical/Core/Validation.h"

#include <set>
#include <string_view>

namespace lotus::cfl::classical::engines {
namespace {

constexpr std::string_view STANDARD_ALIAS = "Production:\n"
                                            "V\tAbar\tV\n"
                                            "M\tDV\td\n"
                                            "DV\tdbar\tV\n"
                                            "V\tV\tA\n"
                                            "V\tFV_i\tf_i\n"
                                            "V\tM\n"
                                            "V\n"
                                            "FV_i\tfbar_i\tV\n"
                                            "A\tA\tA\n"
                                            "A\ta\tM\n"
                                            "A\ta\n"
                                            "A\n"
                                            "Abar\tAbar\tAbar\n"
                                            "Abar\tM\tabar\n"
                                            "Abar\tabar\n"
                                            "Abar\n"
                                            "Count:\n"
                                            "V\n";

// GRAA/GRGspanAA remove the two explicitly transitive productions; all other
// unary and binary summaries are inherited from StdAA in the artifact.
constexpr std::string_view REWRITTEN_ALIAS = "Production:\n"
                                             "V\tAbar\tV\n"
                                             "M\tDV\td\n"
                                             "DV\tdbar\tV\n"
                                             "V\tV\tA\n"
                                             "V\tFV_i\tf_i\n"
                                             "V\tM\n"
                                             "V\n"
                                             "FV_i\tfbar_i\tV\n"
                                             "A\ta\tM\n"
                                             "A\ta\n"
                                             "A\n"
                                             "Abar\tM\tabar\n"
                                             "Abar\tabar\n"
                                             "Abar\n"
                                             "Count:\n"
                                             "V\n";

constexpr std::string_view STANDARD_VALUE_FLOW = "Production:\n"
                                                 "A\tA\tA\n"
                                                 "A\tCl_i\tret_i\n"
                                                 "A\ta\n"
                                                 "A\n"
                                                 "Cl_i\tcall_i\tA\n"
                                                 "Count:\n"
                                                 "A\n";

// GRVFA/GRGspanVFA introduce B and right-linearize A's concatenation. The
// inherited StdVFA initializer still seeds A's empty paths.
constexpr std::string_view REWRITTEN_VALUE_FLOW = "Production:\n"
                                                  "A\tA\ta\n"
                                                  "A\tA\tB\n"
                                                  "A\ta\n"
                                                  "A\n"
                                                  "B\tCl_i\tret_i\n"
                                                  "Cl_i\tcall_i\tA\n"
                                                  "Count:\n"
                                                  "A\n";

} // namespace

Grammar buildPocrClientGrammar(PocrClientGrammar grammar,
                               const LabeledGraph &graph) {
  GrammarParseOptions options = inferGrammarAttributes(graph);
  const bool alias = grammar == PocrClientGrammar::StandardAlias ||
                     grammar == PocrClientGrammar::RewrittenAlias;
  const std::vector<std::string> relevant_kinds =
      alias ? std::vector<std::string>{"f", "fbar"}
            : std::vector<std::string>{"call", "ret"};
  std::set<std::uint32_t> attributes;
  for (const std::string &kind : relevant_kinds) {
    if (const auto it = options.symbol_attributes.find(kind);
        it != options.symbol_attributes.end()) {
      attributes.insert(it->second.begin(), it->second.end());
    }
  }
  if (attributes.empty()) {
    attributes.insert(0);
  }
  auto &domain = options.variable_attributes['i'];
  domain.assign(attributes.begin(), attributes.end());

  std::string_view text;
  switch (grammar) {
  case PocrClientGrammar::StandardAlias:
    text = STANDARD_ALIAS;
    break;
  case PocrClientGrammar::RewrittenAlias:
    text = REWRITTEN_ALIAS;
    break;
  case PocrClientGrammar::StandardValueFlow:
    text = STANDARD_VALUE_FLOW;
    break;
  case PocrClientGrammar::RewrittenValueFlow:
    text = REWRITTEN_VALUE_FLOW;
    break;
  }
  return Grammar::parseFromText(std::string(text), options);
}

} // namespace lotus::cfl::classical::engines
