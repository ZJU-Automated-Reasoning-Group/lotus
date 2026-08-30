#include "Solvers/Datalog/Core/Program.h"
#include "Solvers/Datalog/Internal/Engine.h"

#include <algorithm>
#include <any>
#include <array>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::datalog {

using internal::AggregateSourceOp;
using internal::AtomPlan;
using internal::AtomTermPlan;
using internal::buildSCCPlan;
using internal::collectDependencies;
using internal::computeStrata;
using internal::DependencyEdge;
using internal::ExecutionPlan;
using internal::HeadTermPlan;
using internal::KeyView;
using internal::OpCode;
using internal::PhysicalOp;
using internal::planAndValidateRules;
using internal::PlannedSCC;
using internal::RelationStorage;
using internal::Row;
using internal::RowView;
using internal::RulePlan;

namespace {

void clearLookup(AtomPlan &atom) {
  atom.lookup_mask = 0;
  for (auto &term : atom.terms)
    term.use_in_lookup = false;
}

std::size_t columnCount(ColumnMask mask) {
  std::size_t count = 0;
  while (mask != 0) {
    count += mask & ColumnMask{1};
    mask >>= 1;
  }
  return count;
}

void prepareAccessPaths(
    std::vector<RulePlan> &rules,
    const std::vector<std::unique_ptr<RelationStorage>> &relations,
    std::size_t max_arrangements_per_relation,
    std::size_t index_memory_budget_bytes) {
  std::vector<std::vector<ColumnMask>> requested(relations.size());
  for (const RulePlan &rule : rules) {
    for (const PhysicalOp &operation : rule.body) {
      if ((operation.code == OpCode::Scan ||
           operation.code == OpCode::AntiLookup) &&
          operation.atom.lookup_mask != 0) {
        requested[operation.atom.relation].push_back(
            operation.atom.lookup_mask);
      }
      if (operation.code == OpCode::Aggregate) {
        for (const AggregateSourceOp &source : operation.aggregate_body) {
          if ((source.code == OpCode::Scan ||
               source.code == OpCode::AntiLookup) &&
              source.atom.lookup_mask != 0) {
            requested[source.atom.relation].push_back(source.atom.lookup_mask);
          }
        }
      }
    }
  }

  std::size_t used_bytes = 0;
  for (const auto &storage : relations)
    used_bytes += storage->indexMemoryBytes();
  for (RelationId relation_id = 0; relation_id < relations.size();
       ++relation_id) {
    auto &masks = requested[relation_id];
    std::sort(masks.begin(), masks.end(), [](ColumnMask lhs, ColumnMask rhs) {
      const std::size_t lhs_columns = columnCount(lhs);
      const std::size_t rhs_columns = columnCount(rhs);
      return lhs_columns != rhs_columns ? lhs_columns > rhs_columns : lhs < rhs;
    });
    masks.erase(std::unique(masks.begin(), masks.end()), masks.end());
    RelationStorage &storage = *relations[relation_id];
    for (ColumnMask mask : masks) {
      if (storage.hasPreparedIndex(mask))
        continue;
      const std::size_t columns = columnCount(mask);
      const std::size_t estimated_bytes =
          storage.rowCount() * columns * sizeof(std::size_t);
      const bool arrangement_limit =
          storage.indexCount() >= max_arrangements_per_relation;
      const bool memory_limit =
          estimated_bytes > index_memory_budget_bytes -
                                std::min(index_memory_budget_bytes, used_bytes);
      if (arrangement_limit || memory_limit)
        continue;
      storage.ensureIndex(mask);
      if (estimated_bytes >
          std::numeric_limits<std::size_t>::max() - used_bytes)
        used_bytes = std::numeric_limits<std::size_t>::max();
      else
        used_bytes += estimated_bytes;
    }
  }

  for (RulePlan &rule : rules) {
    for (PhysicalOp &operation : rule.body) {
      auto prepare_atom = [&](AtomPlan &atom) {
        RelationStorage &storage = *relations[atom.relation];
        if (!storage.hasPreparedIndex(atom.lookup_mask))
          clearLookup(atom);
      };
      if (operation.code == OpCode::Scan ||
          operation.code == OpCode::AntiLookup) {
        prepare_atom(operation.atom);
        if (operation.range)
          relations[operation.atom.relation]->ensureOrderedIndex(
              operation.range->column);
      } else if (operation.code == OpCode::Aggregate) {
        for (AggregateSourceOp &source : operation.aggregate_body) {
          if (source.code == OpCode::Scan || source.code == OpCode::AntiLookup)
            prepare_atom(source.atom);
        }
        if (operation.aggregate_body.size() == 1 &&
            operation.aggregate_body.front().code == OpCode::Scan)
          operation.atom = operation.aggregate_body.front().atom;
      }
    }
  }
}

struct GoalPattern {
  RelationId relation = 0;
  std::vector<std::optional<std::any>> columns;
};

bool samePattern(
    const GoalPattern &lhs, const GoalPattern &rhs,
    const std::vector<std::unique_ptr<RelationStorage>> &relations) {
  if (lhs.relation != rhs.relation || lhs.columns.size() != rhs.columns.size())
    return false;
  const RelationIR &definition = relations[lhs.relation]->definition();
  for (std::size_t column = 0; column < lhs.columns.size(); ++column) {
    if (lhs.columns[column].has_value() != rhs.columns[column].has_value())
      return false;
    if (lhs.columns[column] && !definition.columns[column].equal(
                                   *lhs.columns[column], *rhs.columns[column]))
      return false;
  }
  return true;
}

AtomIR *dependencyAtom(BodyItemIR &item) {
  if (auto *atom = std::get_if<AtomIR>(&item))
    return atom;
  if (auto *negation = std::get_if<NegAtomIR>(&item))
    return &negation->atom;
  if (auto *aggregate = std::get_if<AggregateIR>(&item))
    return &aggregate->source;
  return nullptr;
}

std::vector<RuleIR> specializeForQueryGoals(
    const std::vector<RuleIR> &rules, const std::vector<QueryGoal> &query_goals,
    const std::vector<std::unique_ptr<RelationStorage>> &relations) {
  if (query_goals.empty())
    return rules;

  std::vector<GoalPattern> patterns;
  auto enqueue = [&](GoalPattern pattern) {
    for (const GoalPattern &existing : patterns) {
      if (samePattern(existing, pattern, relations))
        return;
    }
    patterns.push_back(std::move(pattern));
  };
  for (const QueryGoal &goal : query_goals) {
    if (goal.relation >= relations.size())
      throw CompileError("query goal references an unknown relation");
    const RelationIR &definition = relations[goal.relation]->definition();
    GoalPattern pattern;
    pattern.relation = goal.relation;
    pattern.columns.resize(definition.columns.size());
    for (const QueryBinding &binding : goal.bindings) {
      if (binding.column >= pattern.columns.size())
        throw CompileError("query binding references an unknown column");
      if (std::type_index(binding.value.type()) !=
          definition.columns[binding.column].type)
        throw CompileError("query binding has the wrong column type");
      pattern.columns[binding.column] = binding.value;
    }
    enqueue(std::move(pattern));
  }

  std::vector<RuleIR> specialized;
  for (std::size_t pattern_index = 0; pattern_index < patterns.size();
       ++pattern_index) {
    const GoalPattern pattern = patterns[pattern_index];
    const RelationIR &head_definition =
        relations[pattern.relation]->definition();
    for (const RuleIR &input_rule : rules) {
      if (input_rule.head.relation != pattern.relation)
        continue;
      RuleIR rule = input_rule;
      struct Constraint {
        VarId variable = 0;
        ColumnType type;
        std::any value;
      };
      std::vector<Constraint> constraints;
      bool compatible = true;
      for (std::size_t column = 0; column < pattern.columns.size(); ++column) {
        if (!pattern.columns[column])
          continue;
        const TermIR &term = rule.head.args[column];
        if (term.kind == TermIR::Kind::Constant) {
          if (!head_definition.columns[column].equal(
                  term.constant, *pattern.columns[column])) {
            compatible = false;
            break;
          }
          continue;
        }
        if (term.kind != TermIR::Kind::Variable)
          continue;
        auto found = std::find_if(constraints.begin(), constraints.end(),
                                  [&](const Constraint &item) {
                                    return item.variable == term.variable;
                                  });
        if (found != constraints.end()) {
          if (!found->type.equal(found->value, *pattern.columns[column])) {
            compatible = false;
            break;
          }
        } else {
          constraints.push_back({term.variable, head_definition.columns[column],
                                 *pattern.columns[column]});
        }
      }
      if (!compatible)
        continue;

      for (const Constraint &constraint : constraints) {
        FilterIR filter;
        filter.predicate.type = typeid(bool);
        filter.predicate.referenced_vars = {constraint.variable};
        filter.predicate.debug_name = "magic-binding";
        filter.predicate.properties = FunctionProperties::parallel();
        filter.predicate.evaluate =
            [constraint](const Binding &binding) -> std::any {
          if (constraint.variable >= binding.size() ||
              !binding[constraint.variable])
            throw std::logic_error("evaluating an unbound magic-set variable");
          const bool equal =
              constraint.type.equal_value
                  ? constraint.type.equal_value(
                        *binding[constraint.variable],
                        ValueRef::fromAny(constraint.value))
                  : constraint.type.equal(
                        binding[constraint.variable].materialize(),
                        constraint.value);
          return std::any(equal);
        };
        rule.body.push_back(std::move(filter));
      }

      for (BodyItemIR &item : rule.body) {
        auto enqueue_atom = [&](const AtomIR &atom) {
          GoalPattern dependency;
          dependency.relation = atom.relation;
          dependency.columns.resize(atom.args.size());
          for (std::size_t column = 0; column < atom.args.size(); ++column) {
            const TermIR &term = atom.args[column];
            if (term.kind == TermIR::Kind::Constant) {
              dependency.columns[column] = term.constant;
              continue;
            }
            if (term.kind != TermIR::Kind::Variable)
              continue;
            auto found =
                std::find_if(constraints.begin(), constraints.end(),
                             [&](const Constraint &constraint) {
                               return constraint.variable == term.variable;
                             });
            if (found != constraints.end())
              dependency.columns[column] = found->value;
          }
          enqueue(std::move(dependency));
        };
        if (auto *aggregate = std::get_if<AggregateIR>(&item)) {
          if (aggregate->source_body.empty()) {
            enqueue_atom(aggregate->source);
          } else {
            for (AggregateSourceItemIR &source_item : aggregate->source_body) {
              if (auto *atom = std::get_if<AtomIR>(&source_item))
                enqueue_atom(*atom);
              else if (auto *negation = std::get_if<NegAtomIR>(&source_item))
                enqueue_atom(negation->atom);
            }
          }
        } else if (AtomIR *atom = dependencyAtom(item)) {
          enqueue_atom(*atom);
        }
      }
      specialized.push_back(std::move(rule));
    }
  }
  return specialized;
}

} // namespace

