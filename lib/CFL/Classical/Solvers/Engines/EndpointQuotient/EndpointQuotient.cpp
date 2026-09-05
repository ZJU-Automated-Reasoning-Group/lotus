#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotient.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lotus {
namespace cfl {
namespace endpoint {
namespace {

using Clock = std::chrono::steady_clock;
using Lists = std::vector<std::vector<Id>>;

static double milliseconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

static Count product(Id a, Id b) {
  static_assert(sizeof(Id) <= sizeof(Count), "IDs must fit in counters");
  if (b && a > std::numeric_limits<Count>::max() / b)
    throw std::overflow_error("endpoint quotient fact count overflow");
  return static_cast<Count>(a) * static_cast<Count>(b);
}

static void addCount(Count &a, Count b) {
  if (a > std::numeric_limits<Count>::max() - b)
    throw std::overflow_error("endpoint quotient fact count overflow");
  a += b;
}

template <class T> static void sortUnique(std::vector<T> &values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

struct VectorHash {
  std::size_t operator()(const std::vector<Id> &v) const noexcept {
    std::size_t h = v.size();
    for (Id x : v)
      h ^= std::hash<Id>{}(x) + std::size_t(0x9e3779b9U) + (h << 6) + (h >> 2);
    return h;
  }
};

static std::vector<Id> intern(const Lists &signatures) {
  std::unordered_map<std::vector<Id>, Id, VectorHash> table;
  table.reserve(signatures.size());
  std::vector<Id> result;
  result.reserve(signatures.size());
  for (const auto &signature : signatures) {
    auto found = table.emplace(signature, table.size());
    result.push_back(found.first->second);
  }
  return result;
}

struct Partition {
  std::vector<Id> class_of;
  Lists members;

  static Partition fromSignatures(const Lists &signatures) {
    Partition p;
    p.class_of = intern(signatures);
    for (Id v = 0; v < p.class_of.size(); ++v) {
      Id c = p.class_of[v];
      if (c == p.members.size())
        p.members.emplace_back();
      p.members[c].push_back(v);
    }
    return p;
  }

  static Partition singleton(Id n) {
    Partition p;
    p.class_of.resize(n);
    std::iota(p.class_of.begin(), p.class_of.end(), Id(0));
    p.members.resize(n);
    for (Id v = 0; v < n; ++v)
      p.members[v].push_back(v);
    return p;
  }
};

// Each fine class belongs to exactly one coarse class. The grammar dependency
// closure guarantees this refinement; checking all members in debug builds
// catches mistakes that representative-only quotient constructions can hide.
static Lists lift(const Partition &fine, const Partition &coarse) {
  Lists result(coarse.members.size());
  for (Id f = 0; f < fine.members.size(); ++f) {
    Id c = coarse.class_of[fine.members[f].front()];
#ifndef NDEBUG
    for (Id v : fine.members[f])
      assert(coarse.class_of[v] == c);
#endif
    result[c].push_back(f);
  }
  return result;
}

static Lists dependencyClosure(const Lists &dependencies) {
  Lists closure(dependencies.size());
  for (Id a = 0; a < dependencies.size(); ++a) {
    std::vector<bool> seen(dependencies.size(), false);
    std::vector<Id> stack{a};
    seen[a] = true;
    while (!stack.empty()) {
      Id b = stack.back();
      stack.pop_back();
      for (Id c : dependencies[b]) {
        if (!seen[c]) {
          seen[c] = true;
          stack.push_back(c);
        }
      }
    }
    for (Id b = 0; b < seen.size(); ++b)
      if (seen[b])
        closure[a].push_back(b);
  }
  return closure;
}

struct Relation {
  std::vector<std::unordered_set<Id>> known_out;
  Lists active_out;
  Lists active_in;
};

struct UnaryPlan {
  Id lhs;
  Id child;
  Lists rows;
  Lists columns;
};

struct BinaryPlan {
  Id lhs;
  Id left;
  Id right;
  Lists rows;
  Lists columns;
  // An interface edge (j,k) means Q_left[j] intersects P_right[k].
  // Numeric class ID equality has NO semantic meaning across partitions.
  Lists to_right;
  Lists to_left;
};

struct Fact {
  Id symbol;
  Id row;
  Id column;
};

} // namespace

void Problem::validate() const {
  for (const Edge &e : edges)
    if (e.source >= nodes || e.target >= nodes || e.symbol >= symbols)
      throw std::invalid_argument("endpoint quotient edge ID out of range");
  for (const Rule &r : rules) {
    if (r.lhs >= symbols)
      throw std::invalid_argument("endpoint quotient rule LHS out of range");
    switch (r.kind) {
    case Rule::Kind::Epsilon:
      break;
    case Rule::Kind::Unary:
      if (r.left >= symbols)
        throw std::invalid_argument("endpoint quotient unary RHS out of range");
      break;
    case Rule::Kind::Binary:
      if (r.left >= symbols || r.right >= symbols)
        throw std::invalid_argument("endpoint quotient binary RHS out of range");
      break;
    default:
      throw std::invalid_argument("endpoint quotient invalid rule kind");
    }
  }
}

struct Solver::Impl {
  Problem problem;
  Options options;
  bool solved = false;
  bool started = false;
  Statistics stats;
  std::vector<bool> nullable;
  std::vector<Partition> sources;
  std::vector<Partition> targets;
  std::vector<Relation> relations;
  std::vector<UnaryPlan> units;
  std::vector<BinaryPlan> binaries;
  Lists unit_uses;
  Lists left_uses;
  Lists right_uses;
  std::deque<Fact> worklist;

