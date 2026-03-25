/*

 * Author: rainoftime
*/
#include "Dataflow/WPDS/Core/DataFlowFacts.h"

#include <algorithm>

namespace wpds {

DataFlowFacts::DataFlowFacts() = default;

DataFlowFacts::DataFlowFacts(const std::set<Value *> &facts) : facts(facts) {}

DataFlowFacts::DataFlowFacts(const DataFlowFacts &other)
    : is_universe(other.is_universe), facts(other.facts) {}

DataFlowFacts &DataFlowFacts::operator=(const DataFlowFacts &other) {
  if (this != &other) {
    is_universe = other.is_universe;
    facts = other.facts;
  }
  return *this;
}

bool DataFlowFacts::operator==(const DataFlowFacts &other) const {
  if (is_universe != other.is_universe) {
    return false;
  }
  if (is_universe) {
    return true;
  }
  return facts == other.facts;
}

DataFlowFacts DataFlowFacts::EmptySet() { return DataFlowFacts(); }

DataFlowFacts DataFlowFacts::UniverseSet() {
  DataFlowFacts result;
  result.is_universe = true;
  return result;
}

void DataFlowFacts::ClearUniverse() {
  // No-op: Universe is represented symbolically.
}

DataFlowFacts DataFlowFacts::Union(const DataFlowFacts &x,
                                   const DataFlowFacts &y) {
  if (x.is_universe && y.is_universe) {
    DataFlowFacts result = UniverseSet();
    std::set_intersection(x.facts.begin(), x.facts.end(), y.facts.begin(),
                          y.facts.end(),
                          std::inserter(result.facts, result.facts.begin()));
    return result;
  }
  if (x.is_universe) {
    DataFlowFacts result = UniverseSet();
    std::set_difference(x.facts.begin(), x.facts.end(), y.facts.begin(),
                        y.facts.end(),
                        std::inserter(result.facts, result.facts.begin()));
    return result;
  }
  if (y.is_universe) {
    return Union(y, x);
  }
  DataFlowFacts result = x;
  result.facts.insert(y.facts.begin(), y.facts.end());
  return result;
}

DataFlowFacts DataFlowFacts::Intersect(const DataFlowFacts &x,
                                       const DataFlowFacts &y) {
  if (x.is_universe && y.is_universe) {
    DataFlowFacts result = UniverseSet();
    result.facts = x.facts;
    result.facts.insert(y.facts.begin(), y.facts.end());
    return result;
  }
  if (x.is_universe) {
    DataFlowFacts result;
    std::set_difference(y.facts.begin(), y.facts.end(), x.facts.begin(),
                        x.facts.end(),
                        std::inserter(result.facts, result.facts.begin()));
    return result;
  }
  if (y.is_universe) {
    return Intersect(y, x);
  }
  DataFlowFacts result;
  std::set_intersection(x.facts.begin(), x.facts.end(), y.facts.begin(),
                        y.facts.end(),
                        std::inserter(result.facts, result.facts.begin()));
  return result;
}

DataFlowFacts DataFlowFacts::Diff(const DataFlowFacts &x,
                                  const DataFlowFacts &y) {
  if (x.is_universe && y.is_universe) {
    DataFlowFacts result;
    std::set_difference(y.facts.begin(), y.facts.end(), x.facts.begin(),
                        x.facts.end(),
                        std::inserter(result.facts, result.facts.begin()));
    return result;
  }
  if (y.is_universe) {
    DataFlowFacts result;
    std::set_intersection(x.facts.begin(), x.facts.end(), y.facts.begin(),
                          y.facts.end(),
                          std::inserter(result.facts, result.facts.begin()));
    return result;
  }
  if (x.is_universe) {
    DataFlowFacts result = UniverseSet();
    result.facts = x.facts;
    result.facts.insert(y.facts.begin(), y.facts.end());
    return result;
  }
  DataFlowFacts result;
  std::set_difference(x.facts.begin(), x.facts.end(), y.facts.begin(),
                      y.facts.end(),
                      std::inserter(result.facts, result.facts.begin()));
  return result;
}

bool DataFlowFacts::Eq(const DataFlowFacts &x, const DataFlowFacts &y) {
  return x == y;
}

const std::set<Value *> &DataFlowFacts::getFacts() const { return facts; }

void DataFlowFacts::addFact(Value *val) {
  if (is_universe) {
    facts.erase(val);
    return;
  }
  facts.insert(val);
}

void DataFlowFacts::removeFact(Value *val) {
  if (is_universe) {
    facts.insert(val);
    return;
  }
  facts.erase(val);
}

bool DataFlowFacts::containsFact(Value *val) const {
  if (is_universe) {
    return facts.find(val) == facts.end();
  }
  return facts.find(val) != facts.end();
}

std::size_t DataFlowFacts::size() const {
  if (is_universe) {
    return 0;
  }
  return facts.size();
}

bool DataFlowFacts::isEmpty() const {
  if (is_universe) {
    return false;
  }
  return facts.empty();
}

std::ostream &DataFlowFacts::print(std::ostream &os) const {
  if (is_universe) {
    os << "DataFlowFacts{<universe>}";
    return os;
  }
  os << "DataFlowFacts{";
  bool first = true;
  for (auto *val : facts) {
    if (!first) {
      os << ", ";
    }
    first = false;

    if (val == nullptr) {
      os << "null";
      continue;
    }

    if (auto *inst = dyn_cast<Instruction>(val)) {
      if (inst->getName().empty()) {
        os << "<unnamed-inst>";
      } else {
        os << inst->getName().str();
      }
    } else if (auto *arg = dyn_cast<Argument>(val)) {
      if (arg->getName().empty()) {
        os << "<unnamed-arg>";
      } else {
        os << arg->getName().str();
      }
    } else if (auto *global = dyn_cast<GlobalValue>(val)) {
      os << global->getName().str();
    } else {
      os << "<unknown-value>";
    }
  }
  os << "}";
  return os;
}

} // namespace wpds