struct CompiledProgram::Impl {
  struct CancelledRun {};

  std::shared_ptr<Context::Impl> context;
  ExecutionPlan plan;
  ExecutionStats stats;
  ExecutionProfile profile;
  struct ExecutionContext {
    const ExecutionOptions &options;
  };
  ExecutionContext *execution = nullptr;
  std::size_t compiled_relation_count = 0;
  std::vector<std::size_t> completed_base_versions;
  bool has_completed_run = false;
  std::mutex trace_mutex;
  std::mutex host_callback_mutex;

  struct ExecutionScope {
    Impl &impl;
    std::unique_lock<std::shared_mutex> context_lock;
    explicit ExecutionScope(Impl &impl, ExecutionContext &execution)
        : impl(impl), context_lock(impl.context->execution_mutex) {
      impl.context->running = true;
      impl.execution = &execution;
    }
    ~ExecutionScope() {
      impl.execution = nullptr;
      impl.context->running = false;
    }
  };

  const ExecutionOptions &options() const {
    if (!execution)
      throw std::logic_error("Datalog execution state is not active");
    return execution->options;
  }

  RuleProfile *ruleProfile(std::size_t rule_index) {
    return options().collect_profile ? &profile.rules.at(rule_index) : nullptr;
  }

  void cancellationPoint() const {
    if (options().cancellation.isCancelled())
      throw CancelledRun{};
  }

  RelationStorage &relation(RelationId id) {
    return *context->relations.at(id);
  }

  const RelationStorage &relation(RelationId id) const {
    return *context->relations.at(id);
  }

  struct CandidateCollection {
    std::vector<Row> rows;
    RelationStorage::SetDirectory set_directory;
  };
  using CandidateMap = std::unordered_map<RelationId, CandidateCollection>;
  struct DeltaRows {
    std::vector<std::size_t> set_row_ids;
    std::vector<Row> lattice_rows;

    bool empty() const { return set_row_ids.empty() && lattice_rows.empty(); }

    std::size_t size() const {
      return set_row_ids.size() + lattice_rows.size();
    }

    RowView row(const RelationStorage &storage, std::size_t index) const {
      if (storage.definition().kind == RelationKind::Set)
        return storage.row(set_row_ids[index]);
      return RowView(lattice_rows[index]);
    }

    void append(DeltaRows rows) {
      set_row_ids.insert(set_row_ids.end(),
                         std::make_move_iterator(rows.set_row_ids.begin()),
                         std::make_move_iterator(rows.set_row_ids.end()));
      lattice_rows.insert(lattice_rows.end(),
                          std::make_move_iterator(rows.lattice_rows.begin()),
                          std::make_move_iterator(rows.lattice_rows.end()));
    }
  };
  using DeltaMap = std::unordered_map<RelationId, DeltaRows>;
  using DeltaHashMap = std::unordered_map<
      RelationId, std::unordered_map<std::size_t, std::vector<std::size_t>>>;

  struct DeltaView {
    RelationId relation_id = 0;
    const DeltaRows *delta_rows = nullptr;
    std::optional<KeyView> direct_key;
    std::size_t begin = 0;
    std::size_t end = 0;

    RowView row(const RelationStorage &storage, std::size_t index) const {
      if (delta_rows)
        return delta_rows->row(storage, index);
      return storage.row(index);
    }
  };

  struct EvaluationTask {
    std::size_t rule_index = 0;
    std::optional<std::size_t> delta_item;
    std::optional<DeltaView> delta;
  };

  static void mergeStats(ExecutionStats &destination,
                         const ExecutionStats &source) {
    destination.rule_evaluations += source.rule_evaluations;
    destination.tuples_scanned += source.tuples_scanned;
    destination.index_lookups += source.index_lookups;
    destination.parallel_tasks += source.parallel_tasks;
    destination.parallel_rule_tasks += source.parallel_rule_tasks;
    destination.parallel_merge_tasks += source.parallel_merge_tasks;
    destination.parallel_aggregate_tasks += source.parallel_aggregate_tasks;
    destination.head_derivations += source.head_derivations;
    destination.local_unique_candidates += source.local_unique_candidates;
    destination.global_unique_candidates += source.global_unique_candidates;
    destination.serial_host_rule_evaluations +=
        source.serial_host_rule_evaluations;
    destination.compiled_kernel_evaluations +=
        source.compiled_kernel_evaluations;
    destination.interpreter_rule_evaluations +=
        source.interpreter_rule_evaluations;
    destination.jit_expression_evaluations += source.jit_expression_evaluations;
    destination.incremental_aggregate_groups +=
        source.incremental_aggregate_groups;
    destination.ordered_range_lookups += source.ordered_range_lookups;
  }

  bool emitCandidate(CandidateMap &candidates, RelationId relation_id,
                     Row row) {
    CandidateCollection &collection = candidates[relation_id];
    RelationStorage &storage = relation(relation_id);
    if (storage.definition().kind == RelationKind::Lattice) {
      collection.rows.push_back(std::move(row));
      return true;
    }

    const std::size_t fingerprint = storage.candidateHash(row);
    std::vector<std::size_t> &bucket = collection.set_directory[fingerprint];
    for (std::size_t candidate_id : bucket) {
      if (storage.rowsEqual(collection.rows[candidate_id], row))
        return false;
    }
    bucket.push_back(collection.rows.size());
    collection.rows.push_back(std::move(row));
    return true;
  }

  static void prepareRuleProfile(RuleProfile &rule_profile,
                                 std::size_t operation_count) {
    if (rule_profile.operations.size() != operation_count)
      rule_profile.operations.resize(operation_count);
  }

  static void mergeRuleProfile(RuleProfile &destination,
                               const RuleProfile &source) {
    destination.evaluations += source.evaluations;
    destination.head_candidates += source.head_candidates;
    if (destination.operations.size() < source.operations.size())
      destination.operations.resize(source.operations.size());
    for (std::size_t index = 0; index < source.operations.size(); ++index) {
      OperationProfile &target = destination.operations[index];
      const OperationProfile &input = source.operations[index];
      target.invocations += input.invocations;
      target.candidate_rows += input.candidate_rows;
      target.matched_rows += input.matched_rows;
      target.output_bindings += input.output_bindings;
    }
  }

  void collectStorageStats() {
    for (std::size_t relation_id = 0; relation_id < compiled_relation_count;
         ++relation_id) {
      const auto &storage = context->relations[relation_id];
      stats.total_facts += storage->rowCount();
      stats.index_count += storage->indexCount();
      stats.index_entries += storage->indexEntries();
      stats.index_memory_bytes += storage->indexMemoryBytes();
      stats.tuple_memory_bytes += storage->tupleMemoryBytes();
      stats.uniqueness_memory_bytes += storage->uniquenessMemoryBytes();
      stats.base_memory_bytes += storage->baseMemoryBytes();
    }
  }

  std::ostream &traceStream() {
    if (execution && options().trace_stream)
      return *options().trace_stream;
    return std::cerr;
  }

  void traceRule(std::size_t rule_index,
                 std::optional<std::size_t> delta_item) {
    if (!execution || !options().trace_rule)
      return;
    std::lock_guard<std::mutex> lock(trace_mutex);
    const RulePlan &rule = plan.rules[rule_index];
    traceStream() << "rule " << rule_index << " -> "
                  << relation(rule.head_relation).definition().name;
    if (delta_item)
      traceStream() << " delta-body=" << *delta_item;
    traceStream() << '\n';
  }

  void traceDelta(std::size_t iteration, const DeltaMap &delta) {
    if (!execution || !options().trace_delta)
      return;
    std::lock_guard<std::mutex> lock(trace_mutex);
    traceStream() << "iteration " << iteration << ':';
    for (const auto &[relation_id, rows] : delta) {
      traceStream() << ' ' << relation(relation_id).definition().name << '='
                    << rows.size();
    }
    traceStream() << '\n';
  }

  template <typename Continuation>
  bool matchRow(const AtomPlan &atom, RowView row, Binding &binding,
                Continuation &&continuation) {
    std::array<VarId, KeyView::MAX_COLUMNS> newly_bound{};
    std::size_t newly_bound_count = 0;
    for (std::size_t column = 0; column < atom.terms.size(); ++column) {
      const auto &term = atom.terms[column];
      const ColumnType &type =
          relation(atom.relation).definition().columns[column];
      if (!term.is_variable) {
        const bool equal =
            type.equal_value
                ? type.equal_value(ValueRef::fromAny(term.constant),
                                   row[column])
                : type.equal(term.constant, row[column].materialize());
        if (!equal) {
          for (std::size_t index = 0; index < newly_bound_count; ++index)
            binding[newly_bound[index]].reset();
          return false;
        }
        continue;
      }

      if (binding[term.variable]) {
        const bool equal =
            type.equal_value
                ? type.equal_value(*binding[term.variable], row[column])
                : type.equal(binding[term.variable].materialize(),
                             row[column].materialize());
        if (!equal) {
          for (std::size_t index = 0; index < newly_bound_count; ++index)
            binding[newly_bound[index]].reset();
          return false;
        }
      } else {
        binding[term.variable].bindReference(row[column]);
        newly_bound[newly_bound_count++] = term.variable;
      }
    }

    continuation();
    for (std::size_t index = 0; index < newly_bound_count; ++index)
      binding[newly_bound[index]].reset();
    return true;
  }