  Impl(Problem p, Options o) : problem(std::move(p)), options(o) {
    problem.validate();
    switch (options.partitions) {
    case PartitionMode::Grammar:
    case PartitionMode::Global:
    case PartitionMode::Singleton:
      break;
    default:
      throw std::invalid_argument("endpoint quotient invalid partition mode");
    }
  }

  void requireSolved() const {
    if (!solved)
      throw std::logic_error("endpoint quotient queried before solve completed");
  }

  void requireSymbol(Id a) const {
    if (a >= problem.symbols)
      throw std::out_of_range("endpoint quotient query symbol out of range");
  }

  void computeNullable() {
    nullable.assign(problem.symbols, false);
    bool changed = true;
    while (changed) {
      changed = false;
      for (const Rule &r : problem.rules) {
        bool value = r.kind == Rule::Kind::Epsilon;
        if (r.kind == Rule::Kind::Unary)
          value = nullable[r.left];
        if (r.kind == Rule::Kind::Binary)
          value = nullable[r.left] && nullable[r.right];
        if (value && !nullable[r.lhs]) {
          nullable[r.lhs] = true;
          changed = true;
        }
      }
    }
    for (bool value : nullable)
      stats.nullable_symbols += value ? 1 : 0;
  }

  void buildPartitions(const Lists &first, const Lists &last) {
    Id k = problem.symbols;
    Id n = problem.nodes;
    sources.resize(k);
    targets.resize(k);
    if (options.partitions == PartitionMode::Singleton) {
      for (Id a = 0; a < k; ++a) {
        sources[a] = Partition::singleton(n);
        targets[a] = Partition::singleton(n);
      }
      return;
    }

    std::vector<std::vector<Edge>> seeds(k);
    for (const Edge &e : problem.edges)
      seeds[e.symbol].push_back(e);
    Lists atomic_out(k), atomic_in(k);
    for (Id a = 0; a < k; ++a) {
      Lists out(n), in(n);
      for (const Edge &e : seeds[a]) {
        out[e.source].push_back(e.target);
        in[e.target].push_back(e.source);
      }
      for (auto &row : out)
        sortUnique(row);
      for (auto &column : in)
        sortUnique(column);
      atomic_out[a] = intern(out);
      atomic_in[a] = intern(in);
    }

    for (Id a = 0; a < k; ++a) {
      Lists row_signatures(n), column_signatures(n);
      for (Id v = 0; v < n; ++v) {
        if (options.partitions == PartitionMode::Global) {
          for (Id b = 0; b < k; ++b) {
            row_signatures[v].push_back(atomic_out[b][v]);
            column_signatures[v].push_back(atomic_in[b][v]);
          }
        } else {
          for (Id b : first[a])
            row_signatures[v].push_back(atomic_out[b][v]);
          for (Id b : last[a])
            column_signatures[v].push_back(atomic_in[b][v]);
        }
      }
      sources[a] = Partition::fromSignatures(row_signatures);
      targets[a] = Partition::fromSignatures(column_signatures);
    }
  }

