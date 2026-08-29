#include "TestSupport.h"

#include <algorithm>

using namespace lotus::datalog;

namespace {

TEST(DatalogTest, EvaluatesStratifiedNegationAsAntiJoin) {
  context ctx;
  auto node = ctx.relation<int>("node");
  auto parent = ctx.relation<int, int>("parent");
  auto root = ctx.relation<int>("root");
  auto x = ctx.var<int>("x");
  node.insert(1);
  node.insert(2);
  node.insert(3);
  parent.insert(1, 2);
  parent.insert(1, 3);

  program p(ctx);
  p.rule(root(x), node(x) && neg(parent(_, x)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(root), (std::set<std::tuple<int>>{{1}}));
}

TEST(DatalogTest, ReRunRetractsNegatedResultsAfterNewBaseFacts) {
  context ctx;
  auto candidate = ctx.relation<int>("candidate");
  auto blocked = ctx.relation<int>("blocked");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");
  candidate.insert(1);

  program p(ctx);
  p.rule(result(x), candidate(x) && neg(blocked(x)));
  auto compiled = p.compile();
  compiled.run();
  ASSERT_TRUE(result.contains(1));

  blocked.insert(1);
  compiled.run();
  EXPECT_FALSE(result.contains(1));
  EXPECT_TRUE(result.rows().empty());
  EXPECT_GT(compiled.stats().incremental_sccs, 0U);
  EXPECT_GT(compiled.stats().rebuilt_sccs, 0U);
}

TEST(DatalogTest, PlannerMovesGroundingAtomBeforeNegation) {
  context ctx;
  auto person = ctx.relation<int>("person");
  auto dead = ctx.relation<int>("dead");
  auto alive = ctx.relation<int>("alive");
  auto x = ctx.var<int>("x");
  person.insert(1);
  person.insert(2);
  dead.insert(2);

  program p(ctx);
  p.rule(alive(x), neg(dead(x)) && person(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(alive), (std::set<std::tuple<int>>{{1}}));
}

TEST(DatalogTest, RejectsUnstratifiableNegativeCycle) {
  context ctx;
  auto universe = ctx.relation<int>("universe");
  auto a = ctx.relation<int>("a");
  auto b = ctx.relation<int>("b");
  auto x = ctx.var<int>("x");

  program p(ctx);
  p.rule(a(x), universe(x) && neg(b(x)));
  p.rule(b(x), universe(x) && neg(a(x)));

  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, ComputesGroupedMeanAggregate) {
  context ctx;
  auto enrolled = ctx.relation<int>("enrolled");
  auto grade = ctx.relation<int, int, int>("grade");
  auto average = ctx.relation<int, double>("average");
  auto student = ctx.var<int>("student");
  auto score = ctx.var<int>("score");
  auto result = ctx.var<double>("result");
  enrolled.insert(1);
  enrolled.insert(2);
  grade.insert(1, 10, 80);
  grade.insert(1, 11, 100);
  grade.insert(2, 12, 70);

  program p(ctx);
  p.rule(average(student, result),
         enrolled(student) &&
             aggregate(result, mean(score), grade(student, _, score)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(average.contains(1, 90.0));
  EXPECT_TRUE(average.contains(2, 70.0));
}

TEST(DatalogTest, CountAggregateProducesZeroForEmptyGroup) {
  context ctx;
  auto person = ctx.relation<int>("person");
  auto grade = ctx.relation<int, int>("grade");
  auto counts = ctx.relation<int, std::size_t>("counts");
  auto student = ctx.var<int>("student");
  auto count_value = ctx.var<std::size_t>("count");
  person.insert(1);
  person.insert(2);
  grade.insert(1, 90);

  program p(ctx);
  p.rule(counts(student, count_value),
         person(student) && aggregate(count_value, count(), grade(student, _)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(counts.contains(1, 1));
  EXPECT_TRUE(counts.contains(2, 0));
}

TEST(DatalogTest, ReRunReplacesAggregateResultsAfterNewBaseFacts) {
  context ctx;
  auto group = ctx.relation<int>("group");
  auto value = ctx.relation<int, int>("value");
  auto result = ctx.relation<int, std::size_t>("result");
  auto key = ctx.var<int>("key");
  auto count_value = ctx.var<std::size_t>("count_value");
  group.insert(1);
  group.insert(2);
  value.insert(1, 10);
  value.insert(2, 30);

  program p(ctx);
  p.rule(result(key, count_value),
         group(key) && aggregate(count_value, count(), value(key, _)));
  auto compiled = p.compile();
  compiled.run();
  ASSERT_TRUE(result.contains(1, 1));
  ASSERT_TRUE(result.contains(2, 1));

  value.insert(1, 20);
  compiled.run();
  EXPECT_FALSE(result.contains(1, 1));
  EXPECT_TRUE(result.contains(1, 2));
  EXPECT_TRUE(result.contains(2, 1));
  EXPECT_EQ(result.rows().size(), 2U);
  EXPECT_GT(compiled.stats().incremental_sccs, 0U);
  EXPECT_EQ(compiled.stats().rebuilt_sccs, 0U);
  EXPECT_EQ(compiled.stats().incremental_aggregate_groups, 1U);
}

TEST(DatalogTest, AggregatesDirectlyOverJoinSubplan) {
  context ctx;
  auto person = ctx.relation<int>("person");
  auto grade = ctx.relation<int, int, int>("grade");
  auto curve = ctx.relation<int, int>("curve");
  auto total = ctx.relation<int, int>("total");
  auto student = ctx.var<int>("student");
  auto course = ctx.var<int>("course");
  auto score = ctx.var<int>("score");
  auto bonus = ctx.var<int>("bonus");
  auto result = ctx.var<int>("result");
  person.insert(1);
  grade.insert(1, 10, 80);
  grade.insert(1, 11, 90);
  curve.insert(10, 5);
  curve.insert(11, 2);

  program p(ctx);
  p.rule(total(student, result),
         person(student) &&
             aggregate(result, sum(score + bonus),
                       grade(student, course, score) && curve(course, bonus)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(total.contains(1, 177));
  EXPECT_EQ(total.rows().size(), 1U);
}

TEST(DatalogTest, SupportsDeclaredMonotoneRecursiveAggregateIntoLattice) {
  context ctx;
  auto level =
      ctx.lattice<max_lattice<int>>("level", FunctionProperties::parallel());
  auto current = ctx.var<max_lattice<int>>("current");
  auto result = ctx.var<int>("result");
  auto current_value = lift(
      FunctionProperties::parallel(),
      [](max_lattice<int> value) { return value.value(); }, current);
  level.insert(max_lattice<int>(0));

  program p(ctx);
  p.rule(level(lift(
             FunctionProperties::parallel(),
             [](int value) { return max_lattice<int>(value); }, result)),
         aggregate(result, maximum(current_value + 1).monotone(),
                   level(current) && where(current_value < 3)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(level.contains(max_lattice<int>(3)));
  EXPECT_EQ(level.rows().size(), 1U);
  EXPECT_GT(compiled.stats().fixpoint_iterations, 1U);
}

TEST(DatalogTest, RejectsMonotoneAggregateWithSetHead) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  auto result = ctx.var<int>("result");
  program p(ctx);
  p.rule(output(result), aggregate(result, maximum(x).monotone(), input(x)));

  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, SupportsGenericBlockingAggregator) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  auto median = ctx.var<int>("median");
  input.insert(1);
  input.insert(9);
  input.insert(4);

  auto median_aggregator =
      make_aggregator<int>(x, "median", [](const std::vector<int> &values) {
        std::vector<int> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        if (sorted.empty())
          return std::vector<int>{};
        return std::vector<int>{sorted[sorted.size() / 2]};
      });
  program p(ctx);
  p.rule(output(median), aggregate(median, median_aggregator, input(x)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(output.contains(4));
}

TEST(DatalogTest, RejectsAggregateDependencyCycle) {
  context ctx;
  auto a = ctx.relation<int>("a");
  auto b = ctx.relation<int>("b");
  auto x = ctx.var<int>("x");
  auto aggregate_value = ctx.var<int>("aggregate_value");

  program p(ctx);
  p.rule(a(aggregate_value), aggregate(aggregate_value, maximum(x), b(x)));
  p.rule(b(x), a(x));

  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, LatticeRelationJoinsValuesByKey) {
  context ctx;
  auto distance = ctx.lattice<int, int, min_lattice<int>>("distance");

  distance.insert(1, 2, min_lattice<int>(100));
  distance.insert(1, 2, min_lattice<int>(70));
  distance.insert(1, 2, min_lattice<int>(90));

  ASSERT_EQ(distance.rows().size(), 1U);
  EXPECT_TRUE(distance.contains(1, 2, min_lattice<int>(70)));
}

TEST(DatalogTest, ErasesBaseLatticeKey) {
  context ctx;
  auto distance = ctx.lattice<int, min_lattice<int>>("distance");
  distance.insert(1, min_lattice<int>(10));

  EXPECT_TRUE(distance.erase(1, min_lattice<int>(10)));
  EXPECT_TRUE(distance.rows().empty());
  EXPECT_FALSE(distance.erase(1, min_lattice<int>(10)));
}

TEST(DatalogTest, LatticeSemiNaiveCoalescesCandidatesPerKey) {
  context ctx;
  auto edge = ctx.relation<int, int, int>("edge");
  auto distance = ctx.lattice<int, int, min_lattice<int>>("distance");
  auto source = ctx.var<int>("source");
  auto middle = ctx.var<int>("middle");
  auto target = ctx.var<int>("target");
  auto weight = ctx.var<int>("weight");
  auto current = ctx.var<min_lattice<int>>("current");
  edge.insert(1, 2, 70);
  edge.insert(1, 2, 50);
  edge.insert(1, 2, 60);
  distance.insert(1, 1, min_lattice<int>(0));

  program p(ctx);
  p.rule(distance(source, target, current + weight),
         distance(source, middle, current) && edge(middle, target, weight));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(distance.contains(1, 2, min_lattice<int>(50)));
  EXPECT_EQ(distance.rows().size(), 2U);
  EXPECT_EQ(compiled.stats().inserted_facts, 1U);
}

TEST(DatalogTest, LatticeShortestPathReachesFixedPoint) {
  context ctx;
  auto edge = ctx.relation<int, int, int>("edge");
  auto distance = ctx.lattice<int, int, min_lattice<int>>("distance");
  auto source = ctx.var<int>("source");
  auto middle = ctx.var<int>("middle");
  auto target = ctx.var<int>("target");
  auto weight = ctx.var<int>("weight");
  auto current = ctx.var<min_lattice<int>>("current");
  edge.insert(1, 2, 10);
  edge.insert(2, 3, 5);
  edge.insert(1, 3, 30);
  distance.insert(1, 1, min_lattice<int>(0));

  program p(ctx);
  p.rule(distance(source, target, current + weight),
         distance(source, middle, current) && edge(middle, target, weight));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(distance.contains(1, 3, min_lattice<int>(15)));
}

TEST(DatalogTest, GreedyPlannerStartsWithLowerEstimatedCardinality) {
  context ctx;
  auto large = ctx.relation<int, int>("large");
  auto small = ctx.relation<int>("small");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  for (int value = 0; value < 100; ++value)
    large.insert(value, value);
  small.insert(42);

  program p(ctx);
  p.rule(result(x), large(x, y) && small(y));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(result.contains(42));
  EXPECT_GT(compiled.stats().planned_reorders, 0U);
  EXPECT_GT(compiled.stats().index_lookups, 0U);
}

} // namespace
