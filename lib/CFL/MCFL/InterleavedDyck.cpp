#include "CFL/MCFL/InterleavedDyck.h"

#include <algorithm>
#include <charconv>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace lotus::cfl::mcfl {
namespace {

enum class DelimiterKind {
  OpenParenthesis,
  CloseParenthesis,
  OpenBracket,
  CloseBracket,
};

struct Delimiter {
  DelimiterKind kind;
  unsigned id = 0;
};

std::optional<Delimiter> parseDelimiter(std::string_view label) {
  if (label.size() <= 4 || label.substr(2, 2) != "--") {
    return std::nullopt;
  }
  unsigned id = 0;
  const std::string_view number = label.substr(4);
  const auto parsed =
      std::from_chars(number.data(), number.data() + number.size(), id);
  if (number.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != number.data() + number.size()) {
    return std::nullopt;
  }
  if (label.substr(0, 2) == "op") {
    return Delimiter{DelimiterKind::OpenParenthesis, id};
  }
  if (label.substr(0, 2) == "cp") {
    return Delimiter{DelimiterKind::CloseParenthesis, id};
  }
  if (label.substr(0, 2) == "ob") {
    return Delimiter{DelimiterKind::OpenBracket, id};
  }
  if (label.substr(0, 2) == "cb") {
    return Delimiter{DelimiterKind::CloseBracket, id};
  }
  return std::nullopt;
}

std::string openParenthesis(unsigned id) { return "op--" + std::to_string(id); }

std::string closeParenthesis(unsigned id) {
  return "cp--" + std::to_string(id);
}

std::string openBracket(unsigned id) { return "ob--" + std::to_string(id); }

std::string closeBracket(unsigned id) { return "cb--" + std::to_string(id); }

struct GrammarBuilder {
  Grammar::Nonterminal add(std::string name, std::size_t arity,
                           char family = 0) {
    const Grammar::Nonterminal result =
        model.grammar.addNonterminal(std::move(name), arity);
    if (family == 'P') {
      model.parenthesis_family.insert(result);
    } else if (family == 'Q') {
      model.bracket_family.insert(result);
    }
    return result;
  }

  Grammar::Nonterminal fresh(std::string_view prefix, std::size_t arity,
                             char family = 0) {
    return add("__" + std::string(prefix) + "_" + std::to_string(next_fresh++),
               arity, family);
  }

