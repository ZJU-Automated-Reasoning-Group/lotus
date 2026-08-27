#pragma once

#include "Dataflow/Datalog/Core/Context.h"
#include "Dataflow/Datalog/Core/Error.h"
#include "Dataflow/Datalog/Runtime/Scheduler.h"
#include "Dataflow/Datalog/Semantic/SemanticIR.h"

#include <algorithm>
#include <any>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::datalog::internal {

using Row = std::vector<std::any>;

class RelationStorage;

class RowView {
public:
  RowView(const RelationStorage &storage, std::size_t row_id);
  explicit RowView(const Row &row);

  std::size_t size() const;
  ValueRef operator[](std::size_t column) const;

private:
  const RelationStorage *storage_ = nullptr;
  const Row *dynamic_ = nullptr;
  std::size_t row_id_ = 0;
};

struct RangePlan {
  std::size_t column = 0;
  ExprOpcode comparison = ExprOpcode::Less;
  bool variable_on_left = true;
  std::any bound;
};

// A lookup key borrows constant terms and binding cells.  It is deliberately
// fixed-size: relation arity is already bounded by ColumnMask, so constructing
// a key in the join loop must not allocate or copy std::any values.
struct KeyView {
  static constexpr std::size_t MAX_COLUMNS = sizeof(ColumnMask) * CHAR_BIT;

  ColumnMask mask = 0;
  std::array<ValueRef, MAX_COLUMNS> values{};
  std::size_t size = 0;

  void push(const std::any &value) {
    values[size++] = ValueRef::fromAny(value);
  }
  void push(ValueRef value) { values[size++] = value; }
};

void combineHash(std::size_t &seed, std::size_t value);

struct RowHash {
  std::vector<ColumnType> columns;
  std::size_t operator()(const Row &row) const;
};

struct RowEqual {
  std::vector<ColumnType> columns;
  bool operator()(const Row &lhs, const Row &rhs) const;
};

struct KeyHash {
  std::vector<ColumnType> columns;
  std::size_t operator()(const Row &row) const;
};

struct KeyEqual {
  std::vector<ColumnType> columns;
  bool operator()(const Row &lhs, const Row &rhs) const;
};

class RuntimeIndex {
public:
  RuntimeIndex(ColumnMask mask, const std::vector<ColumnType> &all_columns);

  void rebuild(const RelationStorage &storage, std::size_t version);
  void append(const RelationStorage &storage, std::size_t row_index,
              std::size_t version);
  std::size_t appendBatch(const RelationStorage &storage,
                          const std::vector<std::size_t> &row_ids,
                          std::size_t version, Scheduler &scheduler);
  void update(const RelationStorage &storage, std::size_t row_index,
              const Row &previous_row, std::size_t version);
  bool isCurrent(std::size_t version) const;
  bool canServe(ColumnMask mask) const;
  ColumnMask mask() const { return mask_; }
  const std::vector<std::size_t> *lookup(const KeyView &key) const;
  bool matches(RowView row, const KeyView &key) const;
  std::size_t bucketCount() const;
  std::size_t entryCount() const;
  std::size_t approximateMemoryBytes() const;

private:
  std::size_t hash(RowView row, std::size_t prefix_size) const;
  std::size_t hash(const KeyView &key) const;
  void erase(std::size_t prefix_index, std::size_t key_hash,
             std::size_t row_index);
  void insert(const RelationStorage &storage, std::size_t row_index);

  std::vector<std::size_t> columns_;
  std::vector<ColumnType> column_types_;
  using Buckets = std::unordered_map<std::size_t, std::vector<std::size_t>>;
  static constexpr std::size_t SHARD_COUNT = 16;
  std::vector<std::vector<Buckets>> prefix_buckets_;
  ColumnMask mask_ = 0;
  std::size_t built_version_ = static_cast<std::size_t>(-1);
};

class RelationStorage {
public:
  using KeyMap = std::unordered_map<Row, std::size_t, KeyHash, KeyEqual>;
  using SetDirectory =
      std::unordered_map<std::size_t, std::vector<std::size_t>>;

  struct BatchMergeResult {
    std::vector<std::size_t> changed_row_ids;
    std::vector<Row> changed_lattice_rows;
    std::size_t parallel_tasks = 0;

    std::size_t changedCount() const {
      return changed_row_ids.size() + changed_lattice_rows.size();
    }

    bool empty() const { return changedCount() == 0; }
  };

  struct KeyFrequency {
    Row key;
    std::size_t frequency = 0;
  };

  struct MaskStatistics {
    std::size_t row_count = 0;
    std::size_t distinct_count = 0;
    std::size_t maximum_frequency = 0;
    std::vector<KeyFrequency> heavy_hitters;

