#include "CFL/MCFL/Solver.h"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lotus::cfl::mcfl {
namespace {

template <typename T> void hashCombine(std::size_t &seed, const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

struct FactHash {
  std::size_t operator()(const Fact &fact) const {
    std::size_t seed = std::hash<Grammar::Nonterminal>{}(fact.nonterminal);
    for (const Span &span : fact.spans) {
      hashCombine(seed, span.source);
      hashCombine(seed, span.target);
    }
    return seed;
  }
};

struct EndpointKey {
  Grammar::Nonterminal nonterminal = 0;
  std::size_t component = 0;
  bool start = false;
  Vertex vertex = 0;

  bool operator==(const EndpointKey &other) const {
    return nonterminal == other.nonterminal && component == other.component &&
           start == other.start && vertex == other.vertex;
  }
};

struct EndpointKeyHash {
  std::size_t operator()(const EndpointKey &key) const {
    std::size_t seed = std::hash<Grammar::Nonterminal>{}(key.nonterminal);
    hashCombine(seed, key.component);
    hashCombine(seed, key.start);
    hashCombine(seed, key.vertex);
    return seed;
  }
};

class PlainReachability {
public:
  explicit PlainReachability(const Graph &graph) {
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
    auto found = reachable_.find(source);
    if (found == reachable_.end()) {
      found = reachable_.emplace(source, compute(source)).first;
    }
    return found->second.count(target) != 0U;
  }

private:
  std::unordered_set<Vertex> compute(Vertex source) const {
    std::unordered_set<Vertex> seen;
    std::vector<Vertex> worklist{source};
    seen.insert(source);
    while (!worklist.empty()) {
      const Vertex current = worklist.back();
      worklist.pop_back();
      const auto found = adjacency_.find(current);
      if (found == adjacency_.end()) {
        continue;
      }
      for (Vertex successor : found->second) {
        if (seen.insert(successor).second) {
          worklist.push_back(successor);
        }
      }
    }
    return seen;
  }

  std::unordered_map<Vertex, std::vector<Vertex>> adjacency_;
  std::unordered_map<Vertex, std::unordered_set<Vertex>> reachable_;
};

enum class ProofKind {
  Basic,
  EpsilonPrepend,
  EpsilonAppend,
  Prepend,
  Append,
  Insert,
  Concatenate,
};

struct Proof {
  ProofKind kind = ProofKind::Basic;
  std::size_t rule = 0;
  std::vector<std::size_t> premises;
  Edge edge;
};

enum class TriggerKind {
  Prepend,
  Append,
  Insert,
  Concatenate,
};

struct Trigger {
  TriggerKind kind = TriggerKind::Prepend;
  std::size_t rule = 0;
  std::size_t body = 0;
};

struct State {
  State(const Graph &input_graph, const Grammar &input_grammar,
        const SolverOptions &input_options)
      : graph(input_graph), grammar(input_grammar), options(input_options),
        plain_reachability(input_graph),
        facts_by_nonterminal(grammar.nonterminals().size()),
        triggers(grammar.nonterminals().size()) {
    for (Vertex vertex : graph.vertices()) {
      graph.addEdge(vertex, vertex, std::string(kEpsilonLabel));
    }
    buildTriggers();
  }

  void buildTriggers() {
    for (std::size_t i = 0; i < grammar.prependRules().size(); ++i) {
      triggers[grammar.prependRules()[i].body].push_back(
          {TriggerKind::Prepend, i, 0});
    }
    for (std::size_t i = 0; i < grammar.appendRules().size(); ++i) {
      triggers[grammar.appendRules()[i].body].push_back(
          {TriggerKind::Append, i, 0});
    }
    for (std::size_t i = 0; i < grammar.insertRules().size(); ++i) {
      triggers[grammar.insertRules()[i].body].push_back(
          {TriggerKind::Insert, i, 0});
    }
    for (std::size_t i = 0; i < grammar.concatenateRules().size(); ++i) {
      const Grammar::ConcatenateRule &rule = grammar.concatenateRules()[i];
      for (std::size_t body = 0; body < rule.bodies.size(); ++body) {
        triggers[rule.bodies[body]].push_back(
            {TriggerKind::Concatenate, i, body});
      }
    }
  }

  bool gapReachable(Grammar::Nonterminal nonterminal, Vertex from, Vertex to) {
    if (!options.prune_unreachable_gaps) {
      return true;
    }
    if (options.gap_reachable) {
      return options.gap_reachable(nonterminal, from, to);
    }
    return plain_reachability.reaches(from, to);
  }

  bool viable(const Fact &fact) {
    for (std::size_t i = 1; i < fact.spans.size(); ++i) {
      if (!gapReachable(fact.nonterminal, fact.spans[i - 1].target,
                        fact.spans[i].source)) {
        ++stats.rejected_unreachable_gaps;
        return false;
      }
    }
    return true;
  }

  std::optional<std::size_t> addFact(Fact fact, Proof proof) {
    if (fact.spans.size() != grammar.info(fact.nonterminal).arity) {
      throw std::logic_error("MCFL solver produced a fact with wrong arity");
    }
    if (!viable(fact)) {
      return std::nullopt;
    }
    const auto inserted = fact_ids.emplace(fact, facts.size());
    if (!inserted.second) {
      ++stats.rejected_duplicates;
      return std::nullopt;
    }

    const std::size_t id = facts.size();
    facts.push_back(std::move(fact));
    proofs.push_back(std::move(proof));
    worklist.push_back(id);
    facts_by_nonterminal[facts[id].nonterminal].push_back(id);
    for (std::size_t component = 0; component < facts[id].spans.size();
         ++component) {
      const Span &span = facts[id].spans[component];
      endpoint_index[{facts[id].nonterminal, component, true, span.source}]
          .push_back(id);
      endpoint_index[{facts[id].nonterminal, component, false, span.target}]
          .push_back(id);
    }
    if (facts[id].nonterminal == grammar.start()) {
      const Pair pair{facts[id].spans[0].source, facts[id].spans[0].target};
      start_fact.try_emplace(pair, id);
    }
    return id;
  }

  const std::vector<std::size_t> &indexedFacts(const EndpointKey &key) const {
    static const std::vector<std::size_t> empty;
    const auto found = endpoint_index.find(key);
    return found == endpoint_index.end() ? empty : found->second;
  }

  void initialize() {
    for (std::size_t rule_index = 0; rule_index < grammar.basicRules().size();
         ++rule_index) {
      const Grammar::BasicRule &rule = grammar.basicRules()[rule_index];
      for (const Edge &edge : graph.edgesForLabel(rule.label)) {
        addFact({rule.head, {{edge.source, edge.target}}},
                {ProofKind::Basic, rule_index, {}, edge});
      }
    }
  }

  void processPrepend(std::size_t rule_index, std::size_t premise_id) {
    const Grammar::PrependRule &rule = grammar.prependRules()[rule_index];
    const Fact premise = facts[premise_id];
    const Span span = premise.spans[rule.component];
    for (const Edge &edge : graph.incoming(span.source, rule.label)) {
      Fact result{rule.head, premise.spans};
      result.spans[rule.component].source = edge.source;
      addFact(std::move(result),
              {ProofKind::Prepend, rule_index, {premise_id}, edge});
    }
  }

  void processEpsilon(std::size_t premise_id) {
    const Fact premise = facts[premise_id];
    for (std::size_t component = 0; component < premise.spans.size();
         ++component) {
      const Span span = premise.spans[component];
      for (const Edge &edge : graph.incoming(span.source, kEpsilonLabel)) {
        Fact result{premise.nonterminal, premise.spans};
        result.spans[component].source = edge.source;
        addFact(std::move(result),
                {ProofKind::EpsilonPrepend, component, {premise_id}, edge});
      }
      for (const Edge &edge : graph.outgoing(span.target, kEpsilonLabel)) {
        Fact result{premise.nonterminal, premise.spans};
        result.spans[component].target = edge.target;
        addFact(std::move(result),
                {ProofKind::EpsilonAppend, component, {premise_id}, edge});
      }
    }
  }

  void processAppend(std::size_t rule_index, std::size_t premise_id) {
    const Grammar::AppendRule &rule = grammar.appendRules()[rule_index];
    const Fact premise = facts[premise_id];
    const Span span = premise.spans[rule.component];
    for (const Edge &edge : graph.outgoing(span.target, rule.label)) {
      Fact result{rule.head, premise.spans};
      result.spans[rule.component].target = edge.target;
      addFact(std::move(result),
              {ProofKind::Append, rule_index, {premise_id}, edge});
    }
  }

  void processInsert(std::size_t rule_index, std::size_t premise_id) {
    const Grammar::InsertRule &rule = grammar.insertRules()[rule_index];
    const Fact premise = facts[premise_id];
    for (const Edge &edge : graph.edgesForLabel(rule.label)) {
      Fact result;
      result.nonterminal = rule.head;
      result.spans.reserve(premise.spans.size() + 1);
      result.spans.insert(result.spans.end(), premise.spans.begin(),
                          premise.spans.begin() + rule.component);
      result.spans.push_back({edge.source, edge.target});
      result.spans.insert(result.spans.end(),
                          premise.spans.begin() + rule.component,
                          premise.spans.end());
      addFact(std::move(result),
              {ProofKind::Insert, rule_index, {premise_id}, edge});
    }
  }

  bool assignedAdjacenciesMatch(
      const Grammar::ConcatenateRule &rule,
      const std::vector<std::optional<std::size_t>> &assignment) const {
    for (const std::vector<Grammar::VariableRef> &output : rule.outputs) {
      for (std::size_t i = 1; i < output.size(); ++i) {
        const Grammar::VariableRef &left = output[i - 1];
        const Grammar::VariableRef &right = output[i];
        if (!assignment[left.body] || !assignment[right.body]) {
          continue;
        }
        const Span &left_span =
            facts[*assignment[left.body]].spans[left.component];
        const Span &right_span =
            facts[*assignment[right.body]].spans[right.component];
        if (left_span.target != right_span.source) {
          return false;
        }
      }
    }
    return true;
  }

  std::vector<EndpointKey> constraintsFor(
      const Grammar::ConcatenateRule &rule, std::size_t body,
      const std::vector<std::optional<std::size_t>> &assignment) const {
    std::vector<EndpointKey> constraints;
    for (const std::vector<Grammar::VariableRef> &output : rule.outputs) {
      for (std::size_t i = 0; i < output.size(); ++i) {
        if (output[i].body != body) {
          continue;
        }
        if (i > 0 && assignment[output[i - 1].body]) {
          const Grammar::VariableRef &left = output[i - 1];
          const Span &span =
              facts[*assignment[left.body]].spans[left.component];
          constraints.push_back(
              {rule.bodies[body], output[i].component, true, span.target});
        }
        if (i + 1 < output.size() && assignment[output[i + 1].body]) {
          const Grammar::VariableRef &right = output[i + 1];
          const Span &span =
              facts[*assignment[right.body]].spans[right.component];
          constraints.push_back(
              {rule.bodies[body], output[i].component, false, span.source});
        }
      }
    }
    return constraints;
  }

  bool satisfies(const Fact &fact,
                 const std::vector<EndpointKey> &constraints) const {
    for (const EndpointKey &constraint : constraints) {
      const Span &span = fact.spans[constraint.component];
      if ((constraint.start ? span.source : span.target) != constraint.vertex) {
        return false;
      }
    }
    return true;
  }

  void
  addType5Result(std::size_t rule_index,
                 const std::vector<std::optional<std::size_t>> &assignment) {
    const Grammar::ConcatenateRule &rule =
        grammar.concatenateRules()[rule_index];
    ++stats.type5_combinations;
    std::vector<Span> spans(rule.outputs.size());
    std::vector<std::size_t> empty_outputs;
    for (std::size_t i = 0; i < rule.outputs.size(); ++i) {
      const std::vector<Grammar::VariableRef> &output = rule.outputs[i];
      if (output.empty()) {
        empty_outputs.push_back(i);
        continue;
      }
      const Grammar::VariableRef &first = output.front();
      const Grammar::VariableRef &last = output.back();
      spans[i] = {facts[*assignment[first.body]].spans[first.component].source,
                  facts[*assignment[last.body]].spans[last.component].target};
    }

    std::vector<std::size_t> premises;
    premises.reserve(assignment.size());
    for (const std::optional<std::size_t> &fact : assignment) {
      premises.push_back(*fact);
    }

    const auto emit = [&]() {
      addFact({rule.head, spans},
              {ProofKind::Concatenate, rule_index, premises, {}});
    };
    if (empty_outputs.empty()) {
      emit();
      return;
    }

    const auto enumerate_empty = [&](const auto &self,
                                     std::size_t index) -> void {
      if (index == empty_outputs.size()) {
        emit();
        return;
      }
      for (Vertex vertex : graph.vertices()) {
        spans[empty_outputs[index]] = {vertex, vertex};
        self(self, index + 1);
      }
    };
    enumerate_empty(enumerate_empty, 0);
  }

  void processConcatenate(std::size_t rule_index, std::size_t anchor_body,
                          std::size_t anchor_id) {
    const Grammar::ConcatenateRule &rule =
        grammar.concatenateRules()[rule_index];
    std::vector<std::optional<std::size_t>> assignment(rule.bodies.size());
    assignment[anchor_body] = anchor_id;
    if (!assignedAdjacenciesMatch(rule, assignment)) {
      return;
    }

    const auto enumerate = [&](const auto &self) -> void {
      std::size_t body = rule.bodies.size();
      std::vector<EndpointKey> best_constraints;
      const std::vector<std::size_t> *best_candidates = nullptr;
      for (std::size_t candidate_body = 0; candidate_body < rule.bodies.size();
           ++candidate_body) {
        if (assignment[candidate_body]) {
          continue;
        }
        std::vector<EndpointKey> constraints =
            constraintsFor(rule, candidate_body, assignment);
        const std::vector<std::size_t> *candidates =
            &facts_by_nonterminal[rule.bodies[candidate_body]];
        for (const EndpointKey &constraint : constraints) {
          const std::vector<std::size_t> &indexed = indexedFacts(constraint);
          if (indexed.size() < candidates->size()) {
            candidates = &indexed;
          }
        }
        if (best_candidates == nullptr ||
            candidates->size() < best_candidates->size()) {
          body = candidate_body;
          best_constraints = std::move(constraints);
          best_candidates = candidates;
        }
      }

      if (body == rule.bodies.size()) {
        addType5Result(rule_index, assignment);
        return;
      }
      // Adding a result can grow the same nonterminal or endpoint index that
      // supplies candidates for an outer recursive frame. Iterate over the
      // facts visible at this saturation step, not a reallocating live vector.
      const std::vector<std::size_t> candidates = *best_candidates;
      for (std::size_t candidate : candidates) {
        if (!satisfies(facts[candidate], best_constraints)) {
          continue;
        }
        assignment[body] = candidate;
        if (assignedAdjacenciesMatch(rule, assignment)) {
          self(self);
        }
        assignment[body].reset();
      }
    };
    enumerate(enumerate);
  }

  void run() {
    initialize();
    std::size_t next = 0;
    while (next < worklist.size()) {
      const std::size_t fact_id = worklist[next++];
      ++stats.worklist_pops;
      const Grammar::Nonterminal nonterminal = facts[fact_id].nonterminal;
      processEpsilon(fact_id);
      const std::vector<Trigger> current_triggers = triggers[nonterminal];
      for (const Trigger &trigger : current_triggers) {
        switch (trigger.kind) {
        case TriggerKind::Prepend:
          processPrepend(trigger.rule, fact_id);
          break;
        case TriggerKind::Append:
          processAppend(trigger.rule, fact_id);
          break;
        case TriggerKind::Insert:
          processInsert(trigger.rule, fact_id);
          break;
        case TriggerKind::Concatenate:
          processConcatenate(trigger.rule, trigger.body, fact_id);
          break;
        }
      }
    }
    stats.facts = facts.size();
  }

  Graph graph;
  const Grammar &grammar;
  const SolverOptions &options;
  PlainReachability plain_reachability;
  std::vector<Fact> facts;
  std::vector<Proof> proofs;
  std::vector<std::size_t> worklist;
  std::unordered_map<Fact, std::size_t, FactHash> fact_ids;
  std::vector<std::vector<std::size_t>> facts_by_nonterminal;
  std::unordered_map<EndpointKey, std::vector<std::size_t>, EndpointKeyHash>
      endpoint_index;
  std::unordered_map<Pair, std::size_t, PairHash> start_fact;
  std::vector<std::vector<Trigger>> triggers;
  SolverStats stats;
};

} // namespace

namespace detail {

struct WitnessData {
  Grammar grammar;
  std::vector<Fact> facts;
  std::vector<Proof> proofs;
  std::unordered_map<Pair, std::size_t, PairHash> start_fact;