  InterleavedGrammar model;
  unsigned next_fresh = 0;
};

void addEmptyTuple(GrammarBuilder &builder, Grammar::Nonterminal target,
                   char family) {
  const std::size_t arity = builder.model.grammar.info(target).arity;
  if (arity == 1) {
    builder.model.grammar.addBasic(target, std::string(kEpsilonLabel));
    return;
  }

  Grammar::Nonterminal current = builder.fresh("empty", 1, family);
  builder.model.grammar.addBasic(current, std::string(kEpsilonLabel));
  for (std::size_t size = 1; size < arity; ++size) {
    const Grammar::Nonterminal head =
        size + 1 == arity ? target : builder.fresh("empty", size + 1, family);
    builder.model.grammar.addInsert(head, current, std::string(kEpsilonLabel),
                                    size);
    current = head;
  }
}

void addWrappingRule(GrammarBuilder &builder, Grammar::Nonterminal predicate,
                     const Label &open, const Label &close, char family) {
  const std::size_t arity = builder.model.grammar.info(predicate).arity;
  const Grammar::Nonterminal intermediate =
      builder.fresh("wrap", arity, family);
  builder.model.grammar.addAppend(intermediate, predicate, close, arity - 1);
  builder.model.grammar.addPrepend(predicate, intermediate, open, 0);
}

std::vector<std::vector<Grammar::VariableRef>>
familyConcatenation(std::size_t left_arity, std::size_t right_arity) {
  const std::size_t output_arity = left_arity + right_arity - 1;
  std::vector<std::vector<Grammar::VariableRef>> outputs(output_arity);
  for (std::size_t i = 0; i + 1 < left_arity; ++i) {
    outputs[i].push_back({0, i});
  }
  outputs[left_arity - 1].push_back({0, left_arity - 1});
  outputs[left_arity - 1].push_back({1, 0});
  for (std::size_t i = 1; i < right_arity; ++i) {
    outputs[left_arity - 1 + i].push_back({1, i});
  }
  return outputs;
}

std::vector<std::vector<Grammar::VariableRef>>
insertionOutputs(std::size_t arity, std::size_t component, bool before) {
  std::vector<std::vector<Grammar::VariableRef>> outputs(arity);
  for (std::size_t i = 0; i < arity; ++i) {
    if (i == component && before) {
      outputs[i].push_back({1, 0});
    }
    outputs[i].push_back({0, i});
    if (i == component && !before) {
      outputs[i].push_back({1, 0});
    }
  }
  return outputs;
}

std::vector<std::vector<Grammar::VariableRef>>
nestingOutputs(std::size_t arity, std::size_t component) {
  std::vector<std::vector<Grammar::VariableRef>> outputs(arity);
  for (std::size_t i = 0; i < arity; ++i) {
    if (i == component) {
      outputs[i].push_back({1, 0});
    }
    outputs[i].push_back({0, i});
    if (i == component) {
      outputs[i].push_back({1, 1});
    }
  }
  return outputs;
}

std::vector<std::vector<Grammar::VariableRef>>
outerNestingOutputs(std::size_t arity) {
  std::vector<std::vector<Grammar::VariableRef>> outputs(arity);
  for (std::size_t i = 0; i < arity; ++i) {
    if (i == 0) {
      outputs[i].push_back({1, 0});
    }
    outputs[i].push_back({0, i});
    if (i + 1 == arity) {
      outputs[i].push_back({1, 1});
    }
  }
  return outputs;
}

InterleavedGrammar buildProjectedGrammar(const InterleavedAlphabet &alphabet,
                                         bool parentheses) {
  GrammarBuilder builder;
  Grammar &grammar = builder.model.grammar;
  const Grammar::Nonterminal start = builder.add("D", 1);
  grammar.setStart(start);
  grammar.addBasic(start, std::string(kEpsilonLabel));
  grammar.addBasic(start, "normal");

  const std::vector<unsigned> &ignored =
      parentheses ? alphabet.brackets : alphabet.parentheses;
  for (unsigned id : ignored) {
    grammar.addBasic(start,
                     parentheses ? openBracket(id) : openParenthesis(id));
    grammar.addBasic(start,
                     parentheses ? closeBracket(id) : closeParenthesis(id));
  }

  const std::vector<unsigned> &balanced =
      parentheses ? alphabet.parentheses : alphabet.brackets;
  for (unsigned id : balanced) {
    addWrappingRule(builder, start,
                    parentheses ? openParenthesis(id) : openBracket(id),
                    parentheses ? closeParenthesis(id) : closeBracket(id), 0);
  }
  grammar.addConcatenate(start, {start, start}, {{{0, 0}, {1, 0}}});
  grammar.validate();
  return std::move(builder.model);
}

Graph filterMatchedDelimiters(const Graph &graph,
                              const InterleavedAlphabet &alphabet) {
  const std::unordered_set<unsigned> parentheses(alphabet.parentheses.begin(),
                                                 alphabet.parentheses.end());
  const std::unordered_set<unsigned> brackets(alphabet.brackets.begin(),
                                              alphabet.brackets.end());
  Graph filtered;
  for (Vertex vertex : graph.vertices()) {
    filtered.addVertex(vertex);
  }
  for (const Edge &edge : graph.edges()) {
    if (edge.label == "normal" || edge.label.empty()) {
      filtered.addEdge(edge.source, edge.target, edge.label);
      continue;
    }
    const std::optional<Delimiter> delimiter = parseDelimiter(edge.label);
    if (!delimiter) {
      continue;
    }
    const bool is_parenthesis =
        delimiter->kind == DelimiterKind::OpenParenthesis ||
        delimiter->kind == DelimiterKind::CloseParenthesis;
    if ((is_parenthesis ? parentheses : brackets).count(delimiter->id) != 0U) {
      filtered.addEdge(edge.source, edge.target, edge.label);
    }
  }
  return filtered;
}

std::vector<Graph> weakComponents(const Graph &graph) {
  std::unordered_map<Vertex, std::vector<Vertex>> neighbors;
  for (Vertex vertex : graph.vertices()) {
    neighbors.try_emplace(vertex);
  }
  for (const Edge &edge : graph.edges()) {
    neighbors[edge.source].push_back(edge.target);
    neighbors[edge.target].push_back(edge.source);
  }

  std::unordered_map<Vertex, std::size_t> component;
  std::size_t component_count = 0;
  for (Vertex vertex : graph.vertices()) {
    if (component.count(vertex) != 0U) {
      continue;
    }
    std::vector<Vertex> worklist{vertex};
    component.emplace(vertex, component_count);
    while (!worklist.empty()) {
      const Vertex current = worklist.back();
      worklist.pop_back();
      for (Vertex neighbor : neighbors[current]) {
        if (component.emplace(neighbor, component_count).second) {
          worklist.push_back(neighbor);
        }
      }
    }
    ++component_count;
  }

  std::vector<Graph> result(component_count);
  for (Vertex vertex : graph.vertices()) {
    result[component[vertex]].addVertex(vertex);
  }
  for (const Edge &edge : graph.edges()) {
    result[component[edge.source]].addEdge(edge.source, edge.target,
                                           edge.label);
  }
  return result;
}

class DisjointSet {
public:
  explicit DisjointSet(const std::vector<Vertex> &vertices) {
    for (Vertex vertex : vertices) {
      parent_[vertex] = vertex;
      weight_[vertex] = 1;
    }
  }

