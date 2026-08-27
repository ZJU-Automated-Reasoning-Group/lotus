#include "Dataflow/Datalog/EngineInternal.h"
#include "Dataflow/Datalog/Core/Program.h"

#include <algorithm>
#include <any>
#include <array>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::datalog {

using internal::AtomPlan;
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
  std::uint64_t compiled_schema_generation = 0;
  std::vector<std::size_t> completed_base_versions;
  bool has_completed_run = false;
  std::mutex trace_mutex;

  struct ExecutionScope {
    Impl &impl;
    std::unique_lock<std::mutex> context_lock;
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
  using CandidateMap =
      std::unordered_map<RelationId, CandidateCollection>;
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
    for (const auto &storage : context->relations) {
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

  bool matchRow(const AtomPlan &atom, RowView row, Binding &binding,
                const std::function<void()> &continuation) {
    std::array<VarId, KeyView::MAX_COLUMNS> newly_bound{};
    std::size_t newly_bound_count = 0;
    for (std::size_t column = 0; column < atom.terms.size(); ++column) {
      const auto &term = atom.terms[column];
      const ColumnType &type =
          relation(atom.relation).definition().columns[column];
      if (!term.is_variable) {
        const bool equal = type.equal_value
                               ? type.equal_value(
                                     ValueRef::fromAny(term.constant),
                                     row[column])
                               : type.equal(term.constant,
                                            row[column].materialize());
        if (!equal) {
          for (std::size_t index = 0; index < newly_bound_count; ++index)
            binding[newly_bound[index]].reset();
          return false;
        }
        continue;
      }

      if (binding[term.variable]) {
        const bool equal = type.equal_value
                               ? type.equal_value(
                                     *binding[term.variable],
                                     row[column])
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
      source_relation.forEachMatchingSlice(
          key, begin, end, [&](RowView row) {
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
    ++evaluation_stats.rule_evaluations;
    if (evaluation_profile) {
      prepareRuleProfile(*evaluation_profile, rule.body.size());
      ++evaluation_profile->evaluations;
    }
    traceRule(rule_index, delta_item);
    std::function<void(std::size_t)> evaluate_item =
        [&](std::size_t item_index) {
          cancellationPoint();
          if (item_index == rule.body.size()) {
            Row row;
            row.reserve(rule.head.size());
            for (const HeadTermPlan &term : rule.head) {
              if (term.kind == HeadTermPlan::Kind::Variable)
                row.push_back(binding[term.variable].materialize());
              else if (term.kind == HeadTermPlan::Kind::Constant)
                row.push_back(term.constant);
              else
                row.push_back(term.expression.evaluate(binding));
            }
            ++evaluation_stats.head_derivations;
            if (emitCandidate(candidates, rule.head_relation, std::move(row)))
              ++evaluation_stats.local_unique_candidates;
            if (evaluation_profile)
              ++evaluation_profile->head_candidates;
            return;
          }

          const PhysicalOp &item = rule.body[item_index];
          OperationProfile *operation_profile =
              evaluation_profile
                  ? &evaluation_profile->operations[item_index]
                  : nullptr;
          if (operation_profile)
            ++operation_profile->invocations;
          if (item.code == OpCode::Filter) {
            if (operation_profile)
              ++operation_profile->candidate_rows;
            if (std::any_cast<bool>(item.filter.evaluate(binding))) {
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
            const AtomPlan &source = item.atom;
            const KeyView key = lookupKey(source, binding);
            std::vector<std::any> results = evaluateAggregate(
                aggregate, source, relation(source.relation), key, binding,
                aggregate_scheduler, evaluation_stats, operation_profile);
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
            if (matchRow(atom, row, binding, [&] {
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
          relation(atom.relation)
              .forEachMatching(key, evaluation_stats, continue_with_old_total);
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
        std::min(scheduler.workerCount(), (rows.size() + grain - 1) / grain);
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
          std::move(coalesced), scheduler, options().parallel_grain_size);
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
            output << ' '
                   << relation(operation.atom.relation).definition().name
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
          tasks.push_back(
              {rule_index, driver_item,
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
        tasks.push_back(
            {rule_index, driver_item,
             DeltaView{driver.relation, nullptr, std::nullopt, begin,
                       std::min(row_count, begin + grain)}});
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
        destination.insert(
            destination.end(),
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
          destination.insert(
              destination.end(),
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
        hashes[relation(relation_id).candidateHash(
                   rows.row(relation(relation_id), row_index))]
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
        destination.insert(
            destination.end(),
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

  RunStatus run(const ExecutionOptions &options) {
    ExecutionContext execution_context{options};
    ExecutionScope execution_scope(*this, execution_context);
    if (context->schema_generation != compiled_schema_generation) {
      throw std::logic_error(
          "Datalog Context schema changed after compilation; recompile the "
          "program");
    }
    if (options.scheduler && options.scheduler->workerCount() == 0) {
      throw std::invalid_argument(
          "Datalog Scheduler must report at least one worker");
    }
    stats = {};
    stats.planned_reorders = plan.planned_reorders;
    stats.scc_count = plan.sccs.size();
    stats.relation_count = context->relations.size();
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
      for (RelationId relation_id = 0; relation_id < context->relations.size();
           ++relation_id) {
        RelationStorage &storage = relation(relation_id);
        if (storage.baseVersion() ==
            completed_base_versions[relation_id])
          continue;
        if (storage.definition().kind == RelationKind::Set) {
          std::vector<std::size_t> rows = storage.baseSetDeltaSince(
              completed_base_versions[relation_id]);
          if (rows.empty())
            continue;
          stats.base_delta_facts += rows.size();
          run_delta[relation_id].set_row_ids = std::move(rows);
          promote_mode(plan.relation_scc[relation_id],
                       UpdateMode::Incremental);
        } else {
          promote_mode(plan.relation_scc[relation_id], UpdateMode::Rebuild);
        }
      }
      while (!worklist.empty()) {
        const std::size_t source = worklist.back();
        worklist.pop_back();
        for (const auto &dependency : plan.scc_dependents[source]) {
          const UpdateMode target_mode =
              update_modes[source] == UpdateMode::Rebuild ||
                      dependency.kind != DependencyKind::Positive
                  ? UpdateMode::Rebuild
                  : UpdateMode::Incremental;
          promote_mode(dependency.target, target_mode);
        }
      }
    }

    auto record_base_versions = [&] {
      completed_base_versions.clear();
      completed_base_versions.reserve(context->relations.size());
      for (const auto &storage : context->relations)
        completed_base_versions.push_back(storage->baseVersion());
    };

    if (std::all_of(update_modes.begin(), update_modes.end(),
                    [](UpdateMode mode) {
                      return mode == UpdateMode::Unchanged;
                    })) {
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
                                : "rebuild")
                        << " relations=";
          for (RelationId relation_id : scc.relations)
            traceStream() << relation(relation_id).definition().name << ' ';
          traceStream() << '\n';
        }
        if (update_modes[scc_index] == UpdateMode::Incremental) {
          ++stats.incremental_sccs;
          runIncremental(scc, run_delta, *scheduler);
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
};

CompiledProgram Program::compile() const {
  std::unique_lock<std::mutex> lock(context_->impl_->execution_mutex,
                                    std::try_to_lock);
  if (!lock.owns_lock() || context_->impl_->running) {
    throw std::logic_error(
        "Datalog programs may not be compiled while a program is running");
  }
  ExecutionPlan plan;
  std::vector<RuleIR> semantic_rules =
      planAndValidateRules(rules_, context_->impl_->relations,
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
  for (const RulePlan &rule : plan.rules) {
    for (const PhysicalOp &operation : rule.body) {
      if (operation.code == OpCode::Scan ||
          operation.code == OpCode::AntiLookup ||
          operation.code == OpCode::Aggregate) {
        context_->impl_->relations.at(operation.atom.relation)
            ->ensureIndex(operation.atom.lookup_mask);
      }
    }
  }
  auto impl = std::make_unique<CompiledProgram::Impl>();
  impl->context = context_->impl_;
  impl->compiled_schema_generation = context_->impl_->schema_generation;
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
    auto existing = std::find_if(
        outgoing.begin(), outgoing.end(), [&](const auto &edge) {
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

const ExecutionStats &CompiledProgram::stats() const { return impl_->stats; }

const ExecutionProfile &CompiledProgram::profile() const {
  return impl_->profile;
}

std::string CompiledProgram::explain(ExplainMode mode) const {
  return impl_->explain(mode);
}

} // namespace lotus::datalog
