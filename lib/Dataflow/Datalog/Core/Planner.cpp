#include "Dataflow/Datalog/EngineInternal.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <sstream>

namespace lotus::datalog::internal {
namespace {

std::string variableLabel(const VariableDefinition &variable) {
  return variable.anonymous ? "anonymous variable"
                            : "variable '" + variable.name + "'";
}

void validateTermType(const TermIR &term, const ColumnType &column,
                      const std::string &relation_name,
                      std::size_t column_index) {
  if (term.type != column.type) {
    std::ostringstream message;
    message << "type mismatch in relation '" << relation_name << "' column "
            << column_index;
    throw CompileError(message.str());
  }
}

void validateConstant(const TermIR &term, const ColumnType &column,
                      const std::string &relation_name,
                      std::size_t column_index, bool is_key) {
  if (term.kind != TermIR::Kind::Constant)
    return;
  try {
    if (column.validate)
      column.validate(term.constant);
    if (is_key && column.validate_key)
      column.validate_key(term.constant);
  } catch (const std::exception &error) {
    std::ostringstream message;
    message << "invalid constant in relation '" << relation_name << "' column "
            << column_index << ": " << error.what();
    throw CompileError(message.str());
  }
}

void validateAtom(
    const AtomIR &atom,
    const std::vector<std::unique_ptr<RelationStorage>> &relations,
    const std::vector<VariableDefinition> &variables,
    const std::string &position) {
  if (atom.relation >= relations.size())
    throw CompileError(position + " references an unknown relation");
  const RelationIR &relation = relations[atom.relation]->definition();
  if (atom.args.size() != relation.columns.size())
    throw CompileError("arity mismatch in relation '" + relation.name + "'");
  for (std::size_t i = 0; i < atom.args.size(); ++i) {
    const TermIR &term = atom.args[i];
    validateTermType(term, relation.columns[i], relation.name, i);
    const bool is_key =
        relation.kind == RelationKind::Set || i + 1 != relation.columns.size();
    validateConstant(term, relation.columns[i], relation.name, i, is_key);
    if (term.kind == TermIR::Kind::Expression) {
      throw CompileError("expression terms are not allowed in " + position +
                         " relation '" + relation.name +
                         "'; use where(...) instead");
    }
    if (term.kind != TermIR::Kind::Variable)
      continue;
    if (term.variable >= variables.size())
      throw CompileError(position + " references an unknown variable");
    if (variables[term.variable].type != term.type)
      throw CompileError("inconsistent type for " +
                         variableLabel(variables[term.variable]));
  }
}

bool referencesGrounded(const std::vector<VarId> &references,
                        const std::vector<bool> &grounded) {
  return std::all_of(references.begin(), references.end(), [&](VarId variable) {
    return variable < grounded.size() && grounded[variable];
  });
}

std::vector<VarId> atomVariables(const AtomIR &atom) {
  std::vector<VarId> variables;
  for (const TermIR &term : atom.args) {
    if (term.kind == TermIR::Kind::Variable)
      variables.push_back(term.variable);
  }
  return variables;
}

std::size_t saturatingAdd(std::size_t lhs, std::size_t rhs) {
  if (lhs > std::numeric_limits<std::size_t>::max() - rhs)
    return std::numeric_limits<std::size_t>::max();
  return lhs + rhs;
}

std::size_t saturatingMultiply(std::size_t lhs, std::size_t rhs) {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs)
    return std::numeric_limits<std::size_t>::max();
  return lhs * rhs;
}

std::size_t estimateAtomCost(
    const AtomIR &atom, const std::vector<bool> &grounded,
    const std::vector<std::unique_ptr<RelationStorage>> &relations) {
  ColumnMask mask = 0;
  bool has_grounded_variable = false;
  bool constants_only = true;
  for (std::size_t column = 0; column < atom.args.size(); ++column) {
    const TermIR &term = atom.args[column];
    if (term.kind == TermIR::Kind::Constant) {
      mask |= ColumnMask{1} << column;
    } else if (term.kind == TermIR::Kind::Variable && grounded[term.variable]) {
      mask |= ColumnMask{1} << column;
      has_grounded_variable = true;
      constants_only = false;
    }
  }
  const RelationStorage &storage = *relations[atom.relation];
  const RelationStorage::MaskStatistics statistics =
      storage.maskStatistics(mask);
  if (mask == 0)
    return std::max<std::size_t>(1, statistics.row_count);

  if (constants_only) {
    for (const auto &heavy_hitter : statistics.heavy_hitters) {
      bool equal = true;
      std::size_t key_column = 0;
      for (std::size_t column = 0; column < atom.args.size(); ++column) {
        if ((mask & (ColumnMask{1} << column)) == 0)
          continue;
        if (!storage.definition().columns[column].equal(
                atom.args[column].constant, heavy_hitter.key[key_column++])) {
          equal = false;
          break;
        }
      }
      if (equal)
        return std::max<std::size_t>(1, heavy_hitter.frequency);
    }
  }

  const std::size_t average = statistics.averageFrequency();
  if (!has_grounded_variable || statistics.maximum_frequency <= average * 4)
    return average;
  return average + (statistics.maximum_frequency - average) / 4;
}

void markAtomGrounded(const AtomIR &atom, std::vector<bool> &grounded) {
  for (VarId variable : atomVariables(atom))
    grounded[variable] = true;
}

bool negationAvailable(const NegAtomIR &negation,
                       const std::vector<bool> &grounded,
                       const std::vector<VariableDefinition> &variables) {
  for (VarId variable : atomVariables(negation.atom)) {
    if (!variables[variable].anonymous && !grounded[variable])
      return false;
  }
  return true;
}

struct JoinOrderState {
  bool valid = false;
  std::size_t cost = 0;
  std::size_t rows = 1;
  std::vector<std::size_t> order;
  std::vector<bool> grounded;
  std::vector<bool> selected;
};

bool betterJoinOrder(const JoinOrderState &lhs, const JoinOrderState &rhs) {
  if (!rhs.valid)
    return true;
  if (lhs.cost != rhs.cost)
    return lhs.cost < rhs.cost;
  if (lhs.rows != rhs.rows)
    return lhs.rows < rhs.rows;
  return lhs.order < rhs.order;
}

std::size_t
applyAvailableSelectivity(std::size_t rows, const std::vector<BodyItemIR> &body,
                          const std::vector<std::size_t> &segment_items,
                          const std::vector<bool> &before,
                          const std::vector<bool> &after,
                          const std::vector<VariableDefinition> &variables) {
  for (std::size_t item_index : segment_items) {
    bool was_available = false;
    bool is_available = false;
    if (const auto *filter = std::get_if<FilterIR>(&body[item_index])) {
      was_available =
          referencesGrounded(filter->predicate.referenced_vars, before);
      is_available =
          referencesGrounded(filter->predicate.referenced_vars, after);
    } else if (const auto *negation =
                   std::get_if<NegAtomIR>(&body[item_index])) {
      was_available = negationAvailable(*negation, before, variables);
      is_available = negationAvailable(*negation, after, variables);
    }
    if (!was_available && is_available)
      rows = std::max<std::size_t>(1, rows / 2);
  }
  return rows;
}

JoinOrderState
extendJoinOrder(const JoinOrderState &state, std::size_t atom_position,
                const std::vector<std::size_t> &atoms, const RuleIR &rule,
                const std::vector<std::size_t> &segment_items,
                const std::vector<std::unique_ptr<RelationStorage>> &relations,
                const std::vector<VariableDefinition> &variables) {
  JoinOrderState next = state;
  next.valid = true;
  const AtomIR &atom = std::get<AtomIR>(rule.body[atoms[atom_position]]);
  const std::size_t lookup = estimateAtomCost(atom, state.grounded, relations);
  next.rows = saturatingMultiply(state.rows, lookup);
  const std::vector<bool> before = next.grounded;
  markAtomGrounded(atom, next.grounded);
  next.rows = applyAvailableSelectivity(next.rows, rule.body, segment_items,
                                        before, next.grounded, variables);
  next.cost = saturatingAdd(state.cost, next.rows);
  next.order.push_back(atoms[atom_position]);
  next.selected[atom_position] = true;
  return next;
}

std::vector<std::size_t> optimizePositiveAtoms(
    const RuleIR &rule, const std::vector<std::size_t> &segment_items,
    const std::vector<bool> &initial_grounded,
    const std::vector<std::unique_ptr<RelationStorage>> &relations,
    const std::vector<VariableDefinition> &variables) {
  std::vector<std::size_t> atoms;
  for (std::size_t item_index : segment_items) {
    if (std::holds_alternative<AtomIR>(rule.body[item_index]))
      atoms.push_back(item_index);
  }
  if (atoms.size() < 2)
    return atoms;

  JoinOrderState initial;
  initial.valid = true;
  initial.grounded = initial_grounded;
  initial.selected.assign(atoms.size(), false);
  const bool has_self_recursive_atom =
      std::any_of(atoms.begin(), atoms.end(), [&](std::size_t item_index) {
        return std::get<AtomIR>(rule.body[item_index]).relation ==
               rule.head.relation;
      });

  if (atoms.size() <= 8) {
    const std::size_t state_count = std::size_t{1} << atoms.size();
    std::vector<JoinOrderState> states(state_count);
    states[0] = initial;
    for (std::size_t subset = 0; subset < state_count; ++subset) {
      if (!states[subset].valid)
        continue;
      for (std::size_t atom = 0; atom < atoms.size(); ++atom) {
        if (subset & (std::size_t{1} << atom))
          continue;
        if (subset == 0 && has_self_recursive_atom &&
            std::get<AtomIR>(rule.body[atoms[atom]]).relation !=
                rule.head.relation)
          continue;
        JoinOrderState candidate =
            extendJoinOrder(states[subset], atom, atoms, rule, segment_items,
                            relations, variables);
        const std::size_t next_subset = subset | (std::size_t{1} << atom);
        if (betterJoinOrder(candidate, states[next_subset]))
          states[next_subset] = std::move(candidate);
      }
    }
    return states.back().order;
  }

  constexpr std::size_t BEAM_WIDTH = 32;
  std::vector<JoinOrderState> beam{std::move(initial)};
  for (std::size_t depth = 0; depth < atoms.size(); ++depth) {
    std::vector<JoinOrderState> expanded;
    for (const JoinOrderState &state : beam) {
      for (std::size_t atom = 0; atom < atoms.size(); ++atom) {
        if (!state.selected[atom] &&
            (depth != 0 || !has_self_recursive_atom ||
             std::get<AtomIR>(rule.body[atoms[atom]]).relation ==
                 rule.head.relation))
          expanded.push_back(extendJoinOrder(
              state, atom, atoms, rule, segment_items, relations, variables));
      }
    }
    std::sort(expanded.begin(), expanded.end(), betterJoinOrder);
    if (expanded.size() > BEAM_WIDTH)
      expanded.resize(BEAM_WIDTH);
    beam = std::move(expanded);
  }
  return beam.front().order;
}

std::vector<bool> planAggregateSource(
    AggregateIR &aggregate, const std::vector<bool> &outer_grounded,
    const std::vector<std::unique_ptr<RelationStorage>> &relations,
    const std::vector<VariableDefinition> &variables) {
  if (aggregate.source_body.empty())
    aggregate.source_body.push_back(aggregate.source);
  std::vector<bool> grounded = outer_grounded;
  std::vector<std::size_t> pending;
  for (std::size_t index = 0; index < aggregate.source_body.size(); ++index) {
    pending.push_back(index);
    const AggregateSourceItemIR &item = aggregate.source_body[index];
    if (const auto *atom = std::get_if<AtomIR>(&item))
      validateAtom(*atom, relations, variables, "aggregate source atom");
    else if (const auto *negation = std::get_if<NegAtomIR>(&item))
      validateAtom(negation->atom, relations, variables,
                   "aggregate negated atom");
    else {
      const FilterIR &filter = std::get<FilterIR>(item);
      if (filter.predicate.type != typeid(bool) || !filter.predicate.evaluate)
        throw CompileError("aggregate filter must be an evaluable bool");
    }
  }

  std::vector<AggregateSourceItemIR> planned;
  while (!pending.empty()) {
    auto chosen = pending.end();
    for (auto it = pending.begin(); it != pending.end(); ++it) {
      const AggregateSourceItemIR &item = aggregate.source_body[*it];
      if (const auto *filter = std::get_if<FilterIR>(&item)) {
        if (referencesGrounded(filter->predicate.referenced_vars, grounded)) {
          chosen = it;
          break;
        }
      } else if (const auto *negation = std::get_if<NegAtomIR>(&item)) {
        if (negationAvailable(*negation, grounded, variables)) {
          chosen = it;
          break;
        }
      }
    }
    if (chosen == pending.end()) {
      std::size_t best_cost = std::numeric_limits<std::size_t>::max();
      for (auto it = pending.begin(); it != pending.end(); ++it) {
        const auto *atom = std::get_if<AtomIR>(&aggregate.source_body[*it]);
        if (!atom)
          continue;
        const std::size_t cost = estimateAtomCost(*atom, grounded, relations);
        if (cost < best_cost) {
          best_cost = cost;
          chosen = it;
        }
      }
    }
    if (chosen == pending.end())
      throw CompileError("unable to ground aggregate source body");
    AggregateSourceItemIR selected = aggregate.source_body[*chosen];
    if (const auto *atom = std::get_if<AtomIR>(&selected))
      markAtomGrounded(*atom, grounded);
    planned.push_back(std::move(selected));
    pending.erase(chosen);
  }
  aggregate.source_body = std::move(planned);
  if (aggregate.source_body.size() == 1 &&
      std::holds_alternative<AtomIR>(aggregate.source_body.front()))
    aggregate.source = std::get<AtomIR>(aggregate.source_body.front());
  return grounded;
}

} // namespace