  Vertex find(Vertex vertex) {
    Vertex &parent = parent_.at(vertex);
    if (parent != vertex) {
      parent = find(parent);
    }
    return parent;
  }

  void join(Vertex first, Vertex second) {
    first = find(first);
    second = find(second);
    if (first == second) {
      return;
    }
    if (weight_[first] < weight_[second]) {
      std::swap(first, second);
    }
    weight_[first] += weight_[second];
    parent_[second] = first;
  }

private:
  std::unordered_map<Vertex, Vertex> parent_;
  std::unordered_map<Vertex, std::size_t> weight_;
};

struct Condensation {
  Graph graph;
  std::unordered_map<Vertex, std::vector<Vertex>> originals;
};

Condensation condense(const Graph &graph, const PairSet &under, bool enabled) {
  DisjointSet sets(graph.vertices());
  if (enabled) {
    std::unordered_map<Vertex, std::size_t> incoming;
    std::unordered_map<Vertex, std::size_t> outgoing;
    for (const Edge &edge : graph.edges()) {
      ++outgoing[edge.source];
      ++incoming[edge.target];
    }
    for (const Edge &edge : graph.edges()) {
      // The artifact graph has one implicit epsilon self-loop at every node.
      if (edge.label == "normal" && outgoing[edge.source] + 1 <= 2 &&
          incoming[edge.target] + 1 <= 2) {
        sets.join(edge.source, edge.target);
      }
    }
    for (const Pair &pair : under) {
      if (graph.containsVertex(pair.source) &&
          graph.containsVertex(pair.target) &&
          under.count({pair.target, pair.source}) != 0U) {
        sets.join(pair.source, pair.target);
      }
    }
  }

  Condensation result;
  for (Vertex vertex : graph.vertices()) {
    const Vertex representative = sets.find(vertex);
    result.graph.addVertex(representative);
    result.originals[representative].push_back(vertex);
  }
  for (const Edge &edge : graph.edges()) {
    result.graph.addEdge(sets.find(edge.source), sets.find(edge.target),
                         edge.label);
  }
  return result;
}

class ReachabilityCache {
public:
  explicit ReachabilityCache(const Graph &graph) {
    for (Vertex vertex : graph.vertices()) {
      adjacency_.try_emplace(vertex);
    }
    for (const Edge &edge : graph.edges()) {
      adjacency_[edge.source].push_back(edge.target);
    }
  }

  bool reaches(Vertex source, Vertex target) {
    if (source == target) {
      return true;
    }
    auto found = cache_.find(source);
    if (found == cache_.end()) {
      std::unordered_set<Vertex> reachable{source};
      std::vector<Vertex> worklist{source};
      while (!worklist.empty()) {
        const Vertex current = worklist.back();
        worklist.pop_back();
        for (Vertex next : adjacency_[current]) {
          if (reachable.insert(next).second) {
            worklist.push_back(next);
          }
        }
      }
      found = cache_.emplace(source, std::move(reachable)).first;
    }
    return found->second.count(target) != 0U;
  }

private:
  std::unordered_map<Vertex, std::vector<Vertex>> adjacency_;
  std::unordered_map<Vertex, std::unordered_set<Vertex>> cache_;
};

void addStats(SolverStats &target, const SolverStats &source) {
  target.facts += source.facts;
  target.worklist_pops += source.worklist_pops;
  target.rejected_duplicates += source.rejected_duplicates;
  target.rejected_unreachable_gaps += source.rejected_unreachable_gaps;
  target.type5_combinations += source.type5_combinations;
}

} // namespace

Graph adaptInterleavedDyckGraph(const interleaved_dyck::Graph &input) {
  Graph result;
  for (Vertex vertex : input.vertices()) {
    result.addVertex(vertex);
  }
  for (const interleaved_dyck::Edge &edge : input.edges()) {
    result.addEdge(edge.source, edge.target, edge.label.str());
  }
  return result;
}