  void appendEdge(const Edge &edge, std::vector<Edge> &output) const {
    if (edge.label.empty() && edge.source == edge.target) {
      return;
    }
    output.push_back(edge);
  }

  void build(std::size_t fact_id, std::size_t component,
             std::vector<Edge> &output) const {
    const Proof &proof = proofs.at(fact_id);
    switch (proof.kind) {
    case ProofKind::Basic:
      appendEdge(proof.edge, output);
      return;
    case ProofKind::EpsilonPrepend:
      if (component == proof.rule) {
        appendEdge(proof.edge, output);
      }
      build(proof.premises.at(0), component, output);
      return;
    case ProofKind::EpsilonAppend:
      build(proof.premises.at(0), component, output);
      if (component == proof.rule) {
        appendEdge(proof.edge, output);
      }
      return;
    case ProofKind::Prepend: {
      const Grammar::PrependRule &rule = grammar.prependRules().at(proof.rule);
      if (component == rule.component) {
        appendEdge(proof.edge, output);
      }
      build(proof.premises.at(0), component, output);
      return;
    }
    case ProofKind::Append: {
      const Grammar::AppendRule &rule = grammar.appendRules().at(proof.rule);
      build(proof.premises.at(0), component, output);
      if (component == rule.component) {
        appendEdge(proof.edge, output);
      }
      return;
    }
    case ProofKind::Insert: {
      const Grammar::InsertRule &rule = grammar.insertRules().at(proof.rule);
      if (component == rule.component) {
        appendEdge(proof.edge, output);
      } else {
        const std::size_t body_component =
            component < rule.component ? component : component - 1;
        build(proof.premises.at(0), body_component, output);
      }
      return;
    }
    case ProofKind::Concatenate: {
      const Grammar::ConcatenateRule &rule =
          grammar.concatenateRules().at(proof.rule);
      for (const Grammar::VariableRef &ref : rule.outputs.at(component)) {
        build(proof.premises.at(ref.body), ref.component, output);
      }
      return;
    }
    }
  }
};

} // namespace detail

bool Span::operator==(const Span &other) const {
  return source == other.source && target == other.target;
}

bool Fact::operator==(const Fact &other) const {
  return nonterminal == other.nonterminal && spans == other.spans;
}

bool ReachabilityResult::reaches(Vertex source, Vertex target) const {
  return reachable_pairs_.count({source, target}) != 0U;
}

const std::vector<Fact> &ReachabilityResult::facts() const {
  static const std::vector<Fact> empty;
  return witness_data_ ? witness_data_->facts : empty;
}

std::optional<std::vector<Edge>>
ReachabilityResult::witness(Vertex source, Vertex target) const {
  if (!witness_data_) {
    return std::nullopt;
  }
  const auto found = witness_data_->start_fact.find({source, target});
  if (found == witness_data_->start_fact.end()) {
    return std::nullopt;
  }
  std::vector<Edge> result;
  witness_data_->build(found->second, 0, result);
  return result;
}

ReachabilityResult Solver::solve(const Graph &graph, const Grammar &grammar,
                                 const SolverOptions &options) const {
  grammar.validate();
  State state(graph, grammar, options);
  state.run();

  ReachabilityResult result;
  result.stats_ = state.stats;
  for (const auto &entry : state.start_fact) {
    result.reachable_pairs_.insert(entry.first);
  }
  auto witness_data = std::make_shared<detail::WitnessData>();
  witness_data->grammar = grammar;
  witness_data->facts = std::move(state.facts);
  witness_data->proofs = std::move(state.proofs);
  witness_data->start_fact = std::move(state.start_fact);
  result.witness_data_ = std::move(witness_data);
  return result;
}

} // namespace lotus::cfl::mcfl
