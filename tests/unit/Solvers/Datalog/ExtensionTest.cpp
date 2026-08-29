#include "TestSupport.h"

#include <any>
#include <atomic>
#include <stdexcept>
#include <string>

using namespace lotus::datalog;

struct ThrowingMinimum {
  int value = 0;
  static bool throw_on_join;

  bool joinMut(const ThrowingMinimum &other) {
    if (other.value >= value)
      return false;
    value = other.value;
    if (throw_on_join)
      throw std::runtime_error("expected lattice failure");
    return true;
  }

  friend bool operator==(const ThrowingMinimum &lhs,
                         const ThrowingMinimum &rhs) {
    return lhs.value == rhs.value;
  }
};

struct NonIdempotentLattice {
  int value = 0;
  bool joinMut(const NonIdempotentLattice &other) {
    value += other.value;
    return other.value != 0;
  }
  friend bool operator==(const NonIdempotentLattice &lhs,
                         const NonIdempotentLattice &rhs) {
    return lhs.value == rhs.value;
  }
};

bool ThrowingMinimum::throw_on_join = false;

namespace std {
template <> struct hash<ThrowingMinimum> {
  std::size_t operator()(const ThrowingMinimum &value) const {
    return std::hash<int>{}(value.value);
  }
};
template <> struct hash<NonIdempotentLattice> {
  std::size_t operator()(const NonIdempotentLattice &value) const {
    return std::hash<int>{}(value.value);
  }
};
} // namespace std

namespace {

TEST(DatalogTest, SupportsRemainderUnaryAndLiftedExpressions) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  for (int value = 1; value <= 4; ++value)
    input.insert(value);

  program p(ctx);
  p.rule(output(lift([](int value) { return value * value; }, -x)),
         input(x) && where((x % 2) == 0));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(output), (std::set<std::tuple<int>>{{4}, {16}}));
}

TEST(DatalogTest, SupportsMultipleRuleHeads) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto left = ctx.relation<int>("left");
  auto right = ctx.relation<int>("right");
  auto x = ctx.var<int>("x");
  source.insert(7);

  program p(ctx);
  p.rule({left(x), right(x)}, source(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(left.contains(7));
  EXPECT_TRUE(right.contains(7));
}

TEST(DatalogTest, SupportsAllBuiltInReducibleAggregates) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto sum_result = ctx.relation<int>("sum_result");
  auto min_result = ctx.relation<int>("min_result");
  auto max_result = ctx.relation<int>("max_result");
  auto count_result = ctx.relation<std::size_t>("count_result");
  auto x = ctx.var<int>("x");
  auto integer_result = ctx.var<int>("integer_result");
  auto size_result = ctx.var<std::size_t>("size_result");
  input.insert(4);
  input.insert(1);
  input.insert(9);

  program p(ctx);
  p.rule(sum_result(integer_result),
         aggregate(integer_result, sum(x), input(x)));
  p.rule(min_result(integer_result),
         aggregate(integer_result, minimum(x), input(x)));
  p.rule(max_result(integer_result),
         aggregate(integer_result, maximum(x), input(x)));
  p.rule(count_result(size_result), aggregate(size_result, count(), input(_)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(sum_result.contains(14));
  EXPECT_TRUE(min_result.contains(1));
  EXPECT_TRUE(max_result.contains(9));
  EXPECT_TRUE(count_result.contains(3));
}

TEST(DatalogTest, SupportsMaximumAndSetUnionLattices) {
  context ctx;
  auto maximum = ctx.lattice<int, max_lattice<int>>("maximum");
  auto sets = ctx.lattice<int, set_lattice<int>>("sets");

  maximum.insert(1, max_lattice<int>(3));
  maximum.insert(1, max_lattice<int>(8));
  maximum.insert(1, max_lattice<int>(5));
  sets.insert(1, set_lattice<int>{1, 2});
  sets.insert(1, set_lattice<int>{2, 3});

  EXPECT_TRUE(maximum.contains(1, max_lattice<int>(8)));
  EXPECT_TRUE(sets.contains(1, set_lattice<int>{1, 2, 3}));
  EXPECT_EQ(maximum.rows().size(), 1U);
  EXPECT_EQ(sets.rows().size(), 1U);
}

TEST(DatalogTest, ParallelizesNonRecursiveRuleEvaluation) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  for (int value = 0; value < 1000; ++value)
    input.insert(value);

  program p(ctx);
  p.rule(output(x + 1), input(x));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 16;
  compiled.run(options);

  EXPECT_EQ(output.rows().size(), 1000U);
  EXPECT_TRUE(output.contains(1000));
  EXPECT_GT(compiled.stats().parallel_tasks, 1U);
  EXPECT_GT(compiled.stats().parallel_rule_tasks, 1U);
}

TEST(DatalogTest, NonRecursiveConstantDriverUsesItsPlannedIndex) {
  context ctx;
  auto input = ctx.relation<int, int>("input");
  auto output = ctx.relation<int>("output");
  auto value = ctx.var<int>("value");
  for (int key = 0; key < 1000; ++key)
    input.insert(key, key + 1);

  program p(ctx);
  p.rule(output(value), input(777, value));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(output.contains(778));
  EXPECT_GT(compiled.stats().index_lookups, 0U);
  EXPECT_LT(compiled.stats().tuples_scanned, 10U);
}

TEST(DatalogTest, ParallelizesReducibleAggregateStates) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  auto result = ctx.var<int>("result");
  for (int value = 1; value <= 1000; ++value)
    input.insert(value);

  program p(ctx);
  p.rule(output(result), aggregate(result, sum(x), input(x)));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 16;
  compiled.run(options);

  EXPECT_TRUE(output.contains(500500));
  EXPECT_GT(compiled.stats().parallel_tasks, 1U);
  EXPECT_GT(compiled.stats().parallel_aggregate_tasks, 1U);
}