  KeyView lookupKey(const AtomPlan &atom, const Binding &binding) {
    KeyView key;
    key.mask = atom.lookup_mask;
    for (std::size_t column = 0; column < atom.terms.size(); ++column) {
      const auto &term = atom.terms[column];
      if (!term.use_in_lookup)
        continue;
      if (!term.is_variable) {
        key.push(term.constant);
      } else {
        if (!binding[term.variable])
          throw std::logic_error(
              "planned Datalog lookup uses an unbound variable");
        key.push(*binding[term.variable]);
      }
    }
    return key;
  }

  std::vector<std::any> evaluateAggregate(const AggregateIR &aggregate,
                                          const AtomPlan &source,
                                          RelationStorage &source_relation,
                                          const KeyView &key, Binding &binding,
                                          Scheduler *aggregate_scheduler,
                                          ExecutionStats &evaluation_stats,
                                          OperationProfile *operation_profile) {
    auto evaluate_serial = [&] {
      AggregateForEach for_each = [&](const AggregateConsumer &consumer) {
        source_relation.forEachMatching(
            key, evaluation_stats, [&](RowView row) {
              cancellationPoint();
              if (operation_profile)
                ++operation_profile->candidate_rows;
              matchRow(source, row, binding, [&] {
                std::any value = aggregate.projection.evaluate(binding);
                consumer(value);
              });
            });
      };
      return aggregate.evaluate(for_each);
    };
    auto debug_evaluate = [&] {
      std::vector<std::any> first =
          aggregate.evaluate([&](const AggregateConsumer &consumer) {
            AggregateForEach for_each = [&](const AggregateConsumer &target) {
              source_relation.forEachMatching(
                  key, evaluation_stats, [&](RowView row) {
                    cancellationPoint();
                    matchRow(source, row, binding, [&] {
                      target(aggregate.projection.evaluate(binding));
                    });
                  });
            };
            for_each(consumer);
          });
      std::vector<std::any> second = evaluate_serial();
      if (first.size() != second.size())
        throw std::logic_error("Datalog aggregate is not deterministic");
      for (std::size_t index = 0; index < first.size(); ++index) {
        if (!debugValuesEqual(aggregate.output_type, first[index],
                              second[index]))
          throw std::logic_error("Datalog aggregate is not deterministic");
      }
      return first;
    };
    if (options().debug_contracts)
      return debug_evaluate();
    if (!aggregate.reducer ||
        !aggregate.reducer->properties.canRunInParallel() ||
        !aggregate_scheduler || aggregate_scheduler->workerCount() <= 1)
      return evaluate_serial();

    const std::size_t matching_rows =
        source_relation.matchingCandidateCount(key, evaluation_stats);
    if (matching_rows == 0) {
      std::any state = aggregate.reducer->make_state();
      return aggregate.reducer->finish(state);
    }
    evaluation_stats.tuples_scanned += matching_rows;

    const std::size_t grain =
        std::max<std::size_t>(1, options().parallel_grain_size);
    const std::size_t available_chunks = (matching_rows + grain - 1) / grain;
    const std::size_t chunk_count =
        std::min(aggregate_scheduler->workerCount(), available_chunks);
    if (chunk_count <= 1)
      return evaluate_serial();
    if (operation_profile)
      operation_profile->candidate_rows += matching_rows;

    std::vector<ReducerIR> reducers(chunk_count, *aggregate.reducer);
    std::vector<std::any> states;
    states.reserve(chunk_count);
    for (const ReducerIR &reducer : reducers)
      states.push_back(reducer.make_state());

    aggregate_scheduler->parallelFor(chunk_count, [&](std::size_t chunk) {
      Binding chunk_binding = binding;
      const std::size_t begin = matching_rows * chunk / chunk_count;
      const std::size_t end = matching_rows * (chunk + 1) / chunk_count;
      source_relation.forEachMatchingSlice(key, begin, end, [&](RowView row) {
        cancellationPoint();
        matchRow(source, row, chunk_binding, [&] {
          reducers[chunk].add(states[chunk],
                              aggregate.projection.evaluate(chunk_binding));
        });
      });
    });
    evaluation_stats.parallel_tasks += chunk_count;
    evaluation_stats.parallel_aggregate_tasks += chunk_count;

    for (std::size_t chunk = 1; chunk < chunk_count; ++chunk)
      reducers[0].merge(states[0], states[chunk]);
    return reducers[0].finish(states[0]);
  }

  std::vector<std::any>
  evaluateAggregateSubplan(const PhysicalOp &operation, Binding &binding,
                           ExecutionStats &evaluation_stats,
                           OperationProfile *operation_profile) {
    const AggregateIR &aggregate = operation.aggregate;
    AggregateForEach for_each = [&](const AggregateConsumer &consumer) {
      std::function<void(std::size_t)> evaluate_source =
          [&](std::size_t source_index) {
            cancellationPoint();
            if (source_index == operation.aggregate_body.size()) {
              consumer(aggregate.projection.evaluate(binding));
              return;
            }
            const AggregateSourceOp &source =
                operation.aggregate_body[source_index];
            if (source.code == OpCode::Filter) {
              if (std::any_cast<bool>(source.filter.evaluate(binding)))
                evaluate_source(source_index + 1);
              return;
            }
            if (source.code == OpCode::AntiLookup) {
              bool found = false;
              const KeyView key = lookupKey(source.atom, binding);
              relation(source.atom.relation)
                  .forEachMatching(key, evaluation_stats, [&](RowView row) {
                    if (found)
                      return;
                    if (operation_profile)
                      ++operation_profile->candidate_rows;
                    matchRow(source.atom, row, binding, [&] { found = true; });
                  });
              if (!found)
                evaluate_source(source_index + 1);
              return;
            }
            const KeyView key = lookupKey(source.atom, binding);
            relation(source.atom.relation)
                .forEachMatching(key, evaluation_stats, [&](RowView row) {
                  if (operation_profile)
                    ++operation_profile->candidate_rows;
                  matchRow(source.atom, row, binding,
                           [&] { evaluate_source(source_index + 1); });
                });
          };
      evaluate_source(0);
    };
    return aggregate.evaluate(for_each);
  }

  bool isCurrentDeltaRow(RelationId relation_id, RowView row,
                         const DeltaMap &delta,
                         const DeltaHashMap &delta_hashes) {
    auto rows = delta.find(relation_id);
    auto hashes = delta_hashes.find(relation_id);
    if (rows == delta.end() || hashes == delta_hashes.end())
      return false;
    auto bucket = hashes->second.find(relation(relation_id).candidateHash(row));
    if (bucket == hashes->second.end())
      return false;
    for (std::size_t row_index : bucket->second) {
      RowView delta_row = rows->second.row(relation(relation_id), row_index);
      if (relation(relation_id).rowsEqual(row, delta_row))
        return true;
    }
    return false;
  }

  void emitHead(const RulePlan &rule, Binding &binding,
                CandidateMap &candidates, ExecutionStats &evaluation_stats,
                RuleProfile *evaluation_profile) {
    Row result;
    result.reserve(rule.head.size());
    const RelationIR &head_definition =
        relation(rule.head_relation).definition();
    for (std::size_t column = 0; column < rule.head.size(); ++column) {
      const HeadTermPlan &term = rule.head[column];
      if (term.kind == HeadTermPlan::Kind::Variable)
        result.push_back(binding[term.variable].materialize());
      else if (term.kind == HeadTermPlan::Kind::Constant)
        result.push_back(term.constant);
      else {
        if (term.expression.compiled_kernel)
          ++evaluation_stats.jit_expression_evaluations;
        std::any value = term.expression.evaluate(binding);
        if (options().debug_contracts) {
          std::any replay = term.expression.evaluate(binding);
          if (!head_definition.columns[column].equal(value, replay))
            throw std::logic_error(
                "Datalog head expression is not deterministic");
        }
        result.push_back(std::move(value));
      }
    }
    ++evaluation_stats.head_derivations;
    if (emitCandidate(candidates, rule.head_relation, std::move(result)))
      ++evaluation_stats.local_unique_candidates;
    if (evaluation_profile)
      ++evaluation_profile->head_candidates;
  }

  void evaluateProjectionKernel(const RulePlan &rule, Binding &binding,
                                std::optional<std::size_t> delta_item,
                                std::optional<DeltaView> delta,
                                CandidateMap &candidates,
                                ExecutionStats &evaluation_stats,
                                RuleProfile *evaluation_profile) {
    ++evaluation_stats.compiled_kernel_evaluations;
    const AtomPlan &atom = rule.body.front().atom;
    OperationProfile *operation_profile =
        evaluation_profile ? &evaluation_profile->operations.front() : nullptr;
    if (operation_profile)
      ++operation_profile->invocations;
    auto process = [&](RowView row) {
      cancellationPoint();
      if (operation_profile)
        ++operation_profile->candidate_rows;
      if (matchRow(atom, row, binding,
                   [&] {
                     if (operation_profile)
                       ++operation_profile->output_bindings;
                     emitHead(rule, binding, candidates, evaluation_stats,
                              evaluation_profile);
                   }) &&
          operation_profile) {
        ++operation_profile->matched_rows;
      }
    };

    if (delta_item && *delta_item == 0) {
      if (delta->direct_key) {
        relation(delta->relation_id)
            .forEachMatchingSlice(*delta->direct_key, delta->begin, delta->end,
                                  [&](RowView row) {
                                    ++evaluation_stats.tuples_scanned;
                                    process(row);
                                  });
      } else {
        for (std::size_t row_index = delta->begin; row_index < delta->end;
             ++row_index) {
          ++evaluation_stats.tuples_scanned;
          process(delta->row(relation(delta->relation_id), row_index));
        }
      }
      return;
    }

    const KeyView key = lookupKey(atom, binding);
    relation(atom.relation).forEachMatching(key, evaluation_stats, process);
  }