InterleavedAlphabet discoverInterleavedAlphabet(const Graph &graph) {
  std::unordered_set<unsigned> open_parentheses;
  std::unordered_set<unsigned> close_parentheses;
  std::unordered_set<unsigned> open_brackets;
  std::unordered_set<unsigned> close_brackets;
  for (const Edge &edge : graph.edges()) {
    const std::optional<Delimiter> delimiter = parseDelimiter(edge.label);
    if (!delimiter) {
      continue;
    }
    switch (delimiter->kind) {
    case DelimiterKind::OpenParenthesis:
      open_parentheses.insert(delimiter->id);
      break;
    case DelimiterKind::CloseParenthesis:
      close_parentheses.insert(delimiter->id);
      break;
    case DelimiterKind::OpenBracket:
      open_brackets.insert(delimiter->id);
      break;
    case DelimiterKind::CloseBracket:
      close_brackets.insert(delimiter->id);
      break;
    }
  }

  InterleavedAlphabet result;
  for (unsigned id : open_parentheses) {
    if (close_parentheses.count(id) != 0U) {
      result.parentheses.push_back(id);
    }
  }
  for (unsigned id : open_brackets) {
    if (close_brackets.count(id) != 0U) {
      result.brackets.push_back(id);
    }
  }
  std::sort(result.parentheses.begin(), result.parentheses.end());
  std::sort(result.brackets.begin(), result.brackets.end());
  return result;
}

InterleavedGrammar
buildInterleavedDyckGrammar(unsigned dimension,
                            const InterleavedAlphabet &alphabet,
                            InterleavedGrammarVariant variant) {
  if (dimension == 0) {
    throw std::invalid_argument("MCFL dimension must be positive");
  }

  GrammarBuilder builder;
  Grammar &grammar = builder.model.grammar;
  const Grammar::Nonterminal start = builder.add("S", 1);
  grammar.setStart(start);
  grammar.addBasic(start, std::string(kEpsilonLabel));
  grammar.addBasic(start, "normal");

  std::vector<Grammar::Nonterminal> p(dimension + 1);
  std::vector<Grammar::Nonterminal> q(dimension + 1);
  for (unsigned arity = 1; arity <= dimension; ++arity) {
    p[arity] = builder.add("P" + std::to_string(arity), arity, 'P');
    q[arity] = builder.add("Q" + std::to_string(arity), arity, 'Q');
    addEmptyTuple(builder, p[arity], 'P');
    addEmptyTuple(builder, q[arity], 'Q');
    for (unsigned id : alphabet.parentheses) {
      addWrappingRule(builder, p[arity], openParenthesis(id),
                      closeParenthesis(id), 'P');
    }
    for (unsigned id : alphabet.brackets) {
      addWrappingRule(builder, q[arity], openBracket(id), closeBracket(id),
                      'Q');
    }
  }

  for (unsigned left = 1; left <= dimension; ++left) {
    for (unsigned right = 1; right <= dimension; ++right) {
      const unsigned output = left + right - 1;
      if (output > dimension) {
        continue;
      }
      grammar.addConcatenate(p[output], {p[left], p[right]},
                             familyConcatenation(left, right));
      grammar.addConcatenate(q[output], {q[left], q[right]},
                             familyConcatenation(left, right));
    }
  }

  std::vector<Grammar::VariableRef> p_first;
  std::vector<Grammar::VariableRef> q_first;
  for (unsigned i = 0; i < dimension; ++i) {
    p_first.push_back({0, i});
    p_first.push_back({1, i});
    q_first.push_back({1, i});
    q_first.push_back({0, i});
  }
  grammar.addConcatenate(start, {p[dimension], q[dimension]}, {p_first});
  grammar.addConcatenate(start, {p[dimension], q[dimension]}, {q_first});

  if (variant == InterleavedGrammarVariant::Full) {
    for (unsigned arity = 1; arity <= dimension; ++arity) {
      for (unsigned component = 0; component < arity; ++component) {
        grammar.addConcatenate(p[arity], {p[arity], start},
                               insertionOutputs(arity, component, true));
        grammar.addConcatenate(p[arity], {p[arity], start},
                               insertionOutputs(arity, component, false));
        grammar.addConcatenate(q[arity], {q[arity], start},
                               insertionOutputs(arity, component, true));
        grammar.addConcatenate(q[arity], {q[arity], start},
                               insertionOutputs(arity, component, false));
      }
    }

    if (dimension >= 2) {
      for (unsigned arity = 1; arity <= dimension; ++arity) {
        for (unsigned component = 0; component < arity; ++component) {
          grammar.addConcatenate(p[arity], {p[arity], q[2]},
                                 nestingOutputs(arity, component));
          grammar.addConcatenate(q[arity], {q[arity], p[2]},
                                 nestingOutputs(arity, component));
        }
        grammar.addConcatenate(p[arity], {p[arity], p[2]},
                               outerNestingOutputs(arity));
        grammar.addConcatenate(q[arity], {q[arity], q[2]},
                               outerNestingOutputs(arity));
      }
    }
  }

  grammar.validate();
  return std::move(builder.model);
}

