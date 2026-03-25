/*
 * Author: rainoftime
 */
#include "Dataflow/WPDS/Core/GenKillTransformer.h"

namespace wpds {

static std::set<Value *> collectRelevantValues(
    const GenKillTransformer &lhs, const GenKillTransformer &rhs) {
  std::set<Value *> values;

  auto collectFacts = [&values](const DataFlowFacts &facts) {
    values.insert(facts.getFacts().begin(), facts.getFacts().end());
  };
  auto collectFlow = [&values, &collectFacts](
                         const std::map<Value *, DataFlowFacts> &flow) {
    for (const auto &entry : flow) {
      values.insert(entry.first);
      collectFacts(entry.second);
    }
  };

  collectFacts(lhs.getKill());
  collectFacts(lhs.getGen());
  collectFacts(rhs.getKill());
  collectFacts(rhs.getGen());
  collectFlow(lhs.getFlow());
  collectFlow(rhs.getFlow());
  return values;
}

GenKillTransformer::GenKillTransformer()
    : count(0), kill(DataFlowFacts::EmptySet()),
      gen(DataFlowFacts::EmptySet()) {}

GenKillTransformer::GenKillTransformer(const DataFlowFacts &kill,
                                       const DataFlowFacts &gen)
    : count(0), kill(DataFlowFacts::Diff(kill, gen)), gen(gen) {}

GenKillTransformer::GenKillTransformer(
    const DataFlowFacts &kill, const DataFlowFacts &gen,
    const std::map<Value *, DataFlowFacts> &flow)
    : count(0), kill(DataFlowFacts::Diff(kill, gen)), gen(gen), flow(flow) {}

GenKillTransformer::GenKillTransformer(
    const DataFlowFacts &k, const DataFlowFacts &g,
    const std::map<Value *, DataFlowFacts> &f, int)
    : count(1), kill(k), gen(g), flow(f) {}

GenKillTransformer *
GenKillTransformer::makeGenKillTransformer(const DataFlowFacts &kill,
                                           const DataFlowFacts &gen) {
  return makeGenKillTransformer(kill, gen, {});
}

GenKillTransformer *GenKillTransformer::makeGenKillTransformer(
    const DataFlowFacts &kill, const DataFlowFacts &gen,
    const std::map<Value *, DataFlowFacts> &flow) {

  DataFlowFacts k_normalized = DataFlowFacts::Diff(kill, gen);

  // Check if flow is empty
  bool flowEmpty = true;
  for (const auto &pair : flow) {
    if (!pair.second.isEmpty()) {
      flowEmpty = false;
      break;
    }
  }

  if (DataFlowFacts::Eq(k_normalized, DataFlowFacts::EmptySet()) &&
      DataFlowFacts::Eq(gen, DataFlowFacts::UniverseSet()) && flowEmpty) {
    return GenKillTransformer::bottom();
  } else if (DataFlowFacts::Eq(k_normalized, DataFlowFacts::EmptySet()) &&
             DataFlowFacts::Eq(gen, DataFlowFacts::EmptySet()) && flowEmpty) {
    return GenKillTransformer::one();
  } else {
    return new GenKillTransformer(k_normalized, gen, flow);
  }
}

GenKillTransformer *GenKillTransformer::one() {
  static GenKillTransformer *ONE = new GenKillTransformer(
      DataFlowFacts::EmptySet(), DataFlowFacts::EmptySet(), {}, 1);
  return ONE;
}

GenKillTransformer *GenKillTransformer::zero() {
  // Join identity and extend annihilator for may transformers. Applying zero
  // to any fact set yields the empty set.
  static GenKillTransformer *ZERO = new GenKillTransformer(
      DataFlowFacts::UniverseSet(), DataFlowFacts::EmptySet(), {}, 1);
  return ZERO;
}

GenKillTransformer *GenKillTransformer::bottom() {
  static GenKillTransformer *BOTTOM = new GenKillTransformer(
      DataFlowFacts::EmptySet(), DataFlowFacts::UniverseSet(), {}, 1);
  return BOTTOM;
}

GenKillTransformer *GenKillTransformer::extend(GenKillTransformer *y) {
  // Composition in program order: this ; y, i.e. result(S) = y(this(S)).
  if (equal(GenKillTransformer::zero()) ||
      y->equal(GenKillTransformer::zero())) {
    return GenKillTransformer::zero();
  }

  if (equal(GenKillTransformer::one())) {
    return y;
  }

  if (y->equal(GenKillTransformer::one())) {
    return this;
  }

  DataFlowFacts composedGen = y->apply(apply(DataFlowFacts::EmptySet()));
  DataFlowFacts composedKill = DataFlowFacts::Union(kill, y->kill);
  std::map<Value *, DataFlowFacts> composedFlow;

  for (Value *value : collectRelevantValues(*this, *y)) {
    DataFlowFacts input;
    input.addFact(value);
    DataFlowFacts output = y->apply(apply(input));

    DataFlowFacts withoutGen = DataFlowFacts::Diff(output, composedGen);
    if (!composedKill.containsFact(value)) {
      withoutGen.removeFact(value);
    }

    if (!withoutGen.isEmpty()) {
      composedFlow[value] = withoutGen;
    }
  }

  return makeGenKillTransformer(composedKill, composedGen, composedFlow);
}

GenKillTransformer *GenKillTransformer::combine(GenKillTransformer *y) {
  // Conservative may-join over alternative paths.
  if (equal(GenKillTransformer::zero())) {
    return y;
  }

  if (y->equal(GenKillTransformer::zero())) {
    return this;
  }

  DataFlowFacts temp_k = DataFlowFacts::Intersect(kill, y->kill);
  DataFlowFacts temp_g = DataFlowFacts::Union(gen, y->gen);
  std::map<Value *, DataFlowFacts> temp_flow;

  for (Value *x : collectRelevantValues(*this, *y)) {
    DataFlowFacts flowOut = DataFlowFacts::EmptySet();
    if (flow.count(x)) {
      flowOut = DataFlowFacts::Union(flowOut, flow.at(x));
    }
    if (y->flow.count(x)) {
      flowOut = DataFlowFacts::Union(flowOut, y->flow.at(x));
    }
    flowOut = DataFlowFacts::Diff(flowOut, temp_g);
    if (!temp_k.containsFact(x)) {
      flowOut.removeFact(x);
    }
    if (!flowOut.isEmpty()) {
      temp_flow[x] = flowOut;
    }
  }

  return makeGenKillTransformer(temp_k, temp_g, temp_flow);
}

GenKillTransformer *GenKillTransformer::diff(GenKillTransformer *y) {
  // The non-differential build does not rely on precise deltas. Keep diff
  // coherent: zero iff unchanged, otherwise return the new weight itself.
  if (equal(y)) {
    return GenKillTransformer::zero();
  }
  return this;
}

GenKillTransformer *GenKillTransformer::quasiOne() const { return one(); }

bool GenKillTransformer::equal(GenKillTransformer *y) const {
  // Handle special values
  if (this == one() && y == one())
    return true;
  if (this == zero() && y == zero())
    return true;
  if (this == bottom() && y == bottom())
    return true;

  if ((this == one() && y != one()) || (this == zero() && y != zero()) ||
      (this == bottom() && y != bottom())) {
    return false;
  }

  // Compare gen and kill sets
  if (!DataFlowFacts::Eq(kill, y->kill))
    return false;
  if (!DataFlowFacts::Eq(gen, y->gen))
    return false;

  // Compare maps
  if (flow.size() != y->flow.size())
    return false;
  for (auto &kv : flow) {
    if (!y->flow.count(kv.first))
      return false;
    if (!DataFlowFacts::Eq(kv.second, y->flow.at(kv.first)))
      return false;
  }

  return true;
}

DataFlowFacts GenKillTransformer::apply(const DataFlowFacts &input) {
  // f(S) = (S \ Kill) U (Union_{x in S \ Kill} Flow(x)) U Gen

  // 1. S \ Kill
  DataFlowFacts survivors = DataFlowFacts::Diff(input, kill);

  // 2. Flow from survivors
  DataFlowFacts flow_out;
  for (Value *v : survivors.getFacts()) {
    if (flow.count(v)) {
      flow_out = DataFlowFacts::Union(flow_out, flow.at(v));
    }
  }

  // 3. Union everything
  DataFlowFacts result = DataFlowFacts::Union(survivors, flow_out);
  return DataFlowFacts::Union(result, gen);
}

const DataFlowFacts &GenKillTransformer::getKill() const { return kill; }

const DataFlowFacts &GenKillTransformer::getGen() const { return gen; }

const std::map<Value *, DataFlowFacts> &GenKillTransformer::getFlow() const {
  return flow;
}

std::ostream &GenKillTransformer::print(std::ostream &os) const {
  os << "GenKillTransformer{kill=";
  kill.print(os);
  os << ", gen=";
  gen.print(os);
  os << ", flow={";
  bool first = true;
  for (auto &kv : flow) {
    if (!first)
      os << ", ";
    first = false;
    // Print key
    if (kv.first->hasName())
      os << kv.first->getName().str();
    else
      os << kv.first;
    os << "->";
    kv.second.print(os);
  }
  os << "}}";
  return os;
}

} // namespace wpds