std::vector<RuleIR> planAndValidateRules(
    const std::vector<RuleIR> &input_rules,
    const std::vector<std::unique_ptr<RelationStorage>> &relations,
    const std::vector<VariableDefinition> &variables,
    std::size_t &reorder_count) {
  std::vector<RuleIR> rules;
  rules.reserve(input_rules.size());

  for (const RuleIR &input_rule : input_rules) {
    RuleIR rule = input_rule;
    std::vector<bool> grounded(variables.size(), false);
    std::vector<BodyItemIR> planned_body;
    std::size_t cursor = 0;

    while (cursor < rule.body.size()) {
      std::size_t aggregate_position = cursor;
      while (
          aggregate_position < rule.body.size() &&
          !std::holds_alternative<AggregateIR>(rule.body[aggregate_position]))
        ++aggregate_position;

      std::vector<std::size_t> pending;
      for (std::size_t i = cursor; i < aggregate_position; ++i) {
        pending.push_back(i);
        if (const auto *atom = std::get_if<AtomIR>(&rule.body[i]))
          validateAtom(*atom, relations, variables, "body atom");
        else if (const auto *negation = std::get_if<NegAtomIR>(&rule.body[i]))
          validateAtom(negation->atom, relations, variables, "negated atom");
        else {
          const FilterIR &filter = std::get<FilterIR>(rule.body[i]);
          if (filter.predicate.type != typeid(bool))
            throw CompileError("where(...) condition must have bool type");
          if (!filter.predicate.evaluate)
            throw CompileError("where(...) condition has no evaluator");
        }
      }
      const std::vector<std::size_t> positive_order =
          optimizePositiveAtoms(rule, pending, grounded, relations, variables);
      std::size_t positive_cursor = 0;

      while (!pending.empty()) {
        const std::size_t expected_position =
            aggregate_position - pending.size();
        auto chosen = pending.end();
        for (auto it = pending.begin(); it != pending.end(); ++it) {
          const BodyItemIR &item = rule.body[*it];
          if (const auto *filter = std::get_if<FilterIR>(&item)) {
            if (referencesGrounded(filter->predicate.referenced_vars,
                                   grounded)) {
              chosen = it;
              break;
            }
          } else if (const auto *negation = std::get_if<NegAtomIR>(&item)) {
            if (negationAvailable(*negation, grounded, variables)) {
              chosen = it;
              break;
            }
          }
        }

        if (chosen == pending.end()) {
          if (positive_cursor < positive_order.size()) {
            chosen = std::find(pending.begin(), pending.end(),
                               positive_order[positive_cursor++]);
          }
        }

        if (chosen == pending.end()) {
          const BodyItemIR &blocked = rule.body[pending.front()];
          if (const auto *filter = std::get_if<FilterIR>(&blocked)) {
            for (VarId variable : filter->predicate.referenced_vars) {
              if (variable >= variables.size() || !grounded[variable]) {
                const std::string label =
                    variable < variables.size()
                        ? variableLabel(variables[variable])
                        : "unknown variable";
                throw CompileError(label + " used in filter is not grounded");
              }
            }
          }
          const NegAtomIR &negation = std::get<NegAtomIR>(blocked);
          for (VarId variable : atomVariables(negation.atom)) {
            if (!variables[variable].anonymous && !grounded[variable])
              throw CompileError(variableLabel(variables[variable]) +
                                 " used in negated atom is not grounded");
          }
          throw CompileError("unable to ground rule body");
        }

        const std::size_t original_position = *chosen;
        if (original_position != expected_position)
          ++reorder_count;
        BodyItemIR selected = rule.body[original_position];
        if (const auto *atom = std::get_if<AtomIR>(&selected))
          markAtomGrounded(*atom, grounded);
        planned_body.push_back(std::move(selected));
        pending.erase(chosen);
      }

      if (aggregate_position == rule.body.size())
        break;

      AggregateIR aggregate =
          std::get<AggregateIR>(rule.body[aggregate_position]);
      if (aggregate.output_var >= variables.size())
        throw CompileError("aggregate references an unknown output variable");
      if (variables[aggregate.output_var].anonymous)
        throw CompileError("aggregate output may not be anonymous");
      if (variables[aggregate.output_var].type != aggregate.output_type)
        throw CompileError("aggregate output variable has inconsistent type");
      if (grounded[aggregate.output_var])
        throw CompileError("aggregate output variable is already grounded");
      if (!aggregate.evaluate)
        throw CompileError("aggregate '" + aggregate.name +
                           "' has no evaluator");
      if (aggregate.monotone) {
        if (rule.head.relation >= relations.size() ||
            relations[rule.head.relation]->definition().kind !=
                RelationKind::Lattice) {
          throw CompileError(
              "monotone recursive aggregates require a lattice head relation");
        }
        for (const AggregateSourceItemIR &source_item : aggregate.source_body) {
          if (std::holds_alternative<NegAtomIR>(source_item))
            throw CompileError(
                "monotone aggregate sources may not contain negation");
        }
      }

      if (aggregate.source_body.empty())
        aggregate.source_body.push_back(aggregate.source);
      for (const AggregateSourceItemIR &source_item : aggregate.source_body) {
        const AtomIR *source_atom = nullptr;
        if (const auto *atom = std::get_if<AtomIR>(&source_item))
          source_atom = atom;
        else if (const auto *negation = std::get_if<NegAtomIR>(&source_item))
          source_atom = &negation->atom;
        if (!source_atom)
          continue;
        for (VarId variable : atomVariables(*source_atom)) {
          if (variable == aggregate.output_var)
            throw CompileError(
                "aggregate output variable may not appear in its source body");
        }
      }
      const std::vector<bool> aggregate_grounded =
          planAggregateSource(aggregate, grounded, relations, variables);
      if (!referencesGrounded(aggregate.projection.referenced_vars,
                              aggregate_grounded)) {
        throw CompileError("aggregate projection references an ungrounded "
                           "variable");
      }
      planned_body.push_back(aggregate);
      grounded[aggregate.output_var] = true;
      cursor = aggregate_position + 1;
    }

    rule.body = std::move(planned_body);
    if (rule.head.relation >= relations.size())
      throw CompileError("rule head references an unknown relation");
    const RelationIR &head_relation =
        relations[rule.head.relation]->definition();
    if (rule.head.args.size() != head_relation.columns.size())
      throw CompileError("arity mismatch in relation '" + head_relation.name +
                         "'");
    for (std::size_t i = 0; i < rule.head.args.size(); ++i) {
      const TermIR &term = rule.head.args[i];
      validateTermType(term, head_relation.columns[i], head_relation.name, i);
      const bool is_key = head_relation.kind == RelationKind::Set ||
                          i + 1 != head_relation.columns.size();
      validateConstant(term, head_relation.columns[i], head_relation.name, i,
                       is_key);
      if (term.kind == TermIR::Kind::Variable) {
        if (term.variable >= variables.size() || !grounded[term.variable]) {
          const std::string label =
              term.variable < variables.size()
                  ? variableLabel(variables[term.variable])
                  : "unknown variable";
          throw CompileError(label + " used in head is not grounded");
        }
      } else if (term.kind == TermIR::Kind::Expression) {
        if (!term.expression.evaluate)
          throw CompileError("head expression has no evaluator");
        if (!referencesGrounded(term.expression.referenced_vars, grounded))
          throw CompileError(
              "head expression references an ungrounded variable");
      }
    }
    rules.push_back(std::move(rule));
  }
  return rules;
}