TEST(DatalogTest, ParallelizesStratifiedNegationDriver) {
  context ctx;
  auto universe = ctx.relation<int>("universe");
  auto blocked = ctx.relation<int>("blocked");
  auto allowed = ctx.relation<int>("allowed");
  auto x = ctx.var<int>("x");
  for (int value = 0; value < 1000; ++value) {
    universe.insert(value);
    if ((value % 2) == 0)
      blocked.insert(value);
  }

  program p(ctx);
  p.rule(allowed(x), universe(x) && neg(blocked(x)));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 16;
  compiled.run(options);

  EXPECT_EQ(allowed.rows().size(), 500U);
  EXPECT_TRUE(allowed.contains(999));
  EXPECT_GT(compiled.stats().parallel_tasks, 1U);
}

TEST(DatalogTest, ThreadSchedulerSupportsRepeatedAndNestedBatches) {
  ThreadScheduler scheduler(4);
  std::atomic<std::size_t> executions{0};
  scheduler.parallelFor(100, [&](std::size_t) { ++executions; });
  scheduler.parallelFor(8, [&](std::size_t) {
    scheduler.parallelFor(3, [&](std::size_t) { ++executions; });
  });

  EXPECT_EQ(executions.load(), 124U);
}

TEST(DatalogTest, ThreadSchedulerDrainsBatchBeforeRethrowingFailure) {
  ThreadScheduler scheduler(4);
  std::atomic<std::size_t> executions{0};

  EXPECT_THROW(scheduler.parallelFor(100,
                                     [&](std::size_t task) {
                                       ++executions;
                                       if (task == 17)
                                         throw std::runtime_error(
                                             "expected test failure");
                                     }),
               std::runtime_error);
  EXPECT_EQ(executions.load(), 100U);
}

TEST(DatalogTest, BindingSlotsReferenceRowsAndOwnComputedValues) {
  std::any row_value = std::string("before");
  BindingSlot referenced;
  referenced.bindReference(row_value);
  EXPECT_FALSE(referenced.ownsValue());

  BindingSlot copied = referenced;
  std::any_cast<std::string &>(row_value) = "after";
  EXPECT_EQ(copied.get<std::string>(), "after");

  BindingSlot owned;
  owned.bindOwned(std::any(std::string("computed")));
  BindingSlot owned_copy = owned;
  owned.reset();
  EXPECT_TRUE(owned_copy.ownsValue());
  EXPECT_EQ(owned_copy.get<std::string>(), "computed");
}