  void evaluateRule(
      std::size_t rule_index, Binding &binding,
      std::optional<std::size_t> delta_item, std::optional<DeltaView> delta,
      CandidateMap &candidates, ExecutionStats &evaluation_stats,
      Scheduler *aggregate_scheduler = nullptr,
      const std::unordered_set<RelationId> *recursive_relations = nullptr,
      const DeltaMap *current_delta = nullptr,
      const DeltaHashMap *delta_hashes = nullptr,
      RuleProfile *evaluation_profile = nullptr) {
    const RulePlan &rule = plan.rules[rule_index];
    std::unique_lock<std::mutex> host_lock(host_callback_mutex,
                                           std::defer_lock);
    if (!rule.parallel_safe) {
      host_lock.lock();
      ++evaluation_stats.serial_host_rule_evaluations;
    }
    ++evaluation_stats.rule_evaluations;
    if (evaluation_profile) {
      prepareRuleProfile(*evaluation_profile, rule.body.size());
      ++evaluation_profile->evaluations;
    }
    traceRule(rule_index, delta_item);
    if (rule.kernel_kind == RulePlan::KernelKind::Projection) {
      evaluateProjectionKernel(rule, binding, delta_item, delta, candidates,
                               evaluation_stats, evaluation_profile);
      return;
    }
    ++evaluation_stats.interpreter_rule_evaluations;
    std::function<void(std::size_t)> evaluate_item =
        [&](std::size_t item_index) {
          cancellationPoint();
          if (item_index == rule.body.size()) {
            emitHead(rule, binding, candidates, evaluation_stats,
                     evaluation_profile);
            return;
          }

          const PhysicalOp &item = rule.body[item_index];
          OperationProfile *operation_profile =
              evaluation_profile ? &evaluation_profile->operations[item_index]
                                 : nullptr;
          if (operation_profile)
            ++operation_profile->invocations;
          if (item.code == OpCode::Filter) {
            if (operation_profile)
              ++operation_profile->candidate_rows;
            if (item.filter.compiled_kernel)
              ++evaluation_stats.jit_expression_evaluations;
            const bool filter_result =
                std::any_cast<bool>(item.filter.evaluate(binding));
            if (options().debug_contracts &&
                filter_result !=
                    std::any_cast<bool>(item.filter.evaluate(binding))) {
              throw std::logic_error("Datalog filter is not deterministic");
            }
            if (filter_result) {
              if (operation_profile) {
                ++operation_profile->matched_rows;
                ++operation_profile->output_bindings;
              }
              evaluate_item(item_index + 1);
            }
            return;
          }

          if (item.code == OpCode::AntiLookup) {
            const AtomPlan &atom = item.atom;
            bool found = false;
            auto test_row = [&](RowView row) {
              cancellationPoint();
              if (found)
                return;
              if (operation_profile)
                ++operation_profile->candidate_rows;
              if (matchRow(atom, row, binding, [&] { found = true; }) &&
                  operation_profile)
                ++operation_profile->matched_rows;
            };
            const KeyView key = lookupKey(atom, binding);
            relation(atom.relation)
                .forEachMatching(key, evaluation_stats, test_row);
            if (!found) {
              if (operation_profile)
                ++operation_profile->output_bindings;
              evaluate_item(item_index + 1);
            }
            return;
          }

          if (item.code == OpCode::Aggregate) {
            const AggregateIR &aggregate = item.aggregate;
            std::vector<std::any> results;
            if (item.aggregate_body.size() == 1 &&
                item.aggregate_body.front().code == OpCode::Scan) {
              const AtomPlan &source = item.aggregate_body.front().atom;
              const KeyView key = lookupKey(source, binding);
              results = evaluateAggregate(
                  aggregate, source, relation(source.relation), key, binding,
                  aggregate_scheduler, evaluation_stats, operation_profile);
            } else {
              results = evaluateAggregateSubplan(
                  item, binding, evaluation_stats, operation_profile);
            }
            if (operation_profile) {
              operation_profile->matched_rows += results.size();
            }
            for (std::any &result : results) {
              binding[aggregate.output_var] = std::move(result);
              if (operation_profile)
                ++operation_profile->output_bindings;
              evaluate_item(item_index + 1);
              binding[aggregate.output_var].reset();
            }
            return;
          }

          const AtomPlan &atom = item.atom;
          auto continue_with = [&](RowView row) {
            cancellationPoint();
            if (operation_profile)
              ++operation_profile->candidate_rows;
            if (matchRow(atom, row, binding,
                         [&] {
                           if (operation_profile)
                             ++operation_profile->output_bindings;
                           evaluate_item(item_index + 1);
                         }) &&
                operation_profile)
              ++operation_profile->matched_rows;
          };

          if (delta_item && *delta_item == item_index) {
            if (delta->direct_key) {
              relation(delta->relation_id)
                  .forEachMatchingSlice(*delta->direct_key, delta->begin,
                                        delta->end, [&](RowView row) {
                                          ++evaluation_stats.tuples_scanned;
                                          continue_with(row);
                                        });
              return;
            }
            for (std::size_t row_index = delta->begin; row_index < delta->end;
                 ++row_index) {
              ++evaluation_stats.tuples_scanned;
              continue_with(
                  delta->row(relation(delta->relation_id), row_index));
            }
            return;
          }

          const KeyView key = lookupKey(atom, binding);
          auto continue_with_old_total = [&](RowView row) {
            if (delta_item && item_index < *delta_item && recursive_relations &&
                current_delta && delta_hashes &&
                recursive_relations->count(atom.relation) != 0 &&
                isCurrentDeltaRow(atom.relation, row, *current_delta,
                                  *delta_hashes))
              return;
            continue_with(row);
          };
          if (item.range) {
            relation(atom.relation)
                .forEachRange(*item.range, evaluation_stats,
                              continue_with_old_total);
          } else {
            relation(atom.relation)
                .forEachMatching(key, evaluation_stats,
                                 continue_with_old_total);
          }
        };

    evaluate_item(0);
  }

  void coalesceLocalLattices(CandidateMap &candidates) {
    for (auto &[relation_id, collection] : candidates) {
      RelationStorage &storage = relation(relation_id);
      if (storage.definition().kind == RelationKind::Lattice)
        collection.rows = storage.coalesce(std::move(collection.rows));
    }
  }

  std::vector<Row> coalesceCandidates(RelationStorage &storage,
                                      std::vector<Row> rows,
                                      Scheduler &scheduler) {
    const std::size_t grain =
        std::max<std::size_t>(1, options().parallel_grain_size);
    const std::size_t shard_count =
        storage.parallelSafe() ? std::min(scheduler.workerCount(),
                                          (rows.size() + grain - 1) / grain)
                               : 1;
    if (shard_count <= 1)
      return storage.coalesce(std::move(rows));

    std::vector<std::vector<Row>> shards(shard_count);
    for (Row &row : rows) {
      const std::size_t shard = storage.candidateHash(row) % shard_count;
      shards[shard].push_back(std::move(row));
    }
    scheduler.parallelFor(shard_count, [&](std::size_t shard) {
      shards[shard] = storage.coalesce(std::move(shards[shard]));
    });
    stats.parallel_tasks += shard_count;
    stats.parallel_merge_tasks += shard_count;

    std::vector<Row> result;
    for (std::vector<Row> &shard : shards) {
      result.insert(result.end(), std::make_move_iterator(shard.begin()),
                    std::make_move_iterator(shard.end()));
    }
    return result;
  }

  DeltaMap mergeCandidates(CandidateMap candidates, Scheduler &scheduler) {
    cancellationPoint();
    DeltaMap inserted;
    for (auto &[relation_id, collection] : candidates) {
      cancellationPoint();
      RelationStorage &storage = relation(relation_id);
      std::vector<Row> coalesced =
          coalesceCandidates(storage, std::move(collection.rows), scheduler);
      stats.global_unique_candidates += coalesced.size();
      RelationStorage::BatchMergeResult merged = storage.mergeDerivedCoalesced(
          std::move(coalesced), scheduler, options().parallel_grain_size,
          options().debug_contracts);
      stats.parallel_tasks += merged.parallel_tasks;
      stats.parallel_merge_tasks += merged.parallel_tasks;
      stats.inserted_facts += merged.changedCount();
      if (!merged.empty()) {
        DeltaRows delta;
        delta.set_row_ids = std::move(merged.changed_row_ids);
        delta.lattice_rows = std::move(merged.changed_lattice_rows);
        inserted.emplace(relation_id, std::move(delta));
      }
    }
    cancellationPoint();
    return inserted;
  }

  static bool hasRows(const DeltaMap &delta) {
    for (const auto &[relation, rows] : delta) {
      (void)relation;
      if (!rows.empty())
        return true;
    }
    return false;
  }

  static std::size_t deltaSize(const DeltaMap &delta) {
    std::size_t size = 0;
    for (const auto &[relation, rows] : delta) {
      (void)relation;
      size += rows.size();
    }
    return size;
  }

  bool monotoneAggregateReadsDelta(
      const RulePlan &rule, const DeltaMap &delta,
      const std::unordered_set<RelationId> *relations = nullptr) const {
    for (const PhysicalOp &operation : rule.body) {
      if (operation.code != OpCode::Aggregate || !operation.aggregate.monotone)
        continue;
      for (const AggregateSourceOp &source : operation.aggregate_body) {
        if (source.code != OpCode::Scan ||
            (relations && relations->count(source.atom.relation) == 0))
          continue;
        auto found = delta.find(source.atom.relation);
        if (found != delta.end() && !found->second.empty())
          return true;
      }
    }
    return false;
  }

  static const char *operationName(OpCode code) {
    switch (code) {
    case OpCode::Scan:
      return "Scan";
    case OpCode::Filter:
      return "Filter";
    case OpCode::AntiLookup:
      return "AntiLookup";
    case OpCode::Aggregate:
      return "Aggregate";
    }
    return "Unknown";
  }