  void prepare() {
    computeNullable();
    std::vector<std::pair<Id, Id>> unary_rules;
    std::vector<std::tuple<Id, Id, Id>> binary_rules;
    for (const Rule &r : problem.rules) {
      if (r.kind == Rule::Kind::Unary)
        unary_rules.emplace_back(r.lhs, r.left);
      if (r.kind == Rule::Kind::Binary) {
        binary_rules.emplace_back(r.lhs, r.left, r.right);
        // R_A = nullable(A)*I union R_A^+. Identity must never be represented
        // as a complete block: a multi-vertex diagonal is not a rectangle.
        if (nullable[r.right])
          unary_rules.emplace_back(r.lhs, r.left);
        if (nullable[r.left])
          unary_rules.emplace_back(r.lhs, r.right);
      }
    }
    sortUnique(unary_rules);
    sortUnique(binary_rules);
    Lists left_dependencies(problem.symbols), right_dependencies(problem.symbols);
    for (auto r : unary_rules) {
      left_dependencies[r.first].push_back(r.second);
      right_dependencies[r.first].push_back(r.second);
    }
    for (auto r : binary_rules) {
      left_dependencies[std::get<0>(r)].push_back(std::get<1>(r));
      right_dependencies[std::get<0>(r)].push_back(std::get<2>(r));
    }
    buildPartitions(dependencyClosure(left_dependencies),
                    dependencyClosure(right_dependencies));

    unit_uses.resize(problem.symbols);
    left_uses.resize(problem.symbols);
    right_uses.resize(problem.symbols);
    relations.resize(problem.symbols);
    for (Id a = 0; a < problem.symbols; ++a) {
      relations[a].known_out.resize(sources[a].members.size());
      relations[a].active_out.resize(sources[a].members.size());
      relations[a].active_in.resize(targets[a].members.size());
    }
    for (auto r : unary_rules) {
      Id a = r.first, b = r.second;
      unit_uses[b].push_back(units.size());
      units.push_back({a, b, lift(sources[a], sources[b]),
                       lift(targets[a], targets[b])});
    }
    for (auto r : binary_rules) {
      Id a = std::get<0>(r), b = std::get<1>(r), c = std::get<2>(r);
      BinaryPlan plan{a, b, c, lift(sources[a], sources[b]),
                      lift(targets[a], targets[c]),
                      Lists(targets[b].members.size()),
                      Lists(sources[c].members.size())};
      std::vector<std::pair<Id, Id>> bridges;
      bridges.reserve(problem.nodes);
      for (Id v = 0; v < problem.nodes; ++v)
        bridges.emplace_back(targets[b].class_of[v], sources[c].class_of[v]);
      sortUnique(bridges);
      addCount(stats.bridge_pairs, bridges.size());
      for (auto bridge : bridges) {
        plan.to_right[bridge.first].push_back(bridge.second);
        plan.to_left[bridge.second].push_back(bridge.first);
      }
      left_uses[b].push_back(binaries.size());
      right_uses[c].push_back(binaries.size());
      binaries.push_back(std::move(plan));
    }
  }

  bool publish(Id a, Id row, Id column) {
    ++stats.insert_attempts;
    if (!relations[a].known_out[row].insert(column).second) {
      ++stats.duplicate_inserts;
      return false;
    }
    worklist.push_back({a, row, column});
    ++stats.cells;
    ++stats.worklist_pushes;
    stats.peak_worklist = std::max(stats.peak_worklist,
                                   static_cast<Count>(worklist.size()));
    return true;
  }

  void binaryJoin(const BinaryPlan &plan, Id row, Id column) {
    ++stats.binary_joins;
    for (Id i : plan.rows[row])
      for (Id j : plan.columns[column]) {
        ++stats.binary_propagations;
        if (publish(plan.lhs, i, j))
          ++stats.successful_binary_propagations;
      }
  }

  void saturate() {
    stats.input_edges = problem.edges.size();
    for (const Edge &e : problem.edges) {
      Id i = sources[e.symbol].class_of[e.source];
      Id j = targets[e.symbol].class_of[e.target];
      if (publish(e.symbol, i, j)) {
        ++stats.seed_cells;
        addCount(stats.seed_facts, product(sources[e.symbol].members[i].size(),
                                           targets[e.symbol].members[j].size()));
      }
    }
    while (!worklist.empty()) {
      // Copy before publishing: queue growth must not invalidate this event.
      Fact f = worklist.front();
      worklist.pop_front();
      ++stats.worklist_pops;
      for (Id id : unit_uses[f.symbol]) {
        const UnaryPlan &plan = units[id];
        for (Id i : plan.rows[f.row])
          for (Id j : plan.columns[f.column]) {
            ++stats.unary_propagations;
            if (publish(plan.lhs, i, j))
              ++stats.successful_unary_propagations;
          }
      }
      // Only popped events enter active adjacency. Thus each distinct pair
      // joins at the later activation, independent of enqueue/derivation order.
      // publish() changes known_out and the queue, never active adjacency.
      for (Id id : left_uses[f.symbol]) {
        const BinaryPlan &plan = binaries[id];
        for (Id middle : plan.to_right[f.column])
          for (Id column : relations[plan.right].active_out[middle])
            binaryJoin(plan, f.row, column);
        // A cell used twice is absent from both active scans. Handle this
        // diagonal of the *event pair space* exactly once, not once per node.
        if (plan.right == f.symbol &&
            std::binary_search(plan.to_right[f.column].begin(),
                               plan.to_right[f.column].end(), f.row))
          binaryJoin(plan, f.row, f.column);
      }
      for (Id id : right_uses[f.symbol]) {
        const BinaryPlan &plan = binaries[id];
        for (Id middle : plan.to_left[f.row])
          for (Id row : relations[plan.left].active_in[middle])
            binaryJoin(plan, row, f.column);
      }
      relations[f.symbol].active_out[f.row].push_back(f.column);
      relations[f.symbol].active_in[f.column].push_back(f.row);
    }
  }