TEST(DatalogTest, PlannerUsesObservedIndexDistinctCounts) {
  context ctx;
  auto seed = ctx.relation<int>("seed");
  auto wide = ctx.relation<int, int>("wide");
  auto selective = ctx.relation<int, int>("selective");
  auto output = ctx.relation<int>("output");
  auto key = ctx.var<int>("key");
  auto value = ctx.var<int>("value");
  auto selected = ctx.var<int>("selected");

  seed.insert(0);
  for (int index = 0; index < 100; ++index) {
    wide.insert(0, index);
    selective.insert(index, index);
  }

  program p(ctx);
  p.rule(output(value),
         seed(key) && wide(key, value) && selective(key, selected));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(output.rows().size(), 100U);
  EXPECT_GT(compiled.stats().planned_reorders, 0U);
}

TEST(DatalogTest, ReusesMultiColumnArrangementForPrefixLookup) {
  context ctx;
  auto single_key = ctx.relation<int>("single_key");
  auto pair_key = ctx.relation<int, int>("pair_key");
  auto triple = ctx.relation<int, int, int>("triple");
  auto first_output = ctx.relation<int>("first_output");
  auto second_output = ctx.relation<int>("second_output");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");
  single_key.insert(1);
  pair_key.insert(1, 3);
  triple.insert(1, 10, 2);
  triple.insert(1, 11, 3);
  triple.insert(2, 12, 3);

  program p(ctx);
  p.rule(first_output(y), single_key(x) && triple(x, y, _));
  p.rule(second_output(y), pair_key(x, z) && triple(x, y, z));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(first_output.rows().size(), 2U);
  EXPECT_TRUE(second_output.contains(11));
  EXPECT_EQ(compiled.stats().index_count, 1U);
}

TEST(DatalogTest, IndexBudgetFallsBackToCorrectFullScan) {
  context ctx;
  auto input = ctx.relation<int, int>("input");
  auto output = ctx.relation<int>("output");
  auto value = ctx.var<int>("value");
  for (int key = 0; key < 100; ++key)
    input.insert(key, key + 1);
  program p(ctx);
  p.rule(output(value), input(77, value));

  CompileOptions options;
  options.index_memory_budget_bytes = 0;
  options.max_arrangements_per_relation = 0;
  auto compiled = p.compile(options);
  compiled.run();

  EXPECT_TRUE(output.contains(78));
  EXPECT_EQ(compiled.stats().index_count, 0U);
  EXPECT_EQ(compiled.stats().index_lookups, 0U);
  EXPECT_EQ(compiled.stats().tuples_scanned, 100U);
}

TEST(DatalogTest, UsesOrderedAccessPathForRangeFilter) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  for (int value = 0; value < 1000; ++value)
    input.insert(value);
  program p(ctx);
  p.rule(output(x), input(x) && where(990 <= x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(output.rows().size(), 10U);
  EXPECT_TRUE(output.contains(999));
  EXPECT_EQ(compiled.stats().ordered_range_lookups, 1U);
  EXPECT_EQ(compiled.stats().tuples_scanned, 10U);
}

TEST(DatalogTest, SupportsStreamingParameterizedAggregator) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  auto result = ctx.var<int>("result");
  for (int value = 1; value <= 10; ++value)
    input.insert(value);

  const int threshold = 5;
  auto sum_above = make_streaming_aggregator<int>(
      x, "sum-above", [threshold](const AggregateRange<int> &values) {
        int sum = 0;
        values.forEach([&](int value) {
          if (value > threshold)
            sum += value;
        });
        return std::vector<int>{sum};
      });

  program p(ctx);
  p.rule(output(result), aggregate(result, sum_above, input(x)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(output.contains(40));
}

TEST(DatalogTest, SupportsParameterizedReducibleAggregatorInParallel) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  auto result = ctx.var<int>("result");
  for (int value = 1; value <= 100; ++value)
    input.insert(value);

  const int scale = 3;
  auto scaled_sum = make_reducible_aggregator<int>(
      x, "scaled-sum", [] { return 0; },
      [scale](int &state, const int &value) { state += scale * value; },
      [](int &state, const int &other) { state += other; },
      [](int &state) { return std::vector<int>{state}; },
      ReducerProperties::parallel());

  program p(ctx);
  p.rule(output(result), aggregate(result, scaled_sum, input(x)));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 8;
  compiled.run(options);

  EXPECT_TRUE(output.contains(15150));
  EXPECT_GT(compiled.stats().parallel_aggregate_tasks, 1U);
}

TEST(DatalogTest, CustomReducerIsSerialUntilItDeclaresParallelSafety) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  auto result = ctx.var<int>("result");
  for (int value = 1; value <= 100; ++value)
    input.insert(value);

  auto serial_sum = make_reducible_aggregator<int>(
      x, "serial-sum", [] { return 0; },
      [](int &state, const int &value) { state += value; },
      [](int &state, const int &other) { state += other; },
      [](int &state) { return std::vector<int>{state}; });
  program p(ctx);
  p.rule(output(result), aggregate(result, serial_sum, input(x)));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 8;
  compiled.run(options);

  EXPECT_TRUE(output.contains(5050));
  EXPECT_EQ(compiled.stats().parallel_aggregate_tasks, 0U);
}