    std::size_t averageFrequency() const {
      return distinct_count == 0
                 ? 1
                 : std::max<std::size_t>(1, (row_count + distinct_count - 1) /
                                                distinct_count);
    }
  };

  explicit RelationStorage(RelationIR definition);

  const RelationIR &definition() const;
  std::size_t rowCount() const;
  RowView row(std::size_t row_id) const;
  ValueRef value(std::size_t row_id, std::size_t column) const;
  Row materializeRow(std::size_t row_id) const;
  std::vector<Row> materializeRows() const;

  bool insertBase(Row row);
  bool eraseBase(const Row &row);
  std::size_t
  removeDerivedMatching(ColumnMask mask,
                        const std::vector<std::vector<std::any>> &keys);
  bool contains(const Row &row) const;
  std::vector<Row> coalesce(std::vector<Row> candidates) const;
  std::size_t candidateHash(const Row &row) const;
  std::size_t candidateHash(RowView row) const;
  bool rowsEqual(const Row &lhs, const Row &rhs) const;
  bool rowsEqual(RowView lhs, RowView rhs) const;
  BatchMergeResult mergeDerivedCoalesced(std::vector<Row> candidates,
                                         Scheduler &scheduler,
                                         std::size_t grain_size,
                                         bool debug_contracts = false);
  void discardDerived();
  std::size_t baseVersion() const;
  std::vector<std::size_t>
  baseSetDeltaSince(std::size_t completed_base_version) const;
  bool hasBaseDeletionSince(std::size_t completed_base_version) const;
  std::size_t estimatedLookupCardinality(ColumnMask mask) const;
  MaskStatistics maskStatistics(ColumnMask mask) const;
  void ensureIndex(ColumnMask mask);
  bool hasPreparedIndex(ColumnMask mask) const;
  void ensureOrderedIndex(std::size_t column);
  std::size_t indexCount() const;
  std::size_t indexEntries() const;
  std::size_t indexMemoryBytes() const;
  std::size_t tupleMemoryBytes() const;
  std::size_t uniquenessMemoryBytes() const;
  std::size_t baseMemoryBytes() const;
  bool parallelSafe() const;

  void forEachMatching(const KeyView &key, ExecutionStats &stats,
                       const std::function<void(RowView)> &callback);
  std::size_t matchingCandidateCount(const KeyView &key, ExecutionStats &stats);
  void forEachMatchingSlice(const KeyView &key, std::size_t begin,
                            std::size_t end,
                            const std::function<void(RowView)> &callback);
  void forEachRange(const RangePlan &range, ExecutionStats &stats,
                    const std::function<void(RowView)> &callback);

private:
  void validateRow(const Row &row) const;
  RuntimeIndex &getIndex(ColumnMask mask);
  RuntimeIndex &preparedIndex(ColumnMask mask);
  void rebuildOrderedIndex(std::size_t column);
  void appendRow(Row row);
  std::size_t appendRowsBatch(std::vector<Row> &rows,
                              const std::vector<std::size_t> &selected,
                              std::vector<std::size_t> &row_ids,
                              Scheduler &scheduler);
  void updateRow(std::size_t row_index, Row row);
  void rebuildFromBase();
  std::optional<std::size_t> findSetRow(const Row &row) const;
  void addSetRow(std::size_t row_id);
  void rebuildSetDirectory();
  Row latticeKey(const Row &row) const;

  RelationIR definition_;
  std::vector<std::unique_ptr<ColumnStorage>> columns_;
  std::size_t row_count_ = 0;
  static constexpr std::size_t SET_SHARD_COUNT = 16;
  std::array<SetDirectory, SET_SHARD_COUNT> set_directories_;
  std::vector<unsigned char> base_flags_;
  std::vector<std::size_t> base_add_versions_;
  std::unique_ptr<KeyMap> lattice_keys_;
  std::vector<Row> base_rows_;
  std::unique_ptr<KeyMap> base_lattice_keys_;
  bool has_derived_state_ = false;
  std::unordered_map<ColumnMask, std::unique_ptr<RuntimeIndex>> indices_;
  struct OrderedIndex {
    std::vector<std::size_t> rows;
    std::size_t built_version = static_cast<std::size_t>(-1);
  };
  std::unordered_map<std::size_t, OrderedIndex> ordered_indices_;
  std::mutex index_mutex_;
  mutable std::mutex statistics_mutex_;
  mutable std::unordered_map<ColumnMask, std::pair<std::size_t, MaskStatistics>>
      statistics_cache_;
  std::size_t version_ = 0;
  std::size_t base_version_ = 0;
  std::size_t base_deletion_version_ = 0;
};