  static bool debugValuesEqual(std::type_index type, const std::any &lhs,
                               const std::any &rhs) {
    if (type == typeid(int))
      return std::any_cast<int>(lhs) == std::any_cast<int>(rhs);
    if (type == typeid(std::int64_t))
      return std::any_cast<std::int64_t>(lhs) ==
             std::any_cast<std::int64_t>(rhs);
    if (type == typeid(std::uint64_t))
      return std::any_cast<std::uint64_t>(lhs) ==
             std::any_cast<std::uint64_t>(rhs);
    if (type == typeid(double))
      return std::any_cast<double>(lhs) == std::any_cast<double>(rhs);
    if (type == typeid(bool))
      return std::any_cast<bool>(lhs) == std::any_cast<bool>(rhs);
    if (type == typeid(std::string))
      return std::any_cast<const std::string &>(lhs) ==
             std::any_cast<const std::string &>(rhs);
    return true;
  }

  std::string explain(ExplainMode mode) const {
    std::ostringstream output;
    if (mode == ExplainMode::Analyze && !profile.collected)
      output << "profile: not collected\n";
    for (std::size_t scc_index = 0; scc_index < plan.sccs.size(); ++scc_index) {
      const PlannedSCC &scc = plan.sccs[scc_index];
      output << "SCC " << scc_index << " [stratum=" << scc.stratum
             << ", recursive=" << (scc.recursive ? "yes" : "no") << "]\n";
      for (std::size_t rule_index : scc.rules) {
        const RulePlan &rule = plan.rules[rule_index];
        output << "  rule " << rule_index << " -> "
               << relation(rule.head_relation).definition().name << '\n';
        for (std::size_t item_index = 0; item_index < rule.body.size();
             ++item_index) {
          const PhysicalOp &operation = rule.body[item_index];
          output << "    " << item_index << ' '
                 << operationName(operation.code);
          if (operation.code == OpCode::Scan ||
              operation.code == OpCode::AntiLookup ||
              operation.code == OpCode::Aggregate) {
            output << ' ' << relation(operation.atom.relation).definition().name
                   << " mask=" << operation.atom.lookup_mask;
          }
          output << " estimate[input=" << operation.estimated_input_rows
                 << ", lookup=" << operation.estimated_lookup_rows
                 << ", output=" << operation.estimated_output_rows << ']';
          if (mode == ExplainMode::Analyze && profile.collected &&
              rule_index < profile.rules.size() &&
              item_index < profile.rules[rule_index].operations.size()) {
            const OperationProfile &actual =
                profile.rules[rule_index].operations[item_index];
            output << " actual[invocations=" << actual.invocations
                   << ", candidates=" << actual.candidate_rows
                   << ", matched=" << actual.matched_rows
                   << ", output=" << actual.output_bindings << ']';
          }
          output << '\n';
        }
        if (mode == ExplainMode::Analyze && profile.collected &&
            rule_index < profile.rules.size()) {
          const RuleProfile &actual = profile.rules[rule_index];
          output << "    head candidates=" << actual.head_candidates
                 << " evaluations=" << actual.evaluations << '\n';
        }
      }
    }
    return output.str();
  }

  void runNonRecursive(const PlannedSCC &scc, Binding &binding,
                       Scheduler &scheduler) {
    std::vector<EvaluationTask> tasks;
    for (std::size_t rule_index : scc.rules) {
      const RulePlan &rule = plan.rules[rule_index];
      std::optional<std::size_t> driver_item;
      for (std::size_t item_index = 0; item_index < rule.body.size();
           ++item_index) {
        if (rule.body[item_index].code == OpCode::Scan) {
          driver_item = item_index;
          break;
        }
      }

      if (!driver_item) {
        tasks.push_back({rule_index, std::nullopt, std::nullopt});
        continue;
      }

      const AtomPlan &driver = rule.body[*driver_item].atom;
      if (rule.body[*driver_item].range) {
        tasks.push_back({rule_index, std::nullopt, std::nullopt});
        continue;
      }
      const bool can_use_driver_lookup = std::none_of(
          driver.terms.begin(), driver.terms.end(), [](const auto &term) {
            return term.use_in_lookup && term.is_variable;
          });
      if (can_use_driver_lookup) {
        const KeyView key = lookupKey(driver, binding);
        const std::size_t matching_rows =
            relation(driver.relation).matchingCandidateCount(key, stats);
        if (matching_rows == 0)
          continue;
        const std::size_t grain =
            scheduler.workerCount() > 1
                ? std::max<std::size_t>(1, options().parallel_grain_size)
                : matching_rows;
        for (std::size_t begin = 0; begin < matching_rows; begin += grain) {
          tasks.push_back({rule_index, driver_item,
                           DeltaView{driver.relation, nullptr, key, begin,
                                     std::min(matching_rows, begin + grain)}});
        }
        continue;
      }

      const std::size_t row_count = relation(driver.relation).rowCount();
      if (row_count == 0)
        continue;
      const std::size_t grain =
          scheduler.workerCount() > 1
              ? std::max<std::size_t>(1, options().parallel_grain_size)
              : row_count;
      for (std::size_t begin = 0; begin < row_count; begin += grain) {
        tasks.push_back({rule_index, driver_item,
                         DeltaView{driver.relation, nullptr, std::nullopt,
                                   begin, std::min(row_count, begin + grain)}});
      }
    }

    if (tasks.empty())
      return;

    if (tasks.size() == 1) {
      CandidateMap candidates;
      const EvaluationTask &task = tasks.front();
      evaluateRule(task.rule_index, binding, task.delta_item, task.delta,
                   candidates, stats, &scheduler, nullptr, nullptr, nullptr,
                   ruleProfile(task.rule_index));
      mergeCandidates(std::move(candidates), scheduler);
      return;
    }

    std::vector<CandidateMap> task_candidates(tasks.size());
    std::vector<ExecutionStats> task_stats(tasks.size());
    std::vector<RuleProfile> task_profiles(
        options().collect_profile ? tasks.size() : 0);
    const bool parallel_batch = scheduler.workerCount() > 1;
    const bool coalesce_locally = tasks.size() >= scheduler.workerCount();
    scheduler.parallelFor(tasks.size(), [&](std::size_t task_index) {
      Binding task_binding(plan.variable_count);
      const EvaluationTask &task = tasks[task_index];
      evaluateRule(task.rule_index, task_binding, task.delta_item, task.delta,
                   task_candidates[task_index], task_stats[task_index], nullptr,
                   nullptr, nullptr, nullptr,
                   options().collect_profile ? &task_profiles[task_index]
                                             : nullptr);
      if (coalesce_locally)
        coalesceLocalLattices(task_candidates[task_index]);
      if (parallel_batch)
        task_stats[task_index].parallel_tasks =
            task_stats[task_index].parallel_rule_tasks = 1;
    });

    CandidateMap candidates;
    for (std::size_t task_index = 0; task_index < tasks.size(); ++task_index) {
      mergeStats(stats, task_stats[task_index]);
      if (options().collect_profile) {
        mergeRuleProfile(profile.rules[tasks[task_index].rule_index],
                         task_profiles[task_index]);
      }
      for (auto &[relation_id, collection] : task_candidates[task_index]) {
        auto &destination = candidates[relation_id].rows;
        destination.insert(destination.end(),
                           std::make_move_iterator(collection.rows.begin()),
                           std::make_move_iterator(collection.rows.end()));
      }
    }
    mergeCandidates(std::move(candidates), scheduler);
  }