TEST(DatalogTest, SupportsExtendedLatticeLibrary) {
  using DualMinimum = dual_lattice<max_lattice<int>>;
  using Product = product_lattice<min_lattice<int>, max_lattice<int>>;
  using Bounded = bounded_set_lattice<2, int>;
  using Constant = constant_propagation_lattice<int>;

  context ctx;
  auto dual = ctx.lattice<int, DualMinimum>("dual");
  auto product = ctx.lattice<int, Product>("product");
  auto bounded = ctx.lattice<int, Bounded>("bounded");
  auto constants = ctx.lattice<int, Constant>("constants");

  dual.insert(1, DualMinimum(max_lattice<int>(10)));
  dual.insert(1, DualMinimum(max_lattice<int>(3)));
  product.insert(1, Product(min_lattice<int>(9), max_lattice<int>(2)));
  product.insert(1, Product(min_lattice<int>(4), max_lattice<int>(8)));
  bounded.insert(1, Bounded::singleton(1));
  bounded.insert(1, Bounded::singleton(2));
  bounded.insert(1, Bounded::singleton(3));
  constants.insert(1, Constant::constant(4));
  constants.insert(1, Constant::constant(5));

  EXPECT_TRUE(dual.contains(1, DualMinimum(max_lattice<int>(3))));
  EXPECT_TRUE(
      product.contains(1, Product(min_lattice<int>(4), max_lattice<int>(8))));
  EXPECT_TRUE(bounded.contains(1, Bounded::top()));
  EXPECT_TRUE(constants.contains(1, Constant::top()));
}

TEST(DatalogTest, SupportsLatticeWithoutKeyColumns) {
  context ctx;
  auto global_minimum = ctx.lattice<min_lattice<int>>("global_minimum");

  global_minimum.insert(min_lattice<int>(9));
  global_minimum.insert(min_lattice<int>(3));
  global_minimum.insert(min_lattice<int>(7));

  ASSERT_EQ(global_minimum.rows().size(), 1U);
  EXPECT_TRUE(global_minimum.contains(min_lattice<int>(3)));
}

TEST(DatalogTest, ThrowingLatticeJoinDoesNotCommitPartialState) {
  context ctx;
  auto input = ctx.relation<int, ThrowingMinimum>("input");
  auto output = ctx.lattice<int, ThrowingMinimum>("output");
  auto key = ctx.var<int>("key");
  auto value = ctx.var<ThrowingMinimum>("value");
  output.insert(1, ThrowingMinimum{10});
  input.insert(1, ThrowingMinimum{5});

  program p(ctx);
  p.rule(output(key, value), input(key, value));
  auto compiled = p.compile();
  ThrowingMinimum::throw_on_join = true;
  EXPECT_THROW(compiled.run(), std::runtime_error);
  EXPECT_TRUE(output.contains(1, ThrowingMinimum{10}));

  ThrowingMinimum::throw_on_join = false;
  compiled.run();
  EXPECT_TRUE(output.contains(1, ThrowingMinimum{5}));
}