const PairSet &InterleavedAnalysisResult::reachablePairs() const {
  static const PairSet empty;
  return dimensions.empty() ? empty : dimensions.back().reachable_pairs;
}

InterleavedAnalysisResult
InterleavedDyckSolver::solve(const Graph &input,
                             const InterleavedOptions &options) const {
  if (options.max_dimension == 0) {
    throw std::invalid_argument("MCFL dimension must be positive");
  }

  const InterleavedAlphabet input_alphabet = discoverInterleavedAlphabet(input);
  const Graph graph = filterMatchedDelimiters(input, input_alphabet);
  ReachabilityCache original_reachability(graph);
  PairSet previous;
  InterleavedAnalysisResult analysis;

  for (unsigned dimension = 1; dimension <= options.max_dimension;
       ++dimension) {
    Condensation condensed = condense(graph, previous, options.condense);
    PairSet compressed_pairs;
    SolverStats aggregate_stats;

    for (const Graph &component : weakComponents(condensed.graph)) {
      const InterleavedAlphabet alphabet =
          discoverInterleavedAlphabet(component);
      ReachabilityResult component_result;

      if (alphabet.parentheses.empty() || alphabet.brackets.empty()) {
        const bool balance_parentheses = !alphabet.parentheses.empty();
        InterleavedGrammar projected =
            buildProjectedGrammar(alphabet, balance_parentheses);
        component_result = Solver{}.solve(component, projected.grammar);
      } else {
        InterleavedGrammar model =
            buildInterleavedDyckGrammar(dimension, alphabet, options.variant);
        SolverOptions solver_options;
        if (dimension >= 2) {
          InterleavedGrammar alpha = buildProjectedGrammar(alphabet, true);
          InterleavedGrammar beta = buildProjectedGrammar(alphabet, false);
          const ReachabilityResult alpha_result =
              Solver{}.solve(component, alpha.grammar);
          const ReachabilityResult beta_result =
              Solver{}.solve(component, beta.grammar);
          const auto alpha_pairs =
              std::make_shared<PairSet>(alpha_result.reachablePairs());
          const auto beta_pairs =
              std::make_shared<PairSet>(beta_result.reachablePairs());
          solver_options.gap_reachable = [&model, alpha_pairs, beta_pairs](
                                             Grammar::Nonterminal nonterminal,
                                             Vertex from, Vertex to) {
            if (model.parenthesis_family.count(nonterminal) != 0U) {
              return alpha_pairs->count({from, to}) != 0U;
            }
            if (model.bracket_family.count(nonterminal) != 0U) {
              return beta_pairs->count({from, to}) != 0U;
            }
            return true;
          };
        }
        component_result =
            Solver{}.solve(component, model.grammar, solver_options);
      }

      addStats(aggregate_stats, component_result.stats());
      compressed_pairs.insert(component_result.reachablePairs().begin(),
                              component_result.reachablePairs().end());
    }

    PairSet expanded;
    for (const Pair &pair : compressed_pairs) {
      const auto sources = condensed.originals.find(pair.source);
      const auto targets = condensed.originals.find(pair.target);
      if (sources == condensed.originals.end() ||
          targets == condensed.originals.end()) {
        continue;
      }
      for (Vertex source : sources->second) {
        for (Vertex target : targets->second) {
          const bool artifact_expansion =
              options.expansion_policy ==
              CondensationExpansionPolicy::ArtifactCompatible;
          if (source != target &&
              (artifact_expansion ||
               original_reachability.reaches(source, target))) {
            expanded.insert({source, target});
          }
        }
      }
    }

    previous = expanded;
    analysis.dimensions.push_back(
        {dimension, std::move(expanded), aggregate_stats});
  }
  return analysis;
}

InterleavedAnalysisResult
InterleavedDyckSolver::solve(const interleaved_dyck::Graph &graph,
                             const InterleavedOptions &options) const {
  return solve(adaptInterleavedDyckGraph(graph), options);
}

} // namespace lotus::cfl::mcfl