AtomPlan lowerAtomPlan(const AtomIR &atom, const std::vector<bool> &grounded) {
  AtomPlan result;
  result.relation = atom.relation;
  result.terms.reserve(atom.args.size());
  for (std::size_t column = 0; column < atom.args.size(); ++column) {
    const TermIR &term = atom.args[column];
    AtomTermPlan planned_term;
    if (term.kind == TermIR::Kind::Variable) {
      planned_term.is_variable = true;
      planned_term.variable = term.variable;
      planned_term.anonymous = term.anonymous;
      planned_term.use_in_lookup =
          term.variable < grounded.size() && grounded[term.variable];
    } else {
      planned_term.constant = term.constant;
      planned_term.use_in_lookup = true;
    }
    if (planned_term.use_in_lookup)
      result.lookup_mask |= ColumnMask{1} << column;
    result.terms.push_back(std::move(planned_term));
  }
  return result;
}

RulePlan
lowerRulePlan(const RuleIR &rule,
              const std::vector<std::unique_ptr<RelationStorage>> &relations) {
  RulePlan result;
  result.head_relation = rule.head.relation;
  const RelationIR &head_definition =
      relations[rule.head.relation]->definition();
  for (const ColumnType &column : head_definition.columns)
    result.parallel_safe =
        result.parallel_safe && column.properties.canRunInParallel();
  if (head_definition.kind == RelationKind::Lattice)
    result.parallel_safe =
        result.parallel_safe &&
        head_definition.lattice_properties.canRunInParallel();
  result.head.reserve(rule.head.args.size());
  for (const TermIR &term : rule.head.args) {
    HeadTermPlan planned_term;
    if (term.kind == TermIR::Kind::Variable) {
      planned_term.kind = HeadTermPlan::Kind::Variable;
      planned_term.variable = term.variable;
    } else if (term.kind == TermIR::Kind::Expression) {
      planned_term.kind = HeadTermPlan::Kind::Expression;
      planned_term.expression = term.expression;
      result.parallel_safe =
          result.parallel_safe && term.expression.properties.canRunInParallel();
    } else {
      planned_term.constant = term.constant;
    }
    result.head.push_back(std::move(planned_term));
  }

  result.body.reserve(rule.body.size());
  std::vector<bool> grounded;
  for (const BodyItemIR &item : rule.body) {
    if (const auto *atom = std::get_if<AtomIR>(&item)) {
      for (const TermIR &term : atom->args) {
        if (term.kind == TermIR::Kind::Variable &&
            term.variable >= grounded.size())
          grounded.resize(term.variable + 1, false);
      }
    } else if (const auto *aggregate = std::get_if<AggregateIR>(&item)) {
      for (const AggregateSourceItemIR &source_item : aggregate->source_body) {
        const AtomIR *source_atom = nullptr;
        if (const auto *atom = std::get_if<AtomIR>(&source_item))
          source_atom = atom;
        else if (const auto *negation = std::get_if<NegAtomIR>(&source_item))
          source_atom = &negation->atom;
        if (!source_atom)
          continue;
        for (const TermIR &term : source_atom->args) {
          if (term.kind == TermIR::Kind::Variable &&
              term.variable >= grounded.size())
            grounded.resize(term.variable + 1, false);
        }
      }
      if (aggregate->output_var >= grounded.size())
        grounded.resize(aggregate->output_var + 1, false);
    }
  }
  std::size_t estimated_rows = 1;
  for (const BodyItemIR &item : rule.body) {
    PhysicalOp operation;
    operation.estimated_input_rows = estimated_rows;
    if (const auto *atom = std::get_if<AtomIR>(&item)) {
      operation.code = OpCode::Scan;
      operation.atom = lowerAtomPlan(*atom, grounded);
      for (const ColumnType &column :
           relations[atom->relation]->definition().columns) {
        result.parallel_safe =
            result.parallel_safe && column.properties.canRunInParallel();
      }
      operation.estimated_lookup_rows =
          relations[atom->relation]->estimatedLookupCardinality(
              operation.atom.lookup_mask);
      if (estimated_rows > std::numeric_limits<std::size_t>::max() /
                               operation.estimated_lookup_rows) {
        estimated_rows = std::numeric_limits<std::size_t>::max();
      } else {
        estimated_rows *= operation.estimated_lookup_rows;
      }
      for (const TermIR &term : atom->args) {
        if (term.kind == TermIR::Kind::Variable)
          grounded[term.variable] = true;
      }
    } else if (const auto *filter = std::get_if<FilterIR>(&item)) {
      operation.code = OpCode::Filter;
      operation.filter = filter->predicate;
      result.parallel_safe = result.parallel_safe &&
                             filter->predicate.properties.canRunInParallel();
      estimated_rows = std::max<std::size_t>(1, estimated_rows / 2);
    } else if (const auto *negation = std::get_if<NegAtomIR>(&item)) {
      operation.code = OpCode::AntiLookup;
      operation.atom = lowerAtomPlan(negation->atom, grounded);
      for (const ColumnType &column :
           relations[negation->atom.relation]->definition().columns) {
        result.parallel_safe =
            result.parallel_safe && column.properties.canRunInParallel();
      }
      operation.estimated_lookup_rows =
          relations[negation->atom.relation]->estimatedLookupCardinality(
              operation.atom.lookup_mask);
      estimated_rows = std::max<std::size_t>(1, estimated_rows / 2);
    } else {
      const auto &aggregate = std::get<AggregateIR>(item);
      operation.code = OpCode::Aggregate;
      operation.aggregate = aggregate;
      result.parallel_safe =
          result.parallel_safe &&
          aggregate.projection.properties.canRunInParallel() &&
          aggregate.properties.canRunInParallel();
      std::vector<bool> source_grounded = grounded;
      operation.estimated_lookup_rows = 1;
      for (const AggregateSourceItemIR &source_item : aggregate.source_body) {
        AggregateSourceOp source_operation;
        if (const auto *source_atom = std::get_if<AtomIR>(&source_item)) {
          source_operation.code = OpCode::Scan;
          source_operation.atom = lowerAtomPlan(*source_atom, source_grounded);
          const RelationIR &source_definition =
              relations[source_atom->relation]->definition();
          for (const ColumnType &column : source_definition.columns) {
            result.parallel_safe =
                result.parallel_safe && column.properties.canRunInParallel();
          }
          const std::size_t lookup =
              relations[source_atom->relation]->estimatedLookupCardinality(
                  source_operation.atom.lookup_mask);
          operation.estimated_lookup_rows =
              saturatingMultiply(operation.estimated_lookup_rows, lookup);
          for (const TermIR &term : source_atom->args) {
            if (term.kind == TermIR::Kind::Variable)
              source_grounded[term.variable] = true;
          }
        } else if (const auto *filter = std::get_if<FilterIR>(&source_item)) {
          source_operation.code = OpCode::Filter;
          source_operation.filter = filter->predicate;
          result.parallel_safe =
              result.parallel_safe &&
              filter->predicate.properties.canRunInParallel();
        } else {
          const auto &negation = std::get<NegAtomIR>(source_item);
          source_operation.code = OpCode::AntiLookup;
          source_operation.atom = lowerAtomPlan(negation.atom, source_grounded);
          for (const ColumnType &column :
               relations[negation.atom.relation]->definition().columns) {
            result.parallel_safe =
                result.parallel_safe && column.properties.canRunInParallel();
          }
        }
        operation.aggregate_body.push_back(std::move(source_operation));
      }
      if (operation.aggregate_body.size() == 1 &&
          operation.aggregate_body.front().code == OpCode::Scan)
        operation.atom = operation.aggregate_body.front().atom;
      if (aggregate.output_var >= grounded.size())
        grounded.resize(aggregate.output_var + 1, false);
      grounded[aggregate.output_var] = true;
    }
    operation.estimated_output_rows = estimated_rows;
    result.body.push_back(std::move(operation));
  }
  for (std::size_t filter_index = 0; filter_index < result.body.size();
       ++filter_index) {
    PhysicalOp &filter = result.body[filter_index];
    if (filter.code != OpCode::Filter || !filter.filter.node)
      continue;
    const ExprNode &node = *filter.filter.node;
    if (node.opcode != ExprOpcode::Less &&
        node.opcode != ExprOpcode::LessEqual &&
        node.opcode != ExprOpcode::Greater &&
        node.opcode != ExprOpcode::GreaterEqual)
      continue;
    const ExprNode *variable = nullptr;
    const ExprNode *constant = nullptr;
    bool variable_on_left = true;
    if (node.lhs && node.rhs && node.lhs->opcode == ExprOpcode::Variable &&
        node.rhs->opcode == ExprOpcode::Constant) {
      variable = node.lhs.get();
      constant = node.rhs.get();
    } else if (node.lhs && node.rhs &&
               node.rhs->opcode == ExprOpcode::Variable &&
               node.lhs->opcode == ExprOpcode::Constant) {
      variable = node.rhs.get();
      constant = node.lhs.get();
      variable_on_left = false;
    } else {
      continue;
    }
    for (std::size_t scan_index = filter_index; scan_index-- > 0;) {
      PhysicalOp &scan = result.body[scan_index];
      if (scan.code != OpCode::Scan)
        continue;
      for (std::size_t column = 0; column < scan.atom.terms.size(); ++column) {
        const AtomTermPlan &term = scan.atom.terms[column];
        if (!term.is_variable || term.variable != variable->variable ||
            term.use_in_lookup ||
            !relations[scan.atom.relation]
                 ->definition()
                 .columns[column]
                 .less_value)
          continue;
        scan.range = RangePlan{column, node.opcode, variable_on_left,
                               constant->constant};
        break;
      }
      if (scan.range)
        break;
    }
  }
  const bool projection_head = std::none_of(
      result.head.begin(), result.head.end(), [](const HeadTermPlan &term) {
        return term.kind == HeadTermPlan::Kind::Expression;
      });
  if (projection_head && result.body.size() == 1 &&
      result.body.front().code == OpCode::Scan) {
    result.kernel_kind = RulePlan::KernelKind::Projection;
  }
  return result;
}

} // namespace lotus::datalog::internal