TEST(DatalogTest, DebugContractsRejectNonIdempotentLattice) {
  context ctx;
  auto input = ctx.relation<int, NonIdempotentLattice>("input");
  auto output = ctx.lattice<int, NonIdempotentLattice>("output");
  auto key = ctx.var<int>("key");
  auto value = ctx.var<NonIdempotentLattice>("value");
  input.insert(1, NonIdempotentLattice{1});
  program p(ctx);
  p.rule(output(key, value), input(key, value));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.debug_contracts = true;

  EXPECT_THROW(compiled.run(options), std::logic_error);
  EXPECT_TRUE(output.rows().empty());
}

TEST(DatalogTest, RunStateIsResetAfterExpressionFailure) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  bool throw_once = true;
  input.insert(1);

  program p(ctx);
  p.rule(output(lift(
             [&](int value) {
               if (throw_once) {
                 throw_once = false;
                 throw std::runtime_error("expected expression failure");
               }
               return value;
             },
             x)),
         input(x));
  auto compiled = p.compile();
  EXPECT_THROW(compiled.run(), std::runtime_error);
  EXPECT_NO_THROW(compiled.run());
  EXPECT_TRUE(output.contains(1));
}

TEST(DatalogTest, FailedRunRemovesPartialDerivedStateFromDirtySccs) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto intermediate = ctx.relation<int>("intermediate");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  bool fail = true;
  input.insert(1);

  program p(ctx);
  p.rule(intermediate(x), input(x));
  p.rule(output(lift(
             [&](int value) {
               if (fail)
                 throw std::runtime_error("expected expression failure");
               return value;
             },
             x)),
         intermediate(x));
  auto compiled = p.compile();

  EXPECT_THROW(compiled.run(), std::runtime_error);
  EXPECT_TRUE(intermediate.rows().empty());
  EXPECT_TRUE(output.rows().empty());
  EXPECT_TRUE(input.contains(1));

  fail = false;
  EXPECT_EQ(compiled.run(), RunStatus::Completed);
  EXPECT_TRUE(intermediate.contains(1));
  EXPECT_TRUE(output.contains(1));
}

TEST(DatalogTest, RelationMutationDuringRunFailsSafely) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  input.insert(1);

  program p(ctx);
  p.rule(output(lift(
             [input](int value) {
               input.insert(value + 1);
               return value;
             },
             x)),
         input(x));
  auto compiled = p.compile();
  EXPECT_THROW(compiled.run(), std::logic_error);
  EXPECT_EQ(input.rows().size(), 1U);
}

TEST(DatalogTest, ProgramCompilationDuringRunFailsSafely) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  input.insert(1);

  program p(ctx);
  p.rule(output(lift(
             [&p](int value) {
               (void)p.compile();
               return value;
             },
             x)),
         input(x));
  auto compiled = p.compile();

  EXPECT_THROW(compiled.run(), std::logic_error);
  EXPECT_TRUE(output.rows().empty());
  EXPECT_NO_THROW(p.compile());
}

TEST(DatalogTest, FloatingPointSumIsSerialByDefault) {
  context ctx;
  auto input = ctx.relation<double>("input");
  auto output = ctx.relation<double>("output");
  auto x = ctx.var<double>("x");
  auto result = ctx.var<double>("result");
  input.insert(1.0);
  input.insert(2.0);

  program p(ctx);
  p.rule(output(result), aggregate(result, sum(x), input(x)));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 1;
  compiled.run(options);

  EXPECT_TRUE(output.contains(3.0));
  EXPECT_EQ(compiled.stats().parallel_aggregate_tasks, 0U);
}

TEST(DatalogTest, NonCommutativeStringSumIsSerialByDefault) {
  context ctx;
  auto input = ctx.relation<std::string>("input");
  auto output = ctx.relation<std::string>("output");
  auto x = ctx.var<std::string>("x");
  auto result = ctx.var<std::string>("result");
  input.insert("first");
  input.insert("second");

  program p(ctx);
  p.rule(output(result), aggregate(result, sum(x), input(x)));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 1;
  compiled.run(options);

  EXPECT_TRUE(output.contains("firstsecond"));
  EXPECT_EQ(compiled.stats().parallel_aggregate_tasks, 0U);
}

} // namespace