  void runRecursive(const PlannedSCC &scc, Binding &binding,
                    Scheduler &scheduler) {
    DeltaMap current_delta;
    for (RelationId relation_id : scc.relations) {
      RelationStorage &storage = relation(relation_id);
      DeltaRows &delta = current_delta[relation_id];
      if (storage.definition().kind == RelationKind::Set) {
        delta.set_row_ids.reserve(storage.rowCount());
        for (std::size_t row_index = 0; row_index < storage.rowCount();
             ++row_index)
          delta.set_row_ids.push_back(row_index);
      } else {
        delta.lattice_rows = storage.materializeRows();
      }
    }

    CandidateMap base_candidates;
    for (std::size_t rule_index : scc.rules) {
      const RulePlan &rule = plan.rules[rule_index];
      bool has_recursive_atom = false;
      for (const PhysicalOp &item : rule.body) {
        if (item.code == OpCode::Scan)
          has_recursive_atom = has_recursive_atom ||
                               scc.relation_set.count(item.atom.relation) != 0;
      }
      has_recursive_atom =
          has_recursive_atom ||
          monotoneAggregateReadsDelta(rule, current_delta, &scc.relation_set);
      if (!has_recursive_atom)
        evaluateRule(rule_index, binding, std::nullopt, std::nullopt,
                     base_candidates, stats, &scheduler, nullptr, nullptr,
                     nullptr, ruleProfile(rule_index));
    }
    DeltaMap base_delta =
        mergeCandidates(std::move(base_candidates), scheduler);
    for (auto &[relation_id, rows] : base_delta) {
      auto &destination = current_delta[relation_id];
      destination.append(std::move(rows));
    }

    std::size_t iteration = 0;
    while (hasRows(current_delta)) {
      cancellationPoint();
      stats.peak_delta = std::max(stats.peak_delta, deltaSize(current_delta));
      traceDelta(iteration, current_delta);
      ++stats.fixpoint_iterations;
      ++iteration;
      DeltaHashMap delta_hashes;
      for (const auto &[relation_id, rows] : current_delta) {
        auto &hashes = delta_hashes[relation_id];
        hashes.reserve(rows.size());
        for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
          cancellationPoint();
          hashes[relation(relation_id)
                     .candidateHash(rows.row(relation(relation_id), row_index))]
              .push_back(row_index);
        }
      }
      std::vector<EvaluationTask> tasks;
      for (std::size_t rule_index : scc.rules) {
        const RulePlan &rule = plan.rules[rule_index];
        for (std::size_t item_index = 0; item_index < rule.body.size();
             ++item_index) {
          const PhysicalOp &item = rule.body[item_index];
          if (item.code != OpCode::Scan ||
              !scc.relation_set.count(item.atom.relation))
            continue;
          auto delta_it = current_delta.find(item.atom.relation);
          if (delta_it == current_delta.end() || delta_it->second.empty())
            continue;
          const std::size_t grain =
              scheduler.workerCount() > 1
                  ? std::max<std::size_t>(1, options().parallel_grain_size)
                  : delta_it->second.size();
          for (std::size_t begin = 0; begin < delta_it->second.size();
               begin += grain) {
            tasks.push_back(
                {rule_index, item_index,
                 DeltaView{item.atom.relation, &delta_it->second, std::nullopt,
                           begin,
                           std::min(delta_it->second.size(), begin + grain)}});
          }
        }
        if (monotoneAggregateReadsDelta(rule, current_delta,
                                        &scc.relation_set)) {
          tasks.push_back({rule_index, std::nullopt, std::nullopt});
        }
      }

      std::vector<CandidateMap> task_candidates(tasks.size());
      std::vector<ExecutionStats> task_stats(tasks.size());
      std::vector<RuleProfile> task_profiles(
          options().collect_profile ? tasks.size() : 0);
      const bool parallel_batch =
          scheduler.workerCount() > 1 && tasks.size() > 1;
      const bool coalesce_locally = tasks.size() >= scheduler.workerCount();
      scheduler.parallelFor(tasks.size(), [&](std::size_t task_index) {
        Binding task_binding(plan.variable_count);
        const EvaluationTask &task = tasks[task_index];
        evaluateRule(task.rule_index, task_binding, task.delta_item, task.delta,
                     task_candidates[task_index], task_stats[task_index],
                     nullptr, &scc.relation_set, &current_delta, &delta_hashes,
                     options().collect_profile ? &task_profiles[task_index]
                                               : nullptr);
        if (coalesce_locally)
          coalesceLocalLattices(task_candidates[task_index]);
        if (parallel_batch)
          task_stats[task_index].parallel_tasks =
              task_stats[task_index].parallel_rule_tasks = 1;
      });

      CandidateMap next_candidates;
      for (std::size_t task_index = 0; task_index < tasks.size();
           ++task_index) {
        mergeStats(stats, task_stats[task_index]);
        if (options().collect_profile) {
          mergeRuleProfile(profile.rules[tasks[task_index].rule_index],
                           task_profiles[task_index]);
        }
        for (auto &[relation_id, collection] : task_candidates[task_index]) {
          auto &destination = next_candidates[relation_id].rows;
          destination.insert(destination.end(),
                             std::make_move_iterator(collection.rows.begin()),
                             std::make_move_iterator(collection.rows.end()));
        }
      }
      current_delta = mergeCandidates(std::move(next_candidates), scheduler);
    }
    traceDelta(iteration, current_delta);
  }

  DeltaMap evaluateDeltaInputs(const PlannedSCC &scc,
                               const DeltaMap &input_delta,
                               Scheduler &scheduler) {
    DeltaHashMap delta_hashes;
    std::unordered_set<RelationId> delta_relations;
    for (const auto &[relation_id, rows] : input_delta) {
      if (rows.empty())
        continue;
      delta_relations.insert(relation_id);
      auto &hashes = delta_hashes[relation_id];
      hashes.reserve(rows.size());
      for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
        cancellationPoint();
        hashes[relation(relation_id)
                   .candidateHash(rows.row(relation(relation_id), row_index))]
            .push_back(row_index);
      }
    }

    std::vector<EvaluationTask> tasks;
    for (std::size_t rule_index : scc.rules) {
      const RulePlan &rule = plan.rules[rule_index];
      for (std::size_t item_index = 0; item_index < rule.body.size();
           ++item_index) {
        const PhysicalOp &item = rule.body[item_index];
        if (item.code != OpCode::Scan)
          continue;
        auto delta_it = input_delta.find(item.atom.relation);
        if (delta_it == input_delta.end() || delta_it->second.empty())
          continue;
        const std::size_t grain =
            scheduler.workerCount() > 1
                ? std::max<std::size_t>(1, options().parallel_grain_size)
                : delta_it->second.size();
        for (std::size_t begin = 0; begin < delta_it->second.size();
             begin += grain) {
          tasks.push_back(
              {rule_index, item_index,
               DeltaView{item.atom.relation, &delta_it->second, std::nullopt,
                         begin,
                         std::min(delta_it->second.size(), begin + grain)}});
        }
      }
      if (monotoneAggregateReadsDelta(rule, input_delta))
        tasks.push_back({rule_index, std::nullopt, std::nullopt});
    }
    if (tasks.empty())
      return {};

    std::vector<CandidateMap> task_candidates(tasks.size());
    std::vector<ExecutionStats> task_stats(tasks.size());
    std::vector<RuleProfile> task_profiles(
        options().collect_profile ? tasks.size() : 0);
    const bool parallel_batch = scheduler.workerCount() > 1 && tasks.size() > 1;
    scheduler.parallelFor(tasks.size(), [&](std::size_t task_index) {
      Binding task_binding(plan.variable_count);
      const EvaluationTask &task = tasks[task_index];
      evaluateRule(task.rule_index, task_binding, task.delta_item, task.delta,
                   task_candidates[task_index], task_stats[task_index], nullptr,
                   &delta_relations, &input_delta, &delta_hashes,
                   options().collect_profile ? &task_profiles[task_index]
                                             : nullptr);
      coalesceLocalLattices(task_candidates[task_index]);
      if (parallel_batch)
        task_stats[task_index].parallel_tasks =
            task_stats[task_index].parallel_rule_tasks = 1;
    });

    CandidateMap candidates;
    for (std::size_t task_index = 0; task_index < tasks.size(); ++task_index) {
      mergeStats(stats, task_stats[task_index]);
      if (options().collect_profile) {
        mergeRuleProfile(profile.rules[tasks[task_index].rule_index],
                         task_profiles[task_index]);
      }
      for (auto &[relation_id, collection] : task_candidates[task_index]) {
        auto &destination = candidates[relation_id].rows;
        destination.insert(destination.end(),
                           std::make_move_iterator(collection.rows.begin()),
                           std::make_move_iterator(collection.rows.end()));
      }
    }
    return mergeCandidates(std::move(candidates), scheduler);
  }

  void runIncremental(const PlannedSCC &scc, DeltaMap &run_delta,
                      Scheduler &scheduler) {
    DeltaMap current_delta = evaluateDeltaInputs(scc, run_delta, scheduler);
    for (auto &[relation_id, rows] : current_delta)
      run_delta[relation_id].append(rows);

    if (!scc.recursive)
      return;

    std::size_t iteration = 0;
    while (hasRows(current_delta)) {
      cancellationPoint();
      stats.peak_delta = std::max(stats.peak_delta, deltaSize(current_delta));
      traceDelta(iteration++, current_delta);
      ++stats.fixpoint_iterations;
      DeltaMap next_delta = evaluateDeltaInputs(scc, current_delta, scheduler);
      for (auto &[relation_id, rows] : next_delta)
        run_delta[relation_id].append(rows);
      current_delta = std::move(next_delta);
    }
    traceDelta(iteration, current_delta);
  }

  bool canIncrementalAggregate(const PlannedSCC &scc) const {
    if (scc.recursive || scc.rules.empty())
      return false;
    for (std::size_t rule_index : scc.rules) {
      const RulePlan &rule = plan.rules[rule_index];
      if (relation(rule.head_relation).definition().kind != RelationKind::Set)
        return false;
      const PhysicalOp *aggregate = nullptr;
      for (const PhysicalOp &operation : rule.body) {
        if (operation.code == OpCode::AntiLookup)
          return false;
        if (operation.code == OpCode::Aggregate) {
          if (aggregate || !operation.aggregate.reducer ||
              operation.aggregate.monotone)
            return false;
          aggregate = &operation;
        }
      }
      if (!aggregate)
        return false;
      bool output_in_head = false;
      for (const HeadTermPlan &term : rule.head) {
        if (term.kind == HeadTermPlan::Kind::Expression)
          return false;
        output_in_head = output_in_head ||
                         (term.kind == HeadTermPlan::Kind::Variable &&
                          term.variable == aggregate->aggregate.output_var);
      }
      if (!output_in_head)
        return false;
    }
    return true;
  }

  bool runIncrementalAggregate(const PlannedSCC &scc, DeltaMap &run_delta,
                               Scheduler &scheduler) {
    for (std::size_t rule_index : scc.rules) {
      const RulePlan &rule = plan.rules[rule_index];
      const PhysicalOp *aggregate_operation = nullptr;
      for (const PhysicalOp &operation : rule.body) {
        if (operation.code == OpCode::Aggregate)
          aggregate_operation = &operation;
      }
      if (!aggregate_operation)
        return false;
      const VarId output_variable = aggregate_operation->aggregate.output_var;
      ColumnMask group_mask = 0;
      std::vector<VarId> group_variables;
      for (std::size_t column = 0; column < rule.head.size(); ++column) {
        const HeadTermPlan &term = rule.head[column];
        if (term.kind == HeadTermPlan::Kind::Variable &&
            term.variable == output_variable)
          continue;
        group_mask |= ColumnMask{1} << column;
        if (term.kind == HeadTermPlan::Kind::Variable &&
            std::find(group_variables.begin(), group_variables.end(),
                      term.variable) == group_variables.end())
          group_variables.push_back(term.variable);
      }

      struct Group {
        Binding binding;
        std::vector<std::any> key;
      };
      std::vector<Group> groups;
      const RelationIR &head_definition =
          relation(rule.head_relation).definition();
      auto same_group = [&](const std::vector<std::any> &lhs,
                            const std::vector<std::any> &rhs) {
        std::size_t key_column = 0;
        for (std::size_t column = 0; column < rule.head.size(); ++column) {
          if ((group_mask & (ColumnMask{1} << column)) == 0)
            continue;
          if (!head_definition.columns[column].equal(lhs[key_column],
                                                     rhs[key_column]))
            return false;
          ++key_column;
        }
        return true;
      };

      bool saw_relevant_delta = false;
      for (const AggregateSourceOp &source :
           aggregate_operation->aggregate_body) {
        if (source.code != OpCode::Scan)
          continue;
        auto delta = run_delta.find(source.atom.relation);
        if (delta == run_delta.end() || delta->second.empty())
          continue;
        saw_relevant_delta = true;
        for (VarId group_variable : group_variables) {
          const bool present = std::any_of(
              source.atom.terms.begin(), source.atom.terms.end(),
              [&](const AtomTermPlan &term) {
                return term.is_variable && term.variable == group_variable;
              });
          if (!present)
            return false;
        }
        for (std::size_t row_index = 0; row_index < delta->second.size();
             ++row_index) {
          RowView changed_row =
              delta->second.row(relation(source.atom.relation), row_index);
          Group group{Binding(plan.variable_count), {}};
          for (std::size_t column = 0; column < source.atom.terms.size();
               ++column) {
            const AtomTermPlan &term = source.atom.terms[column];
            if (term.is_variable &&
                std::find(group_variables.begin(), group_variables.end(),
                          term.variable) != group_variables.end() &&
                !group.binding[term.variable]) {
              group.binding[term.variable].bindOwned(
                  changed_row[column].materialize());
            }
          }
          for (const HeadTermPlan &term : rule.head) {
            if (term.kind == HeadTermPlan::Kind::Variable &&
                term.variable == output_variable)
              continue;
            if (term.kind == HeadTermPlan::Kind::Variable)
              group.key.push_back(group.binding[term.variable].materialize());
            else
              group.key.push_back(term.constant);
          }
          if (std::none_of(groups.begin(), groups.end(),
                           [&](const Group &item) {
                             return same_group(item.key, group.key);
                           }))
            groups.push_back(std::move(group));
        }
      }
      if (!saw_relevant_delta)
        continue;

      std::vector<std::vector<std::any>> keys;
      keys.reserve(groups.size());
      for (const Group &group : groups)
        keys.push_back(group.key);
      relation(rule.head_relation).removeDerivedMatching(group_mask, keys);
      CandidateMap candidates;
      for (Group &group : groups) {
        evaluateRule(rule_index, group.binding, std::nullopt, std::nullopt,
                     candidates, stats, &scheduler, nullptr, nullptr, nullptr,
                     ruleProfile(rule_index));
      }
      stats.incremental_aggregate_groups += groups.size();
      DeltaMap inserted = mergeCandidates(std::move(candidates), scheduler);
      for (auto &[relation_id, rows] : inserted)
        run_delta[relation_id].append(std::move(rows));
    }
    return true;
  }

  bool replanIfNeeded() {
    if (!has_completed_run ||
        completed_base_versions.size() != compiled_relation_count)
      return false;
    bool base_changed = false;
    for (RelationId relation_id = 0; relation_id < compiled_relation_count;
         ++relation_id) {
      base_changed = base_changed || relation(relation_id).baseVersion() !=
                                         completed_base_versions[relation_id];
    }
    if (!base_changed)
      return false;

    const std::size_t ratio =
        std::max<std::size_t>(2, plan.adaptive_replan_ratio);
    bool should_replan = false;
    for (RelationId relation_id = 0; relation_id < compiled_relation_count;
         ++relation_id) {
      const std::size_t previous = plan.planned_cardinalities[relation_id];
      const std::size_t current = relation(relation_id).rowCount();
      const std::size_t smaller = std::min(previous, current);
      const std::size_t larger = std::max(previous, current);
      if ((smaller == 0 && larger != 0) ||
          (smaller != 0 && larger / smaller >= ratio)) {
        should_replan = true;
        break;
      }
    }
    if (!should_replan && profile.collected) {
      for (std::size_t rule_index = 0;
           rule_index < plan.rules.size() && !should_replan; ++rule_index) {
        for (std::size_t item_index = 0;
             item_index < plan.rules[rule_index].body.size(); ++item_index) {
          const std::size_t estimate = std::max<std::size_t>(
              1, plan.rules[rule_index].body[item_index].estimated_output_rows);
          const std::size_t actual =
              profile.rules[rule_index].operations[item_index].output_bindings;
          if (actual / estimate >= ratio ||
              estimate / std::max<std::size_t>(1, actual) >= ratio) {
            should_replan = true;
            break;
          }
        }
      }
    }
    if (!should_replan)
      return false;

    std::size_t reorder_count = 0;
    std::vector<RuleIR> semantic_rules =
        planAndValidateRules(plan.logical_rules, context->relations,
                             context->variables, reorder_count);
    std::vector<RulePlan> physical_rules;
    physical_rules.reserve(semantic_rules.size());
    for (const RuleIR &rule : semantic_rules)
      physical_rules.push_back(
          internal::lowerRulePlan(rule, context->relations));
    prepareAccessPaths(physical_rules, context->relations,
                       plan.max_arrangements_per_relation,
                       plan.index_memory_budget_bytes);
    plan.rules = std::move(physical_rules);
    plan.jit_compiled_expressions = internal::compileRuleKernels(plan.rules);
    plan.planned_reorders = reorder_count;
    plan.planned_cardinalities.clear();
    for (std::size_t relation_id = 0; relation_id < compiled_relation_count;
         ++relation_id)
      plan.planned_cardinalities.push_back(
          context->relations[relation_id]->rowCount());
    return true;
  }

  RunStatus run(const ExecutionOptions &options) {
    ExecutionContext execution_context{options};
    ExecutionScope execution_scope(*this, execution_context);
    if (options.scheduler && options.scheduler->workerCount() == 0) {
      throw std::invalid_argument(
          "Datalog Scheduler must report at least one worker");
    }
    const bool adaptive_replanned = replanIfNeeded();
    stats = {};
    stats.planned_reorders = plan.planned_reorders;
    stats.scc_count = plan.sccs.size();
    stats.relation_count = compiled_relation_count;
    stats.adaptive_replans = adaptive_replanned ? 1 : 0;
    stats.pruned_rules = plan.pruned_rules;
    stats.jit_compiled_expressions = plan.jit_compiled_expressions;
    profile = {};
    profile.collected = options.collect_profile;
    if (profile.collected) {
      profile.rules.resize(plan.rules.size());
      for (std::size_t rule_index = 0; rule_index < plan.rules.size();
           ++rule_index) {
        profile.rules[rule_index].operations.resize(
            plan.rules[rule_index].body.size());
      }
    }

    enum class UpdateMode : unsigned char {
      Unchanged,
      Incremental,
      AggregateIncremental,
      Rebuild,
    };
    std::vector<UpdateMode> update_modes(plan.sccs.size(),
                                         UpdateMode::Unchanged);
    std::vector<std::size_t> worklist;
    auto promote_mode = [&](std::size_t scc_index, UpdateMode mode) {
      if (static_cast<unsigned char>(update_modes[scc_index]) >=
          static_cast<unsigned char>(mode))
        return;
      update_modes[scc_index] = mode;
      worklist.push_back(scc_index);
    };
    DeltaMap run_delta;
    if (!has_completed_run) {
      std::fill(update_modes.begin(), update_modes.end(), UpdateMode::Rebuild);
    } else {
      for (RelationId relation_id = 0; relation_id < compiled_relation_count;
           ++relation_id) {
        RelationStorage &storage = relation(relation_id);
        if (storage.baseVersion() == completed_base_versions[relation_id])
          continue;
        if (storage.hasBaseDeletionSince(
                completed_base_versions[relation_id])) {
          promote_mode(plan.relation_scc[relation_id], UpdateMode::Rebuild);
          continue;
        }
        if (storage.definition().kind == RelationKind::Set) {
          std::vector<std::size_t> rows =
              storage.baseSetDeltaSince(completed_base_versions[relation_id]);
          if (rows.empty())
            continue;
          stats.base_delta_facts += rows.size();
          run_delta[relation_id].set_row_ids = std::move(rows);
          promote_mode(plan.relation_scc[relation_id], UpdateMode::Incremental);
        } else {
          promote_mode(plan.relation_scc[relation_id], UpdateMode::Rebuild);
        }
      }
      while (!worklist.empty()) {
        const std::size_t source = worklist.back();
        worklist.pop_back();
        for (const auto &dependency : plan.scc_dependents[source]) {
          UpdateMode target_mode = UpdateMode::Rebuild;
          if (update_modes[source] == UpdateMode::Incremental) {
            if (dependency.kind == DependencyKind::Positive) {
              target_mode = UpdateMode::Incremental;
            } else if (dependency.kind == DependencyKind::Aggregate &&
                       canIncrementalAggregate(plan.sccs[dependency.target])) {
              target_mode = UpdateMode::AggregateIncremental;
            }
          }
          promote_mode(dependency.target, target_mode);
        }
      }
    }

    auto record_base_versions = [&] {
      completed_base_versions.clear();
      completed_base_versions.reserve(compiled_relation_count);
      for (std::size_t relation_id = 0; relation_id < compiled_relation_count;
           ++relation_id) {
        completed_base_versions.push_back(
            context->relations[relation_id]->baseVersion());
      }
    };

    if (std::all_of(
            update_modes.begin(), update_modes.end(),
            [](UpdateMode mode) { return mode == UpdateMode::Unchanged; })) {
      record_base_versions();
      collectStorageStats();
      return options.cancellation.isCancelled() ? RunStatus::Cancelled
                                                : RunStatus::Completed;
    }

    for (std::size_t scc_index = 0; scc_index < plan.sccs.size(); ++scc_index) {
      if (update_modes[scc_index] != UpdateMode::Rebuild)
        continue;
      for (RelationId relation_id : plan.sccs[scc_index].relations)
        relation(relation_id).discardDerived();
    }

    auto discard_dirty_derived = [&] {
      for (std::size_t scc_index = 0; scc_index < plan.sccs.size();
           ++scc_index) {
        if (update_modes[scc_index] == UpdateMode::Unchanged)
          continue;
        for (RelationId relation_id : plan.sccs[scc_index].relations)
          relation(relation_id).discardDerived();
      }
    };

    try {
      cancellationPoint();
      Binding binding(plan.variable_count);
      SerialScheduler serial_scheduler;
      std::unique_ptr<ThreadScheduler> thread_scheduler;
      Scheduler *scheduler = options.scheduler;
      if (!scheduler && options.worker_count > 1) {
        thread_scheduler =
            std::make_unique<ThreadScheduler>(options.worker_count);
        scheduler = thread_scheduler.get();
      }
      if (!scheduler)
        scheduler = &serial_scheduler;
      for (std::size_t scc_index = 0; scc_index < plan.sccs.size();
           ++scc_index) {
        cancellationPoint();
        if (update_modes[scc_index] == UpdateMode::Unchanged)
          continue;
        const PlannedSCC &scc = plan.sccs[scc_index];
        if (options.trace_scc) {
          std::lock_guard<std::mutex> lock(trace_mutex);
          traceStream() << "SCC " << scc_index << " stratum=" << scc.stratum
                        << " recursive=" << (scc.recursive ? "yes" : "no")
                        << " mode="
                        << (update_modes[scc_index] == UpdateMode::Incremental
                                ? "incremental"
                            : update_modes[scc_index] ==
                                    UpdateMode::AggregateIncremental
                                ? "aggregate-incremental"
                                : "rebuild")
                        << " relations=";
          for (RelationId relation_id : scc.relations)
            traceStream() << relation(relation_id).definition().name << ' ';
          traceStream() << '\n';
        }
        if (update_modes[scc_index] == UpdateMode::Incremental) {
          ++stats.incremental_sccs;
          runIncremental(scc, run_delta, *scheduler);
        } else if (update_modes[scc_index] ==
                   UpdateMode::AggregateIncremental) {
          if (runIncrementalAggregate(scc, run_delta, *scheduler)) {
            ++stats.incremental_sccs;
          } else {
            for (RelationId relation_id : scc.relations)
              relation(relation_id).discardDerived();
            ++stats.rebuilt_sccs;
            runNonRecursive(scc, binding, *scheduler);
          }
        } else if (scc.recursive) {
          ++stats.rebuilt_sccs;
          runRecursive(scc, binding, *scheduler);
        } else {
          ++stats.rebuilt_sccs;
          runNonRecursive(scc, binding, *scheduler);
        }
      }
      cancellationPoint();
    } catch (const CancelledRun &) {
      discard_dirty_derived();
      has_completed_run = false;
      collectStorageStats();
      return RunStatus::Cancelled;
    } catch (...) {
      discard_dirty_derived();
      has_completed_run = false;
      collectStorageStats();
      throw;
    }

    record_base_versions();
    collectStorageStats();
    has_completed_run = true;
    return RunStatus::Completed;
  }

  RunStatus runReadOnly() {
    std::shared_lock<std::shared_mutex> lock(context->execution_mutex);
    if (!has_completed_run)
      throw std::logic_error(
          "Datalog read-only execution requires a completed fixpoint");
    for (RelationId relation_id = 0; relation_id < compiled_relation_count;
         ++relation_id) {
      if (relation(relation_id).baseVersion() !=
          completed_base_versions[relation_id]) {
        throw std::logic_error(
            "Datalog read-only execution observed pending base changes");
      }
    }
    return RunStatus::Completed;
  }
};

