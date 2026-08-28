#include "CFL/MCFL/Grammar.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lotus::cfl::mcfl {

bool Grammar::VariableRef::operator==(const VariableRef &other) const {
  return body == other.body && component == other.component;
}

Grammar::Nonterminal Grammar::addNonterminal(std::string name,
                                             std::size_t arity) {
  if (name.empty()) {
    throw std::invalid_argument("an MCFL nonterminal name cannot be empty");
  }
  if (arity == 0) {
    throw std::invalid_argument("an MCFL nonterminal must have positive arity");
  }
  if (name_to_nonterminal_.count(name) != 0U) {
    throw std::invalid_argument("duplicate MCFL nonterminal: " + name);
  }
  if (nonterminals_.size() ==
      static_cast<std::size_t>(std::numeric_limits<Nonterminal>::max())) {
    throw std::overflow_error("too many MCFL nonterminals");
  }

  const Nonterminal id = static_cast<Nonterminal>(nonterminals_.size());
  name_to_nonterminal_.emplace(name, id);
  nonterminals_.push_back({std::move(name), arity});
  return id;
}

Grammar::Nonterminal Grammar::nonterminal(std::string_view name) const {
  const auto found = name_to_nonterminal_.find(std::string(name));
  if (found == name_to_nonterminal_.end()) {
    throw std::out_of_range("unknown MCFL nonterminal: " + std::string(name));
  }
  return found->second;
}

bool Grammar::hasNonterminal(std::string_view name) const {
  return name_to_nonterminal_.count(std::string(name)) != 0U;
}

const Grammar::NonterminalInfo &Grammar::info(Nonterminal nonterminal) const {
  checkNonterminal(nonterminal);
  return nonterminals_[nonterminal];
}

void Grammar::setStart(Nonterminal start) {
  checkNonterminal(start);
  if (info(start).arity != 1) {
    throw std::invalid_argument("the MCFL start nonterminal must have arity 1");
  }
  start_ = start;
  has_start_ = true;
}

Grammar::Nonterminal Grammar::start() const {
  if (!has_start_) {
    throw std::logic_error("the MCFL grammar has no start nonterminal");
  }
  return start_;
}

void Grammar::addBasic(Nonterminal head, Label label) {
  checkNonterminal(head);
  basic_rules_.push_back({head, std::move(label)});
}

void Grammar::addPrepend(Nonterminal head, Nonterminal body, Label label,
                         std::size_t component) {
  checkNonterminal(head);
  checkNonterminal(body);
  prepend_rules_.push_back({head, body, std::move(label), component});
}

void Grammar::addAppend(Nonterminal head, Nonterminal body, Label label,
                        std::size_t component) {
  checkNonterminal(head);
  checkNonterminal(body);
  append_rules_.push_back({head, body, std::move(label), component});
}

void Grammar::addInsert(Nonterminal head, Nonterminal body, Label label,
                        std::size_t component) {
  checkNonterminal(head);
  checkNonterminal(body);
  insert_rules_.push_back({head, body, std::move(label), component});
}

void Grammar::addConcatenate(Nonterminal head, std::vector<Nonterminal> bodies,
                             std::vector<std::vector<VariableRef>> outputs) {
  checkNonterminal(head);
  for (Nonterminal body : bodies) {
    checkNonterminal(body);
  }
  concatenate_rules_.push_back({head, std::move(bodies), std::move(outputs)});
}

void Grammar::validate() const {
  if (!has_start_) {
    throw std::invalid_argument("the MCFL grammar has no start nonterminal");
  }
  if (info(start_).arity != 1) {
    throw std::invalid_argument("the MCFL start nonterminal must have arity 1");
  }

  for (const BasicRule &rule : basic_rules_) {
    if (info(rule.head).arity != 1) {
      throw std::invalid_argument("a type-1 MCFL rule must have arity 1");
    }
  }

  const auto validate_extension = [&](Nonterminal head, Nonterminal body,
                                      const Label &label,
                                      std::size_t component) {
    if (info(head).arity != info(body).arity) {
      throw std::invalid_argument(
          "type-2/3 MCFL rules must preserve predicate arity");
    }
    if (component >= info(body).arity) {
      throw std::invalid_argument("MCFL component index is out of range");
    }
    if (label.empty()) {
      throw std::invalid_argument(
          "type-2/3 MCFL rules cannot extend with epsilon");
    }
  };
  for (const PrependRule &rule : prepend_rules_) {
    validate_extension(rule.head, rule.body, rule.label, rule.component);
  }
  for (const AppendRule &rule : append_rules_) {
    validate_extension(rule.head, rule.body, rule.label, rule.component);
  }

  for (const InsertRule &rule : insert_rules_) {
    if (info(rule.head).arity != info(rule.body).arity + 1) {
      throw std::invalid_argument(
          "a type-4 MCFL rule must add exactly one component");
    }
    if (rule.component > info(rule.body).arity) {
      throw std::invalid_argument("MCFL insertion index is out of range");
    }
  }

  for (const ConcatenateRule &rule : concatenate_rules_) {
    if (rule.bodies.empty()) {
      throw std::invalid_argument(
          "a type-5 MCFL rule needs at least one body predicate");
    }
    if (rule.outputs.size() != info(rule.head).arity) {
      throw std::invalid_argument(
          "type-5 MCFL output count does not match head arity");
    }

    std::vector<std::vector<bool>> seen;
    std::vector<std::size_t> previous(rule.bodies.size(), 0);
    std::vector<bool> has_previous(rule.bodies.size(), false);
    for (Nonterminal body : rule.bodies) {
      seen.emplace_back(info(body).arity, false);
    }

    for (const std::vector<VariableRef> &output : rule.outputs) {
      for (const VariableRef &ref : output) {
        if (ref.body >= rule.bodies.size() ||
            ref.component >= info(rule.bodies[ref.body]).arity) {
          throw std::invalid_argument(
              "type-5 MCFL variable reference is out of range");
        }
        if (seen[ref.body][ref.component]) {
          throw std::invalid_argument(
              "type-5 MCFL rules must be non-deleting and linear");
        }
        if (has_previous[ref.body] && ref.component <= previous[ref.body]) {
          throw std::invalid_argument(
              "type-5 MCFL rules must be non-permuting");
        }
        seen[ref.body][ref.component] = true;
        previous[ref.body] = ref.component;
        has_previous[ref.body] = true;
      }
    }

    for (const std::vector<bool> &body_seen : seen) {
      if (std::find(body_seen.begin(), body_seen.end(), false) !=
          body_seen.end()) {
        throw std::invalid_argument(
            "type-5 MCFL rules must use every body variable exactly once");
      }
    }
  }
}

std::size_t Grammar::dimension() const {
  std::size_t result = 0;
  for (const NonterminalInfo &nonterminal : nonterminals_) {
    result = std::max(result, nonterminal.arity);
  }
  return result;
}

std::size_t Grammar::rank() const {
  std::size_t result =
      prepend_rules_.empty() && append_rules_.empty() && insert_rules_.empty()
          ? 0
          : 1;
  for (const ConcatenateRule &rule : concatenate_rules_) {
    result = std::max(result, rule.bodies.size());
  }
  return result;
}

void Grammar::checkNonterminal(Nonterminal nonterminal) const {
  if (nonterminal >= nonterminals_.size()) {
    throw std::out_of_range("MCFL nonterminal id is out of range");
  }
}

} // namespace lotus::cfl::mcfl
