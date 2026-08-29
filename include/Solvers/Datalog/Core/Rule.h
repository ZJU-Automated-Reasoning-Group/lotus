#pragma once

#include "Solvers/Datalog/Core/Aggregate.h"
#include "Solvers/Datalog/Core/Atom.h"
#include "Solvers/Datalog/Semantic/SemanticIR.h"

#include <vector>

namespace lotus::datalog {

class Body {
public:
  Body() = delete;

  Context *context() const { return context_; }
  const std::vector<BodyItemIR> &items() const { return items_; }

private:
  explicit Body(const Atom &atom)
      : context_(atom.context()), items_{atom.ir()} {}
  explicit Body(const Condition &condition)
      : context_(condition.context()), items_{condition.ir()} {}
  explicit Body(const Negation &negation)
      : context_(negation.context()), items_{negation.ir()} {}
  explicit Body(const AggregateClause &aggregate)
      : context_(aggregate.context()), items_{aggregate.ir()} {}

  void append(Context *context, BodyItemIR item);

  Context *context_ = nullptr;
  std::vector<BodyItemIR> items_;

  friend Body operator&&(const Atom &, const Atom &);
  friend Body operator&&(Body, const Atom &);
  friend Body operator&&(const Condition &, const Atom &);
  friend Body operator&&(const Atom &, const Condition &);
  friend Body operator&&(Body, const Condition &);
  friend Body operator&&(const Negation &, const Atom &);
  friend Body operator&&(const Atom &, const Negation &);
  friend Body operator&&(Body, const Negation &);
  friend Body operator&&(const Atom &, const AggregateClause &);
  friend Body operator&&(Body, const AggregateClause &);
  friend Body operator&&(const AggregateClause &, const Atom &);
  friend class Program;
};

Body operator&&(const Atom &lhs, const Atom &rhs);
Body operator&&(Body lhs, const Atom &rhs);
Body operator&&(const Condition &lhs, const Atom &rhs);
Body operator&&(const Atom &lhs, const Condition &rhs);
Body operator&&(Body lhs, const Condition &rhs);
Body operator&&(const Negation &lhs, const Atom &rhs);
Body operator&&(const Atom &lhs, const Negation &rhs);
Body operator&&(Body lhs, const Negation &rhs);
Body operator&&(const Atom &lhs, const AggregateClause &rhs);
Body operator&&(Body lhs, const AggregateClause &rhs);
Body operator&&(const AggregateClause &lhs, const Atom &rhs);

template <typename Input, typename Output>
AggregateClause aggregate(const Var<Output> &output,
                          const AggregatorSpec<Input, Output> &aggregator,
                          const Body &source) {
  Context *context =
      detail::mergeContexts(output.context(), aggregator.context());
  context = detail::mergeContexts(context, source.context());
  AggregateIR ir;
  ir.output_var = output.id();
  ir.output_type = typeid(Output);
  ir.projection = aggregator.projection_.lower();
  ir.name = aggregator.name_;
  ir.evaluate = aggregator.evaluator_;
  ir.reducer = aggregator.reducer_;
  ir.properties = aggregator.properties_;
  ir.monotone = aggregator.monotone_;
  for (const BodyItemIR &item : source.items()) {
    if (const auto *atom = std::get_if<AtomIR>(&item))
      ir.source_body.push_back(*atom);
    else if (const auto *filter = std::get_if<FilterIR>(&item))
      ir.source_body.push_back(*filter);
    else if (const auto *negation = std::get_if<NegAtomIR>(&item))
      ir.source_body.push_back(*negation);
    else
      throw std::invalid_argument(
          "nested Datalog aggregates are not supported");
  }
  if (ir.source_body.size() == 1 &&
      std::holds_alternative<AtomIR>(ir.source_body.front()))
    ir.source = std::get<AtomIR>(ir.source_body.front());
  return AggregateClause(context, std::move(ir));
}

using body = Body;
using rule = RuleIR;

} // namespace lotus::datalog