CompiledProgram Program::compile() const { return compile(CompileOptions{}); }

CompiledProgram Program::compile(const CompileOptions &options) const {
  std::unique_lock<std::shared_mutex> lock(context_->impl_->execution_mutex,
                                           std::try_to_lock);
  if (!lock.owns_lock() || context_->impl_->running) {
    throw std::logic_error(
        "Datalog programs may not be compiled while a program is running");
  }
  ExecutionPlan plan;
  plan.index_memory_budget_bytes = options.index_memory_budget_bytes;
  plan.max_arrangements_per_relation = options.max_arrangements_per_relation;
  plan.adaptive_replan_ratio =
      std::max<std::size_t>(2, options.adaptive_replan_ratio);
  std::vector<RuleIR> selected_rules = rules_;
  std::vector<RelationId> goal_relations = options.goals;
  for (const QueryGoal &goal : options.query_goals)
    goal_relations.push_back(goal.relation);
  if (!goal_relations.empty()) {
    std::vector<bool> needed(context_->impl_->relations.size(), false);
    for (RelationId goal : goal_relations) {
      if (goal >= needed.size())
        throw CompileError("compile goal references an unknown relation");
      needed[goal] = true;
    }
    bool changed = true;
    while (changed) {
      changed = false;
      for (const RuleIR &rule : rules_) {
        if (rule.head.relation >= needed.size())
          throw CompileError("rule head references an unknown relation");
        if (!needed[rule.head.relation])
          continue;
        for (const BodyItemIR &item : rule.body) {
          auto mark_source = [&](RelationId source) {
            if (source >= needed.size())
              throw CompileError("rule body references an unknown relation");
            if (!needed[source]) {
              needed[source] = true;
              changed = true;
            }
          };
          if (const auto *atom = std::get_if<AtomIR>(&item)) {
            mark_source(atom->relation);
          } else if (const auto *negation = std::get_if<NegAtomIR>(&item)) {
            mark_source(negation->atom.relation);
          } else if (const auto *aggregate = std::get_if<AggregateIR>(&item)) {
            if (aggregate->source_body.empty()) {
              mark_source(aggregate->source.relation);
            } else {
              for (const AggregateSourceItemIR &source_item :
                   aggregate->source_body) {
                if (const auto *source_atom = std::get_if<AtomIR>(&source_item))
                  mark_source(source_atom->relation);
                else if (const auto *source_negation =
                             std::get_if<NegAtomIR>(&source_item))
                  mark_source(source_negation->atom.relation);
              }
            }
          }
        }
      }
    }
    selected_rules.clear();
    for (const RuleIR &rule : rules_) {
      if (needed[rule.head.relation])
        selected_rules.push_back(rule);
    }
    plan.pruned_rules = rules_.size() - selected_rules.size();
  }
  selected_rules = specializeForQueryGoals(selected_rules, options.query_goals,
                                           context_->impl_->relations);
  plan.logical_rules = selected_rules;
  std::vector<RuleIR> semantic_rules =
      planAndValidateRules(selected_rules, context_->impl_->relations,
                           context_->impl_->variables, plan.planned_reorders);
  plan.variable_count = context_->impl_->variables.size();
  const std::vector<DependencyEdge> dependencies =
      collectDependencies(semantic_rules);
  const std::vector<std::size_t> strata =
      computeStrata(dependencies, context_->impl_->relations.size());
  plan.sccs = buildSCCPlan(semantic_rules, dependencies, strata,
                           context_->impl_->relations.size());
  plan.rules.reserve(semantic_rules.size());
  for (const RuleIR &rule : semantic_rules)
    plan.rules.push_back(
        internal::lowerRulePlan(rule, context_->impl_->relations));
  plan.planned_cardinalities.reserve(context_->impl_->relations.size());
  for (const auto &storage : context_->impl_->relations)
    plan.planned_cardinalities.push_back(storage->rowCount());
  prepareAccessPaths(plan.rules, context_->impl_->relations,
                     plan.max_arrangements_per_relation,
                     plan.index_memory_budget_bytes);
  plan.jit_compiled_expressions = internal::compileRuleKernels(plan.rules);
  auto impl = std::make_unique<CompiledProgram::Impl>();
  impl->context = context_->impl_;
  impl->compiled_relation_count = context_->impl_->relations.size();
  impl->plan = std::move(plan);
  impl->plan.relation_scc.assign(context_->impl_->relations.size(), 0);
  for (std::size_t scc_index = 0; scc_index < impl->plan.sccs.size();
       ++scc_index) {
    for (RelationId relation_id : impl->plan.sccs[scc_index].relations)
      impl->plan.relation_scc[relation_id] = scc_index;
  }
  impl->plan.scc_dependents.resize(impl->plan.sccs.size());
  for (const DependencyEdge &dependency : dependencies) {
    const std::size_t source_scc = impl->plan.relation_scc[dependency.source];
    const std::size_t target_scc = impl->plan.relation_scc[dependency.target];
    if (source_scc == target_scc)
      continue;
    auto &outgoing = impl->plan.scc_dependents[source_scc];
    auto existing =
        std::find_if(outgoing.begin(), outgoing.end(), [&](const auto &edge) {
          return edge.target == target_scc;
        });
    if (existing == outgoing.end()) {
      outgoing.push_back({target_scc, dependency.kind});
    } else if (dependency.kind != DependencyKind::Positive) {
      existing->kind = dependency.kind;
    }
  }
  for (auto &dependents : impl->plan.scc_dependents) {
    std::sort(dependents.begin(), dependents.end(),
              [](const auto &lhs, const auto &rhs) {
                return lhs.target < rhs.target;
              });
  }
  return CompiledProgram(std::move(impl));
}

CompiledProgram::CompiledProgram(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
CompiledProgram::CompiledProgram(CompiledProgram &&) noexcept = default;
CompiledProgram &
CompiledProgram::operator=(CompiledProgram &&) noexcept = default;
CompiledProgram::~CompiledProgram() = default;

RunStatus CompiledProgram::run() { return impl_->run(ExecutionOptions{}); }

RunStatus CompiledProgram::run(const ExecutionOptions &options) {
  return impl_->run(options);
}

RunStatus CompiledProgram::runReadOnly() { return impl_->runReadOnly(); }

const ExecutionStats &CompiledProgram::stats() const { return impl_->stats; }

const ExecutionProfile &CompiledProgram::profile() const {
  return impl_->profile;
}

std::string CompiledProgram::explain(ExplainMode mode) const {
  return impl_->explain(mode);
}

} // namespace lotus::datalog
