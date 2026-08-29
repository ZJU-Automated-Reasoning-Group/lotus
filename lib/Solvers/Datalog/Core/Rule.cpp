#include "Solvers/Datalog/Core/Rule.h"

#include "Solvers/Datalog/Core/Program.h"
#include "Solvers/Datalog/Core/TypeSupport.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace lotus::datalog {

void Body::append(Context *context, BodyItemIR item) {
  context_ = detail::mergeContexts(context_, context);
  items_.push_back(std::move(item));
}

Body operator&&(const Atom &lhs, const Atom &rhs) {
  Body body(lhs);
  body.append(rhs.context(), rhs.ir());
  return body;
}

Body operator&&(Body lhs, const Atom &rhs) {
  lhs.append(rhs.context(), rhs.ir());
  return lhs;
}

Body operator&&(const Condition &lhs, const Atom &rhs) {
  Body body(lhs);
  body.append(rhs.context(), rhs.ir());
  return body;
}

Body operator&&(const Atom &lhs, const Condition &rhs) {
  Body body(lhs);
  body.append(rhs.context(), rhs.ir());
  return body;
}

Body operator&&(Body lhs, const Condition &rhs) {
  lhs.append(rhs.context(), rhs.ir());
  return lhs;
}

Body operator&&(const Negation &lhs, const Atom &rhs) {
  Body body(lhs);
  body.append(rhs.context(), rhs.ir());
  return body;
}

Body operator&&(const Atom &lhs, const Negation &rhs) {
  Body body(lhs);
  body.append(rhs.context(), rhs.ir());
  return body;
}

Body operator&&(Body lhs, const Negation &rhs) {
  lhs.append(rhs.context(), rhs.ir());
  return lhs;
}

Body operator&&(const Atom &lhs, const AggregateClause &rhs) {
  Body body(lhs);
  body.append(rhs.context(), rhs.ir());
  return body;
}

Body operator&&(Body lhs, const AggregateClause &rhs) {
  lhs.append(rhs.context(), rhs.ir());
  return lhs;
}

Body operator&&(const AggregateClause &lhs, const Atom &rhs) {
  Body body(lhs);
  body.append(rhs.context(), rhs.ir());
  return body;
}

void Program::rule(const Atom &head, const Atom &body) {
  addRule(head, body.context(), {body.ir()});
}

void Program::rule(const Atom &head, const Body &body) {
  addRule(head, body.context(), body.items());
}

void Program::rule(const Atom &head, const Condition &body) {
  addRule(head, body.context(), {body.ir()});
}

void Program::rule(const Atom &head, const Negation &body) {
  addRule(head, body.context(), {body.ir()});
}

void Program::rule(const Atom &head, const AggregateClause &body) {
  addRule(head, body.context(), {body.ir()});
}

void Program::rule(std::initializer_list<Atom> heads, const Atom &body) {
  addRules(heads, body.context(), {body.ir()});
}

void Program::rule(std::initializer_list<Atom> heads, const Body &body) {
  addRules(heads, body.context(), body.items());
}

void Program::rule(std::initializer_list<Atom> heads, const Condition &body) {
  addRules(heads, body.context(), {body.ir()});
}

void Program::rule(std::initializer_list<Atom> heads, const Negation &body) {
  addRules(heads, body.context(), {body.ir()});
}

void Program::rule(std::initializer_list<Atom> heads,
                   const AggregateClause &body) {
  addRules(heads, body.context(), {body.ir()});
}

void Program::addRule(const Atom &head, Context *body_context,
                      std::vector<BodyItemIR> body) {
  if (head.context() != context_)
    throw std::invalid_argument("Datalog rule head belongs to another context");
  if (body_context && body_context != context_)
    throw std::invalid_argument("Datalog rule body belongs to another context");
  rules_.push_back({head.ir(), std::move(body)});
}

void Program::addRules(std::initializer_list<Atom> heads,
                       Context *body_context,
                       const std::vector<BodyItemIR> &body) {
  if (heads.size() == 0)
    throw std::invalid_argument("Datalog rule requires at least one head");
  for (const Atom &head : heads)
    addRule(head, body_context, body);
}

} // namespace lotus::datalog