  void count() {
    stats.per_symbol.resize(problem.symbols);
    for (Id a = 0; a < problem.symbols; ++a) {
      SymbolStatistics &s = stats.per_symbol[a];
      s.source_classes = sources[a].members.size();
      s.target_classes = targets[a].members.size();
      for (Id i = 0; i < relations[a].known_out.size(); ++i) {
        addCount(s.positive_cells, relations[a].known_out[i].size());
        for (Id j : relations[a].known_out[i])
          addCount(s.positive_facts, product(sources[a].members[i].size(),
                                             targets[a].members[j].size()));
      }
      addCount(stats.logical_facts, s.positive_facts);
      if (nullable[a])
        for (Id v = 0; v < problem.nodes; ++v)
          if (!positiveContains(a, v, v))
            addCount(stats.logical_facts, 1);
    }
    stats.inferred_facts = stats.logical_facts - stats.seed_facts;
  }

  bool positiveContains(Id a, Id u, Id v) const {
    const auto &row = relations[a].known_out[sources[a].class_of[u]];
    return row.find(targets[a].class_of[v]) != row.end();
  }

  void solve() {
    if (solved)
      return;
    if (started)
      throw std::logic_error("cannot retry an interrupted endpoint quotient solve");
    started = true;
    auto begin = Clock::now();
    prepare();
    auto prepared = Clock::now();
    saturate();
    auto saturated = Clock::now();
    count();
    auto counted = Clock::now();
    stats.preprocess_ms = milliseconds(begin, prepared);
    stats.saturation_ms = milliseconds(prepared, saturated);
    stats.count_ms = milliseconds(saturated, counted);
    solved = true;
  }
};

Solver::Solver(Problem problem, Options options)
    : impl_(new Impl(std::move(problem), options)) {}
Solver::~Solver() = default;
Solver::Solver(Solver &&) noexcept = default;
Solver &Solver::operator=(Solver &&) noexcept = default;

void Solver::solve() { impl_->solve(); }

bool Solver::contains(Id symbol, Id source, Id target) const {
  impl_->requireSolved();
  impl_->requireSymbol(symbol);
  if (source >= impl_->problem.nodes || target >= impl_->problem.nodes)
    throw std::out_of_range("endpoint quotient query node out of range");
  return (source == target && impl_->nullable[symbol]) ||
         impl_->positiveContains(symbol, source, target);
}

bool Solver::isNullable(Id symbol) const {
  impl_->requireSolved();
  impl_->requireSymbol(symbol);
  return impl_->nullable[symbol];
}

const Statistics &Solver::statistics() const {
  impl_->requireSolved();
  return impl_->stats;
}

void Solver::forEachPositiveRectangle(const RectangleVisitor &visitor) const {
  impl_->requireSolved();
  for (Id a = 0; a < impl_->problem.symbols; ++a)
    for (Id i = 0; i < impl_->relations[a].known_out.size(); ++i)
      for (Id j : impl_->relations[a].known_out[i])
        visitor(a, impl_->sources[a].members[i], impl_->targets[a].members[j]);
}

void Solver::forEachFact(const FactVisitor &visitor) const {
  forEachPositiveRectangle([&](Id a, const std::vector<Id> &rows,
                              const std::vector<Id> &columns) {
    for (Id u : rows)
      for (Id v : columns)
        visitor(a, u, v);
  });
  for (Id a = 0; a < impl_->problem.symbols; ++a)
    if (impl_->nullable[a])
      for (Id v = 0; v < impl_->problem.nodes; ++v)
        if (!impl_->positiveContains(a, v, v))
          visitor(a, v, v);
}

} // namespace endpoint
} // namespace cfl
} // namespace lotus
