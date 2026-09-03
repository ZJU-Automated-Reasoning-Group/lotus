#include "CFL/InterleavedDyckApproximation/InterleavedDyckApproximation.h"
#include "CFL/MutualRefinement/CnfGrammar.h"
#include "CFL/MutualRefinement/CnfGraph.h"
#include "CFL/MutualRefinement/Hasher.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck_approximation {
namespace {

namespace mr = lotus::cfl::mutual_refinement;

constexpr unsigned MAX_PARITY_GROUPS = 4;

struct LabelHash {
  std::size_t operator()(const Label &label) const {
    std::size_t seed = static_cast<std::size_t>(label.kind);
    seed ^= std::hash<unsigned>{}(label.id) + 0x9e3779b9U + (seed << 6U) +
            (seed >> 2U);
    return seed;
  }
};

bool isParenthesis(LabelKind kind) {
  return kind == LabelKind::OpenParenthesis ||
         kind == LabelKind::CloseParenthesis;
}

bool isBracket(LabelKind kind) {
  return kind == LabelKind::OpenBracket || kind == LabelKind::CloseBracket;
}

bool isOpen(LabelKind kind) {
  return kind == LabelKind::OpenParenthesis || kind == LabelKind::OpenBracket;
}

bool belongsTo(LabelKind kind, Alphabet alphabet) {
  return alphabet == Alphabet::Parenthesis ? isParenthesis(kind)
                                           : isBracket(kind);
}

Label openLabel(Alphabet alphabet, unsigned id) {
  return alphabet == Alphabet::Parenthesis ? Label::openParenthesis(id)
                                           : Label::openBracket(id);
}

Label closeLabel(Alphabet alphabet, unsigned id) {
  return alphabet == Alphabet::Parenthesis ? Label::closeParenthesis(id)
                                           : Label::closeBracket(id);
}

PairSet intersect(const PairSet &left, const PairSet &right) {
  const PairSet *small = &left;
  const PairSet *large = &right;
  if (small->size() > large->size()) {
    std::swap(small, large);
  }
  PairSet result;
  for (const Pair &pair : *small) {
    if (large->count(pair) != 0U && pair.source != pair.target) {
      result.insert(pair);
    }
  }
  return result;
}

std::vector<unsigned> labelIds(const Graph &graph, Alphabet alphabet) {
  std::unordered_set<unsigned> ids;
  for (const Edge &edge : graph.edges()) {
    if (belongsTo(edge.label.kind, alphabet)) {
      ids.insert(edge.label.id);
    }
  }
  std::vector<unsigned> result(ids.begin(), ids.end());
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<unsigned> matchedLabelIds(const Graph &graph, Alphabet alphabet) {
  std::unordered_set<unsigned> opens;
  std::unordered_set<unsigned> closes;
  for (const Edge &edge : graph.edges()) {
    if (!belongsTo(edge.label.kind, alphabet)) {
      continue;
    }
    (isOpen(edge.label.kind) ? opens : closes).insert(edge.label.id);
  }
  std::vector<unsigned> result;
  for (unsigned id : opens) {
    if (closes.count(id) != 0U) {
      result.push_back(id);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

Graph retainMatchedLabels(const Graph &graph) {
  const auto parenthesis = matchedLabelIds(graph, Alphabet::Parenthesis);
  const auto bracket = matchedLabelIds(graph, Alphabet::Bracket);
  const std::unordered_set<unsigned> parenthesis_set(parenthesis.begin(),
                                                     parenthesis.end());
  const std::unordered_set<unsigned> bracket_set(bracket.begin(),
                                                 bracket.end());
  Graph result;
  for (const Edge &edge : graph.edges()) {
    if (edge.label.kind == LabelKind::Neutral ||
        (isParenthesis(edge.label.kind) &&
         parenthesis_set.count(edge.label.id) != 0U) ||
        (isBracket(edge.label.kind) &&
         bracket_set.count(edge.label.id) != 0U)) {
      result.addEdge(edge.source, edge.target, edge.label);
    }
  }
  return result;
}

using Adjacency =
    std::unordered_map<Vertex, std::vector<std::pair<Vertex, std::size_t>>>;

Adjacency adjacency(const Graph &graph, bool reverse = false) {
  Adjacency result;
  for (std::size_t index = 0; index < graph.edges().size(); ++index) {
    const Edge &edge = graph.edges()[index];
    const Vertex from = reverse ? edge.target : edge.source;
    const Vertex to = reverse ? edge.source : edge.target;
    result[from].push_back({to, index});
  }
  return result;
}

std::unordered_set<Vertex> reachable(const Adjacency &edges,
                                     const std::vector<Vertex> &starts) {
  std::unordered_set<Vertex> seen;
  std::deque<Vertex> worklist;
  for (Vertex start : starts) {
    if (seen.insert(start).second) {
      worklist.push_back(start);
    }
  }
  while (!worklist.empty()) {
    const Vertex current = worklist.front();
    worklist.pop_front();
    const auto found = edges.find(current);
    if (found == edges.end()) {
      continue;
    }
    for (const auto &[next, unused] : found->second) {
      (void)unused;
      if (seen.insert(next).second) {
        worklist.push_back(next);
      }
    }
  }
  return seen;
}

Graph removeNotOnCandidatePaths(const Graph &graph, const PairSet &pairs) {
  if (pairs.empty() || graph.empty()) {
    return {};
  }

  std::unordered_map<Vertex, std::vector<Vertex>> targets_by_source;
  for (const Pair &pair : pairs) {
    targets_by_source[pair.source].push_back(pair.target);
  }

  const Adjacency forward_edges = adjacency(graph);
  const Adjacency reverse_edges = adjacency(graph, true);
  std::vector<bool> keep(graph.edges().size(), false);
  for (const auto &[source, targets] : targets_by_source) {
    const auto from_source = reachable(forward_edges, {source});
    const auto to_target = reachable(reverse_edges, targets);
    for (std::size_t index = 0; index < graph.edges().size(); ++index) {
      const Edge &edge = graph.edges()[index];
      if (from_source.count(edge.source) != 0U &&
          to_target.count(edge.target) != 0U) {
        keep[index] = true;
      }
    }
  }

  Graph result;
  for (std::size_t index = 0; index < graph.edges().size(); ++index) {
    if (keep[index]) {
      const Edge &edge = graph.edges()[index];
      result.addEdge(edge.source, edge.target, edge.label);
    }
  }
  return result;
}

Graph removeValueFlowUnreachable(const Graph &graph) {
  std::vector<Vertex> sources;
  std::vector<Vertex> sinks;
  for (const Edge &edge : graph.edges()) {
    if (edge.label.kind == LabelKind::OpenBracket) {
      sources.push_back(edge.source);
    } else if (edge.label.kind == LabelKind::CloseBracket) {
      sinks.push_back(edge.target);
    }
  }
  if (sources.empty() || sinks.empty()) {
    return {};
  }

  const auto after_source = reachable(adjacency(graph), sources);
  const auto before_sink = reachable(adjacency(graph, true), sinks);
  Graph result;
  for (const Edge &edge : graph.edges()) {
    if (after_source.count(edge.source) != 0U &&
        after_source.count(edge.target) != 0U &&
        before_sink.count(edge.source) != 0U &&
        before_sink.count(edge.target) != 0U) {
      result.addEdge(edge.source, edge.target, edge.label);
    }
  }
  return result;
}

PairSet filterBracketPaths(const Graph &graph, const PairSet &pairs) {
  std::unordered_map<Vertex, std::vector<Vertex>> opens;
  std::unordered_map<Vertex, std::vector<Vertex>> closes;
  for (const Edge &edge : graph.edges()) {
    if (edge.label == Label::openBracket(0)) {
      opens[edge.source].push_back(edge.target);
    } else if (edge.label == Label::closeBracket(0)) {
      closes[edge.target].push_back(edge.source);
    }
  }

  const Adjacency forward = adjacency(graph);
  std::unordered_map<Vertex, std::unordered_set<Vertex>> reach_cache;
  PairSet result;
  for (const Pair &pair : pairs) {
    const auto open_it = opens.find(pair.source);
    const auto close_it = closes.find(pair.target);
    if (open_it == opens.end() || close_it == closes.end()) {
      continue;
    }
    bool accepted = false;
    for (Vertex open_target : open_it->second) {
      auto [cache_it, inserted] = reach_cache.try_emplace(open_target);
      if (inserted) {
        cache_it->second = reachable(forward, {open_target});
      }
      for (Vertex close_source : close_it->second) {
        if (cache_it->second.count(close_source) != 0U) {
          accepted = true;
          break;
        }
      }
      if (accepted) {
        break;
      }
    }
    if (accepted && pair.source != pair.target) {
      result.insert(pair);
    }
  }
  return result;
}

struct EncodedGrammar {
  mr::CnfGrammar grammar;
  std::unordered_map<Vertex, int> vertex_to_dense;
  std::vector<Vertex> dense_to_vertex;
  std::unordered_map<Label, int, LabelHash> label_to_terminal;
  std::unordered_map<int, Label> terminal_to_label;
  std::unordered_set<mr::Edge, mr::EdgeHasher> edges;
  int next_symbol = 0;

  explicit EncodedGrammar(const Graph &graph) {
    for (Vertex vertex : graph.vertices()) {
      const int dense = static_cast<int>(dense_to_vertex.size());
      vertex_to_dense.emplace(vertex, dense);
      dense_to_vertex.push_back(vertex);
    }

    std::vector<Label> labels;
    std::unordered_set<Label, LabelHash> seen;
    for (const Edge &edge : graph.edges()) {
      if (seen.insert(edge.label).second) {
        labels.push_back(edge.label);
      }
    }
    std::sort(labels.begin(), labels.end(),
              [](const Label &left, const Label &right) {
                return std::tie(left.kind, left.id) <
                       std::tie(right.kind, right.id);
              });
    for (const Label &label : labels) {
      const int symbol = next_symbol++;
      label_to_terminal.emplace(label, symbol);
      terminal_to_label.emplace(symbol, label);
      grammar.addTerminal(symbol);
    }
    for (const Edge &edge : graph.edges()) {
      edges.insert(std::make_tuple(vertex_to_dense.at(edge.source),
                                   label_to_terminal.at(edge.label),
                                   vertex_to_dense.at(edge.target)));
    }
  }

  int nonterminal() {
    const int symbol = next_symbol++;
    grammar.addNonterminal(symbol);
    return symbol;
  }

  std::optional<int> terminal(const Label &label) const {
    const auto found = label_to_terminal.find(label);
    if (found == label_to_terminal.end()) {
      return std::nullopt;
    }
    return found->second;
  }
};

void addTerminalProduction(EncodedGrammar &encoded, int lhs,
                           const Label &label) {
  if (const auto terminal = encoded.terminal(label)) {
    encoded.grammar.addUnaryProduction(lhs, *terminal);
  }
}

void addWrappedProductions(EncodedGrammar &encoded, int relation,
                           Alphabet alphabet,
                           const std::vector<unsigned> &ids) {
  for (unsigned id : ids) {
    const auto open_terminal = encoded.terminal(openLabel(alphabet, id));
    const auto close_terminal = encoded.terminal(closeLabel(alphabet, id));
    if (!open_terminal || !close_terminal) {
      continue;
    }
    const int open_nonterminal = encoded.nonterminal();
    const int close_nonterminal = encoded.nonterminal();
    const int tail = encoded.nonterminal();
    encoded.grammar.addUnaryProduction(open_nonterminal, *open_terminal);
    encoded.grammar.addUnaryProduction(close_nonterminal, *close_terminal);
    encoded.grammar.addBinaryProduction(tail, relation, close_nonterminal);
    encoded.grammar.addBinaryProduction(relation, open_nonterminal, tail);
  }
}

EncodedGrammar buildClassicGrammar(const Graph &graph, Alphabet balanced) {
  EncodedGrammar encoded(graph);
  const int start = encoded.nonterminal();
  encoded.grammar.addStartSymbol(start);
  encoded.grammar.addEmptyProduction(start);
  addTerminalProduction(encoded, start, Label::neutral());

  const Alphabet ignored = balanced == Alphabet::Parenthesis
                               ? Alphabet::Bracket
                               : Alphabet::Parenthesis;
  for (const auto &[label, terminal] : encoded.label_to_terminal) {
    if (belongsTo(label.kind, ignored)) {
      encoded.grammar.addUnaryProduction(start, terminal);
    }
  }
  encoded.grammar.addBinaryProduction(start, start, start);
  addWrappedProductions(encoded, start, balanced,
                        matchedLabelIds(graph, balanced));
  encoded.grammar.initFastIndices();
  return encoded;
}

EncodedGrammar buildCombinedGrammar(const Graph &graph) {
  EncodedGrammar encoded(graph);
  const int start = encoded.nonterminal();
  encoded.grammar.addStartSymbol(start);
  encoded.grammar.addEmptyProduction(start);
  addTerminalProduction(encoded, start, Label::neutral());
  encoded.grammar.addBinaryProduction(start, start, start);
  addWrappedProductions(encoded, start, Alphabet::Parenthesis,
                        matchedLabelIds(graph, Alphabet::Parenthesis));
  addWrappedProductions(encoded, start, Alphabet::Bracket,
                        matchedLabelIds(graph, Alphabet::Bracket));
  encoded.grammar.initFastIndices();
  return encoded;
}

std::size_t parityState(unsigned mask, bool leading_close, bool trailing_open) {
  return (static_cast<std::size_t>(mask) * 2U +
          static_cast<unsigned>(leading_close)) *
             2U +
         static_cast<unsigned>(trailing_open);
}

EncodedGrammar buildParityGrammar(const Graph &graph, Alphabet balanced,
                                  unsigned parity_groups) {
  if (parity_groups == 0 || parity_groups > MAX_PARITY_GROUPS) {
    throw std::invalid_argument("parity_groups must be between 1 and " +
                                std::to_string(MAX_PARITY_GROUPS));
  }

  EncodedGrammar encoded(graph);
  const int start = encoded.nonterminal();
  const int empty = encoded.nonterminal();
  const unsigned mask_count = 1U << parity_groups;
  std::vector<int> states(static_cast<std::size_t>(mask_count) * 4U);
  for (int &state : states) {
    state = encoded.nonterminal();
  }

  encoded.grammar.addStartSymbol(start);
  encoded.grammar.addUnaryProduction(start, empty);
  encoded.grammar.addUnaryProduction(start,
                                     states[parityState(0, false, false)]);
  encoded.grammar.addEmptyProduction(empty);
  addTerminalProduction(encoded, empty, Label::neutral());
  encoded.grammar.addBinaryProduction(empty, empty, empty);

  const Alphabet ignored = balanced == Alphabet::Parenthesis
                               ? Alphabet::Bracket
                               : Alphabet::Parenthesis;
  const auto ignored_ids = labelIds(graph, ignored);
  std::unordered_map<unsigned, unsigned> group;
  for (std::size_t index = 0; index < ignored_ids.size(); ++index) {
    group[ignored_ids[index]] = static_cast<unsigned>(index) % parity_groups;
  }
  for (const auto &[label, terminal] : encoded.label_to_terminal) {
    if (!belongsTo(label.kind, ignored)) {
      continue;
    }
    const unsigned mask = 1U << group.at(label.id);
    const bool leading_close = !isOpen(label.kind);
    const bool trailing_open = isOpen(label.kind);
    encoded.grammar.addUnaryProduction(
        states[parityState(mask, leading_close, trailing_open)], terminal);
  }

  const auto balanced_ids = matchedLabelIds(graph, balanced);
  addWrappedProductions(encoded, empty, balanced, balanced_ids);
  for (int state : states) {
    addWrappedProductions(encoded, state, balanced, balanced_ids);
    encoded.grammar.addBinaryProduction(state, state, empty);
    encoded.grammar.addBinaryProduction(state, empty, state);
  }

  for (unsigned left_mask = 0; left_mask < mask_count; ++left_mask) {
    for (unsigned left_close = 0; left_close < 2; ++left_close) {
      for (unsigned left_open = 0; left_open < 2; ++left_open) {
        const int left =
            states[parityState(left_mask, left_close != 0, left_open != 0)];
        for (unsigned right_mask = 0; right_mask < mask_count; ++right_mask) {
          for (unsigned right_close = 0; right_close < 2; ++right_close) {
            for (unsigned right_open = 0; right_open < 2; ++right_open) {
              const int right = states[parityState(right_mask, right_close != 0,
                                                   right_open != 0)];
              const int output = states[parityState(
                  left_mask ^ right_mask, left_close != 0, right_open != 0)];
              encoded.grammar.addBinaryProduction(output, left, right);
            }
          }
        }
      }
    }
  }

  encoded.grammar.initFastIndices();
  return encoded;
}

struct ReachabilityRun {
  PairSet pairs;
  Graph used_edges;
};

ReachabilityRun
runGrammar(EncodedGrammar encoded, bool trace, bool factorized_tracing = false,
           const std::optional<Pair> &trace_pair = std::nullopt) {
  if (encoded.dense_to_vertex.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::overflow_error("too many vertices for the CFL engine");
  }

  mr::CnfGraph engine;
  engine.reinit(static_cast<int>(encoded.dense_to_vertex.size()),
                encoded.edges);
  std::unordered_map<mr::Edge, std::unordered_set<int>, mr::EdgeHasher>
      unary_record;
  std::unordered_map<
      mr::Edge,
      std::unordered_set<std::tuple<int, int, int>, mr::IntTripleHasher>,
      mr::EdgeHasher>
      binary_record;
  std::unordered_set<mr::Edge, mr::EdgeHasher> raw_result;
  if (trace) {
    if (factorized_tracing) {
      raw_result = engine.runCFLReachability(encoded.grammar);
    } else {
      raw_result = engine.runCFLReachability(encoded.grammar, unary_record,
                                             binary_record);
    }
  } else {
    raw_result = engine.runCFLReachability(encoded.grammar);
  }

  ReachabilityRun result;
  std::unordered_set<mr::Edge, mr::EdgeHasher> closure_roots;
  for (const mr::Edge &edge : raw_result) {
    const Pair pair{encoded.dense_to_vertex.at(std::get<0>(edge)),
                    encoded.dense_to_vertex.at(std::get<2>(edge))};
    if (pair.source == pair.target) {
      continue;
    }
    result.pairs.insert(pair);
    if (trace && (!trace_pair || pair == *trace_pair)) {
      closure_roots.insert(edge);
    }
  }

  if (!trace) {
    return result;
  }
  const auto closure =
      factorized_tracing
          ? engine.getFactorizedEdgeClosure(encoded.grammar, closure_roots)
          : engine.getEdgeClosure(encoded.grammar, closure_roots, unary_record,
                                  binary_record);
  for (const mr::Edge &edge : closure) {
    const auto label = encoded.terminal_to_label.find(std::get<1>(edge));
    if (label == encoded.terminal_to_label.end()) {
      continue;
    }
    result.used_edges.addEdge(encoded.dense_to_vertex.at(std::get<0>(edge)),
                              encoded.dense_to_vertex.at(std::get<2>(edge)),
                              label->second);
  }
  return result;
}

ReachabilityRun
runProjected(const Graph &graph, Alphabet balanced, GrammarStrength strength,
             unsigned parity_groups, bool trace = false,
             bool factorized_tracing = false,
             const std::optional<Pair> &trace_pair = std::nullopt) {
  if (strength == GrammarStrength::Parity) {
    return runGrammar(buildParityGrammar(graph, balanced, parity_groups), trace,
                      factorized_tracing, trace_pair);
  }
  return runGrammar(buildClassicGrammar(graph, balanced), trace,
                    factorized_tracing, trace_pair);
}

PairSet runCombined(const Graph &graph) {
  return runGrammar(buildCombinedGrammar(graph), false).pairs;
}

Vertex productVertex(Vertex vertex, std::size_t states, std::size_t state) {
  const __int128 value = static_cast<__int128>(vertex) * states + state;
  if (value < std::numeric_limits<Vertex>::min() ||
      value > std::numeric_limits<Vertex>::max()) {
    throw std::overflow_error("product-automaton vertex id overflow");
  }
  return static_cast<Vertex>(value);
}

Graph automatonProduct(const Graph &graph, BenchmarkKind benchmark,
                       std::size_t &state_count, std::size_t &accept_state) {
  Graph product;
  if (benchmark == BenchmarkKind::ValueFlow) {
    state_count = 6;
    accept_state = 2;
    for (const Edge &edge : graph.edges()) {
      const auto add = [&](std::size_t from, std::size_t to, Label label) {
        product.addEdge(productVertex(edge.source, state_count, from),
                        productVertex(edge.target, state_count, to), label);
      };
      if (edge.label.kind == LabelKind::OpenBracket) {
        add(0, 3, Label::neutral());
        add(1, 3, Label::neutral());
        add(2, 3, Label::neutral());
        add(3, 4, Label::neutral());
        add(4, 4, Label::neutral());
        add(5, 4, Label::neutral());
      } else if (edge.label.kind == LabelKind::CloseBracket) {
        add(3, 2, Label::neutral());
        add(4, 5, Label::neutral());
        add(5, 5, Label::neutral());
      } else {
        add(1, 1, edge.label);
        add(2, 1, edge.label);
        add(3, 3, edge.label);
        add(4, 4, edge.label);
        add(5, 4, edge.label);
      }
    }
    return product;
  }

  const auto brackets = labelIds(graph, Alphabet::Bracket);
  state_count = brackets.size() + 2;
  accept_state = 0;
  std::unordered_map<unsigned, std::size_t> bracket_state;
  for (std::size_t index = 0; index < brackets.size(); ++index) {
    bracket_state[brackets[index]] = index + 1;
  }
  for (const Edge &edge : graph.edges()) {
    const auto add = [&](std::size_t from, std::size_t to, Label label) {
      product.addEdge(productVertex(edge.source, state_count, from),
                      productVertex(edge.target, state_count, to), label);
    };
    if (edge.label.kind == LabelKind::OpenBracket) {
      for (std::size_t state = 1; state < state_count; ++state) {
        add(state, state_count - 1, Label::neutral());
      }
      add(0, bracket_state.at(edge.label.id), Label::neutral());
    } else if (edge.label.kind == LabelKind::CloseBracket) {
      add(bracket_state.at(edge.label.id), 0, Label::neutral());
      add(state_count - 1, state_count - 1, Label::neutral());
    } else {
      for (std::size_t state = 0; state < state_count; ++state) {
        add(state, state, edge.label);
      }
    }
  }
  return product;
}

PairSet regularization(const Graph &graph, BenchmarkKind benchmark) {
  std::size_t states = 0;
  std::size_t accept_state = 0;
  const Graph product =
      automatonProduct(graph, benchmark, states, accept_state);
  const PairSet product_pairs =
      runProjected(product, Alphabet::Parenthesis, GrammarStrength::Classic, 2)
          .pairs;
  PairSet result;
  for (const Pair &pair : product_pairs) {
    const auto source_remainder = pair.source % static_cast<Vertex>(states);
    auto target_remainder = pair.target % static_cast<Vertex>(states);
    if (source_remainder < 0) {
      continue;
    }
    if (target_remainder < 0) {
      target_remainder += static_cast<Vertex>(states);
    }
    if (source_remainder == 0 &&
        (target_remainder == static_cast<Vertex>(accept_state) ||
         target_remainder == static_cast<Vertex>(states - 1))) {
      const Pair mapped{pair.source / static_cast<Vertex>(states),
                        pair.target / static_cast<Vertex>(states)};
      if (mapped.source != mapped.target) {
        result.insert(mapped);
      }
    }
  }
  return result;
}

Graph valueFlowTransform(const Graph &graph) {
  Graph transformed;
  for (const Edge &edge : graph.edges()) {
    transformed.addEdge(productVertex(edge.source, 3, 1),
                        productVertex(edge.target, 3, 1), edge.label);
    if (edge.label.kind == LabelKind::OpenBracket) {
      transformed.addEdge(productVertex(edge.source, 3, 0),
                          productVertex(edge.target, 3, 1), edge.label);
    } else if (edge.label.kind == LabelKind::CloseBracket) {
      transformed.addEdge(productVertex(edge.source, 3, 1),
                          productVertex(edge.target, 3, 2), edge.label);
    }
  }
  return transformed;
}

PairSet filterValueFlowPairs(const PairSet &pairs) {
  PairSet result;
  for (const Pair &pair : pairs) {
    Vertex source_mod = pair.source % 3;
    Vertex target_mod = pair.target % 3;
    if (source_mod < 0) {
      source_mod += 3;
    }
    if (target_mod < 0) {
      target_mod += 3;
    }
    if (source_mod == 0 && target_mod == 2) {
      const Pair mapped{pair.source / 3, pair.target / 3};
      if (mapped.source != mapped.target) {
        result.insert(mapped);
      }
    }
  }
  return result;
}

std::vector<Graph> weakComponents(const Graph &graph) {
  std::unordered_map<Vertex, std::vector<Vertex>> undirected;
  for (const Edge &edge : graph.edges()) {
    undirected[edge.source].push_back(edge.target);
    undirected[edge.target].push_back(edge.source);
  }

  std::unordered_map<Vertex, std::size_t> component_of;
  std::size_t component_count = 0;
  for (Vertex vertex : graph.vertices()) {
    if (component_of.count(vertex) != 0U) {
      continue;
    }
    std::deque<Vertex> worklist{vertex};
    component_of[vertex] = component_count;
    while (!worklist.empty()) {
      const Vertex current = worklist.front();
      worklist.pop_front();
      for (Vertex next : undirected[current]) {
        if (component_of.emplace(next, component_count).second) {
          worklist.push_back(next);
        }
      }
    }
    ++component_count;
  }

  std::vector<Graph> components(component_count);
  for (const Edge &edge : graph.edges()) {
    components[component_of.at(edge.source)].addEdge(edge.source, edge.target,
                                                     edge.label);
  }
  return components;
}

PairSet mutualRefinementImpl(const Graph &input, GrammarStrength strength,
                             unsigned parity_groups, BenchmarkKind benchmark,
                             bool factorized_tracing,
                             const std::optional<Pair> &target = std::nullopt) {
  const std::vector<Graph> components = weakComponents(input);
  if (components.size() > 1U) {
    PairSet result;
    for (const Graph &component : components) {
      PairSet component_result =
          mutualRefinementImpl(component, strength, parity_groups, benchmark,
                               factorized_tracing, target);
      result.insert(component_result.begin(), component_result.end());
    }
    return result;
  }

  Graph graph =
      target ? removeNotOnCandidatePaths(input, PairSet{*target}) : input;
  graph = retainMatchedLabels(graph);
  PairSet alpha_paths;
  PairSet beta_paths;

  while (!graph.empty()) {
    const std::size_t old_edge_count = graph.edges().size();
    auto alpha = runProjected(graph, Alphabet::Parenthesis, strength,
                              parity_groups, true, factorized_tracing, target);
    alpha_paths = std::move(alpha.pairs);
    if (target && alpha_paths.count(*target) == 0U) {
      return {};
    }
    graph = retainMatchedLabels(alpha.used_edges);
    if (graph.empty()) {
      return {};
    }

    auto beta = runProjected(graph, Alphabet::Bracket, strength, parity_groups,
                             true, factorized_tracing, target);
    beta_paths = std::move(beta.pairs);
    if (target && beta_paths.count(*target) == 0U) {
      return {};
    }
    graph = beta.used_edges;

    if (benchmark == BenchmarkKind::ValueFlow) {
      beta_paths = filterBracketPaths(graph, beta_paths);
      if (target && beta_paths.count(*target) == 0U) {
        return {};
      }
      graph = removeValueFlowUnreachable(graph);
    }
    graph = retainMatchedLabels(graph);

    if (graph.empty() || graph.edges().size() == old_edge_count) {
      break;
    }
  }

  PairSet result = intersect(alpha_paths, beta_paths);
  if (target) {
    if (result.count(*target) != 0U) {
      return PairSet{*target};
    }
    return {};
  }
  return result;
}

class DisjointSet {
public:
  explicit DisjointSet(const std::vector<Vertex> &vertices) {
    for (Vertex vertex : vertices) {
      parent_[vertex] = vertex;
      size_[vertex] = 1;
    }
  }

  Vertex find(Vertex vertex) {
    Vertex &parent = parent_.at(vertex);
    if (parent != vertex) {
      parent = find(parent);
    }
    return parent;
  }

  void join(Vertex left, Vertex right) {
    left = find(left);
    right = find(right);
    if (left == right) {
      return;
    }
    if (size_.at(left) < size_.at(right)) {
      std::swap(left, right);
    }
    parent_[right] = left;
    size_[left] += size_[right];
  }

private:
  std::unordered_map<Vertex, Vertex> parent_;
  std::unordered_map<Vertex, std::size_t> size_;
};

struct Condensation {
  Graph graph;
  std::unordered_map<Vertex, Vertex> root;
  std::unordered_map<Vertex, std::vector<Vertex>> groups;
};

Condensation condense(const Graph &graph, const PairSet &underapproximation) {
  DisjointSet sets(graph.vertices());
  for (const Pair &pair : underapproximation) {
    if (pair.source != pair.target &&
        underapproximation.count({pair.target, pair.source}) != 0U) {
      sets.join(pair.source, pair.target);
    }
  }

  Condensation result;
  for (Vertex vertex : graph.vertices()) {
    const Vertex root = sets.find(vertex);
    result.root[vertex] = root;
    result.groups[root].push_back(vertex);
  }
  for (const Edge &edge : graph.edges()) {
    result.graph.addEdge(result.root.at(edge.source),
                         result.root.at(edge.target), edge.label);
  }
  return result;
}

PairSet expandCondensedPairs(const Condensation &condensation,
                             const PairSet &condensed_pairs) {
  PairSet result;
  for (const Pair &pair : condensed_pairs) {
    const auto source_group = condensation.groups.find(pair.source);
    const auto target_group = condensation.groups.find(pair.target);
    if (source_group == condensation.groups.end() ||
        target_group == condensation.groups.end()) {
      continue;
    }
    for (Vertex source : source_group->second) {
      for (Vertex target : target_group->second) {
        if (source != target) {
          result.insert({source, target});
        }
      }
    }
  }
  for (const auto &[unused, group] : condensation.groups) {
    (void)unused;
    for (Vertex source : group) {
      for (Vertex target : group) {
        if (source != target) {
          result.insert({source, target});
        }
      }
    }
  }
  return result;
}

PairSet refinedWithCondensation(const Graph &graph,
                                const PairSet &underapproximation,
                                GrammarStrength strength,
                                unsigned parity_groups, BenchmarkKind benchmark,
                                bool factorized_tracing) {
  const Condensation condensation = condense(graph, underapproximation);
  const PairSet condensed =
      mutualRefinementImpl(condensation.graph, strength, parity_groups,
                           benchmark, factorized_tracing);
  return expandCondensedPairs(condensation, condensed);
}

PairSet onDemand(const Graph &graph, const PairSet &underapproximation,
                 const PairSet &overapproximation, GrammarStrength strength,
                 unsigned parity_groups, BenchmarkKind benchmark,
                 bool factorized_tracing) {
  const Condensation condensation = condense(graph, underapproximation);
  PairSet result = underapproximation;
  std::unordered_map<Pair, bool, PairHash> memory;
  for (const Pair &candidate : overapproximation) {
    if (underapproximation.count(candidate) != 0U) {
      continue;
    }
    const auto source = condensation.root.find(candidate.source);
    const auto target = condensation.root.find(candidate.target);
    if (source == condensation.root.end() ||
        target == condensation.root.end()) {
      continue;
    }
    const Pair root_pair{source->second, target->second};
    auto found = memory.find(root_pair);
    if (found == memory.end()) {
      const bool accepted =
          mutualRefinementImpl(condensation.graph, strength, parity_groups,
                               benchmark, factorized_tracing, root_pair)
              .count(root_pair) != 0U;
      found = memory.emplace(root_pair, accepted).first;
    }
    if (found->second) {
      result.insert(candidate);
    }
  }
  return result;
}

} // namespace

PairSet Solver::projectedReachability(const Graph &graph,
                                      Alphabet balanced_alphabet,
                                      GrammarStrength strength,
                                      unsigned parity_groups) const {
  return runProjected(graph, balanced_alphabet, strength, parity_groups).pairs;
}

PairSet Solver::intersection(const Graph &graph, GrammarStrength strength,
                             unsigned parity_groups) const {
  const PairSet alpha = projectedReachability(graph, Alphabet::Parenthesis,
                                              strength, parity_groups);
  const PairSet beta =
      projectedReachability(graph, Alphabet::Bracket, strength, parity_groups);
  return intersect(alpha, beta);
}

PairSet Solver::underapproximation(const Graph &graph) const {
  return runCombined(graph);
}

PairSet Solver::mutualRefinement(const Graph &graph, GrammarStrength strength,
                                 unsigned parity_groups,
                                 BenchmarkKind benchmark,
                                 bool factorized_tracing) const {
  return mutualRefinementImpl(graph, strength, parity_groups, benchmark,
                              factorized_tracing);
}

ApproximationResult Solver::analyze(const Graph &input, BenchmarkKind benchmark,
                                    const Options &options) const {
  if (options.parity_groups == 0 || options.parity_groups > MAX_PARITY_GROUPS) {
    throw std::invalid_argument("parity_groups must be between 1 and " +
                                std::to_string(MAX_PARITY_GROUPS));
  }

  ApproximationResult result;
  Graph graph = benchmark == BenchmarkKind::ValueFlow
                    ? removeValueFlowUnreachable(input)
                    : input;
  result.regularization = regularization(graph, benchmark);
  result.intersection =
      intersection(graph, GrammarStrength::Classic, options.parity_groups);
  if (benchmark == BenchmarkKind::ValueFlow) {
    result.intersection = filterBracketPaths(graph, result.intersection);
  }

  graph = retainMatchedLabels(
      removeNotOnCandidatePaths(graph, result.intersection));
  Graph under_graph = graph;
  if (benchmark == BenchmarkKind::ValueFlow) {
    under_graph = valueFlowTransform(under_graph);
  }
  result.underapproximation = underapproximation(under_graph);
  if (benchmark == BenchmarkKind::ValueFlow) {
    result.underapproximation = filterValueFlowPairs(result.underapproximation);
  }

  result.mutual_refinement = refinedWithCondensation(
      graph, result.underapproximation, GrammarStrength::Classic,
      options.parity_groups, benchmark, options.factorized_tracing);
  graph = retainMatchedLabels(
      removeNotOnCandidatePaths(graph, result.mutual_refinement));

  result.stronger_grammar = refinedWithCondensation(
      graph, result.underapproximation, GrammarStrength::Parity,
      options.parity_groups, benchmark, options.factorized_tracing);
  graph = retainMatchedLabels(
      removeNotOnCandidatePaths(graph, result.stronger_grammar));

  if (!options.run_on_demand) {
    result.on_demand = result.stronger_grammar;
    return result;
  }

  const PairSet classic_on_demand =
      onDemand(graph, result.underapproximation, result.stronger_grammar,
               GrammarStrength::Classic, options.parity_groups, benchmark,
               options.factorized_tracing);
  graph =
      retainMatchedLabels(removeNotOnCandidatePaths(graph, classic_on_demand));
  result.on_demand =
      onDemand(graph, result.underapproximation, classic_on_demand,
               GrammarStrength::Parity, options.parity_groups, benchmark,
               options.factorized_tracing);
  return result;
}

} // namespace lotus::cfl::interleaved_dyck_approximation