// Physical rule plans retain only the data needed by the executor. Semantic
// RuleIR remains the validation and analysis boundary; it is lowered once at
// compile time so the tuple loop does not repeatedly inspect semantic variants.
struct AtomTermPlan {
  bool is_variable = false;
  VarId variable = 0;
  bool anonymous = false;
  bool use_in_lookup = false;
  std::any constant;
};

struct AtomPlan {
  RelationId relation = 0;
  ColumnMask lookup_mask = 0;
  std::vector<AtomTermPlan> terms;
};

struct HeadTermPlan {
  enum class Kind { Variable, Constant, Expression };

  Kind kind = Kind::Constant;
  VarId variable = 0;
  std::any constant;
  ExprIR expression;
};

enum class OpCode { Scan, Filter, AntiLookup, Aggregate };

struct AggregateSourceOp {
  OpCode code = OpCode::Scan;
  AtomPlan atom;
  ExprIR filter;
};

struct PhysicalOp {
  OpCode code = OpCode::Scan;
  AtomPlan atom;
  ExprIR filter;
  AggregateIR aggregate;
  std::vector<AggregateSourceOp> aggregate_body;
  std::optional<RangePlan> range;
  std::size_t estimated_input_rows = 1;
  std::size_t estimated_lookup_rows = 1;
  std::size_t estimated_output_rows = 1;
};

struct RulePlan {
  enum class KernelKind {
    Interpreter,
    Projection,
  };

  RelationId head_relation = 0;
  std::vector<HeadTermPlan> head;
  std::vector<PhysicalOp> body;
  bool parallel_safe = true;
  KernelKind kernel_kind = KernelKind::Interpreter;
};

AtomPlan lowerAtomPlan(const AtomIR &atom, const std::vector<bool> &grounded);
RulePlan
lowerRulePlan(const RuleIR &rule,
              const std::vector<std::unique_ptr<RelationStorage>> &relations);
std::size_t compileRuleKernels(std::vector<RulePlan> &rules);

struct VariableDefinition {
  std::string name;
  std::type_index type = typeid(void);
  bool anonymous = false;
};

struct PlannedSCC {
  std::vector<RelationId> relations;
  std::unordered_set<RelationId> relation_set;
  std::vector<std::size_t> rules;
  bool recursive = false;
  std::size_t stratum = 0;
};

struct ExecutionPlan {
  std::vector<RuleIR> logical_rules;
  std::vector<RulePlan> rules;
  std::vector<PlannedSCC> sccs;
  std::size_t variable_count = 0;
  std::size_t planned_reorders = 0;
  std::size_t pruned_rules = 0;
  std::vector<std::size_t> planned_cardinalities;
  std::size_t index_memory_budget_bytes = static_cast<std::size_t>(-1);
  std::size_t max_arrangements_per_relation = static_cast<std::size_t>(-1);
  std::size_t adaptive_replan_ratio = 4;
  std::size_t jit_compiled_expressions = 0;
  // The SCC dependency DAG covers positive, negative, and aggregate reads.
  // Reruns invalidate only the transitive dependents of changed base facts.
  std::vector<std::size_t> relation_scc;
  struct SccDependency {
    std::size_t target = 0;
    DependencyKind kind = DependencyKind::Positive;
  };
  std::vector<std::vector<SccDependency>> scc_dependents;
};

struct DependencyEdge {
  RelationId source = 0;
  RelationId target = 0;
  DependencyKind kind = DependencyKind::Positive;
};

std::vector<RuleIR> planAndValidateRules(
    const std::vector<RuleIR> &input_rules,
    const std::vector<std::unique_ptr<RelationStorage>> &relations,
    const std::vector<VariableDefinition> &variables,
    std::size_t &reorder_count);

std::vector<DependencyEdge>
collectDependencies(const std::vector<RuleIR> &rules);

std::vector<std::size_t>
computeStrata(const std::vector<DependencyEdge> &dependencies,
              std::size_t relation_count);

std::vector<PlannedSCC>
buildSCCPlan(const std::vector<RuleIR> &rules,
             const std::vector<DependencyEdge> &dependencies,
             const std::vector<std::size_t> &strata,
             std::size_t relation_count);

} // namespace lotus::datalog::internal

namespace lotus::datalog {

struct Context::Impl {
  std::vector<std::unique_ptr<internal::RelationStorage>> relations;
  std::vector<internal::VariableDefinition> variables;
  std::unordered_set<std::string> relation_names;
  std::shared_mutex execution_mutex;
  std::uint64_t schema_generation = 0;
  bool running = false;
};

} // namespace lotus::datalog
