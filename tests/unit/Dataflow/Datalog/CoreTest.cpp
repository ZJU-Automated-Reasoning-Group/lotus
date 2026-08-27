#include "TestSupport.h"

#include <limits>
#include <memory>

using namespace lotus::datalog;

namespace {

TEST(DatalogTest, EvaluatesNonRecursiveTypedRule) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");

  edge.insert(1, 2);
  edge.insert(2, 3);

  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(path), (std::set<std::tuple<int, int>>{{1, 2}, {2, 3}}));
}

TEST(DatalogTest, ComputesTransitiveClosureWithSemiNaiveRecursion) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");

  edge.insert(1, 2);
  edge.insert(2, 3);
  edge.insert(3, 4);

  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(path), (std::set<std::tuple<int, int>>{
                             {1, 2}, {1, 3}, {1, 4}, {2, 3}, {2, 4}, {3, 4}}));
  EXPECT_GE(compiled.stats().fixpoint_iterations, 1U);
  EXPECT_GE(compiled.stats().index_lookups, 1U);
}

TEST(DatalogTest, HandlesMultipleRecursiveAtomsInOneRule) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");

  edge.insert(1, 2);
  edge.insert(2, 3);
  edge.insert(3, 4);
  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && path(y, z));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(path.contains(1, 4));
  EXPECT_EQ(path.rows().size(), 6U);
}

TEST(DatalogTest, UsesExtensionalFactsAsInitialRecursiveDelta) {
  context ctx;
  auto number = ctx.relation<int>("number");
  auto x = ctx.var<int>("x");

  number.insert(0);
  program p(ctx);
  p.rule(number(x + 1), number(x) && where(x < 3));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(number), (std::set<std::tuple<int>>{{0}, {1}, {2}, {3}}));
}

TEST(DatalogTest, OrdersAcyclicSccsIndependentlyOfRuleDeclarationOrder) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto middle = ctx.relation<int>("middle");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");

  source.insert(9);
  program p(ctx);
  p.rule(result(x), middle(x));
  p.rule(middle(x), source(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(result.contains(9));
}

TEST(DatalogTest, EvaluatesMutuallyRecursiveScc) {
  context ctx;
  auto seed = ctx.relation<int>("seed");
  auto a = ctx.relation<int>("a");
  auto b = ctx.relation<int>("b");
  auto x = ctx.var<int>("x");

  seed.insert(7);
  program p(ctx);
  p.rule(a(x), seed(x));
  p.rule(b(x), a(x));
  p.rule(a(x), b(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(a.contains(7));
  EXPECT_TRUE(b.contains(7));
}

TEST(DatalogTest, EnforcesConstantsAndRepeatedVariableEquality) {
  context ctx;
  auto pair = ctx.relation<int, int>("pair");
  auto diagonal = ctx.relation<int>("diagonal");
  auto selected = ctx.relation<int>("selected");
  auto x = ctx.var<int>("x");

  pair.insert(1, 1);
  pair.insert(1, 2);
  pair.insert(2, 2);

  program p(ctx);
  p.rule(diagonal(x), pair(x, x));
  p.rule(selected(x), pair(1, x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(diagonal), (std::set<std::tuple<int>>{{1}, {2}}));
  EXPECT_EQ(asSet(selected), (std::set<std::tuple<int>>{{1}, {2}}));
}

TEST(DatalogTest, SupportsConditionsAndHeadExpressions) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");

  input.insert(1);
  input.insert(2);
  input.insert(3);

  program p(ctx);
  p.rule(output(x + 10), input(x) && where(x >= 2));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(output), (std::set<std::tuple<int>>{{12}, {13}}));
}

TEST(DatalogTest, WildcardsAreFreshAndDoNotImposeEquality) {
  context ctx;
  auto triple = ctx.relation<int, int, int>("triple");
  auto projected = ctx.relation<int>("projected");
  auto x = ctx.var<int>("x");

  triple.insert(1, 2, 3);
  triple.insert(4, 5, 6);

  program p(ctx);
  p.rule(projected(x), triple(x, _, _));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(projected), (std::set<std::tuple<int>>{{1}, {4}}));
}

TEST(DatalogTest, DeduplicatesFacts) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");

  source.insert(1);
  source.insert(1);
  program p(ctx);
  p.rule(result(x), source(x));
  p.rule(result(x), source(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(result.rows().size(), 1U);
}

TEST(DatalogTest, RejectsUngroundedHeadVariableAtCompileTime) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int, int>("result");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");

  program p(ctx);
  p.rule(result(x, y), source(x));

  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, RejectsUngroundedFilterVariableAtCompileTime) {
  context ctx;
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");

  program p(ctx);
  p.rule(result(x), where(x > 0));

  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, PlannerMovesGroundingAtomBeforeFilter) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");
  source.insert(1);

  program p(ctx);
  p.rule(result(x), where(x > 0) && source(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(result.contains(1));
}

TEST(DatalogTest, ReRunningACompiledProgramIsIdempotent) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");

  edge.insert(1, 2);
  edge.insert(2, 3);
  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  compiled.run();
  const auto first = asSet(path);
  compiled.run();

  EXPECT_EQ(asSet(path), first);
  EXPECT_EQ(compiled.stats().inserted_facts, 0U);
}

TEST(DatalogTest, CompiledProgramKeepsContextStateAlive) {
  std::unique_ptr<compiled_program> compiled;
  {
    context ctx;
    auto input = ctx.relation<int>("input");
    auto output = ctx.relation<int>("output");
    auto x = ctx.var<int>("x");
    input.insert(9);
    program p(ctx);
    p.rule(output(x), input(x));
    compiled = std::make_unique<compiled_program>(p.compile());
  }

  EXPECT_NO_THROW(compiled->run());
  EXPECT_EQ(compiled->stats().total_facts, 2U);
}

TEST(DatalogTest, RejectsCrossContextRules) {
  context first;
  context second;
  auto lhs = first.relation<int>("lhs");
  auto rhs = second.relation<int>("rhs");
  auto x = first.var<int>("x");
  auto y = second.var<int>("y");

  program p(first);
  EXPECT_THROW(p.rule(lhs(x), rhs(y)), std::invalid_argument);
}

TEST(DatalogTest, SupportsStringColumnsWithoutRuntimeTypeDispatchInApi) {
  context ctx;
  auto person = ctx.relation<int, std::string>("person");
  auto named = ctx.relation<int>("named");
  auto id = ctx.var<int>("id");

  person.insert(1, "alice");
  person.insert(2, "bob");
  program p(ctx);
  p.rule(named(id), person(id, "alice"));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(named), (std::set<std::tuple<int>>{{1}}));
}

TEST(DatalogTest, ConstantHeadFactsAreDeduplicated) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto answer = ctx.relation<int>("answer");
  source.insert(1);
  source.insert(2);

  program p(ctx);
  p.rule(answer(42), source(_));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(answer), (std::set<std::tuple<int>>{{42}}));
}

TEST(DatalogTest, SupportsCompoundBooleanConditions) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  for (int value = 0; value < 5; ++value)
    input.insert(value);

  program p(ctx);
  p.rule(output(x), input(x) && where((x > 1) && (x < 4)));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(output), (std::set<std::tuple<int>>{{2}, {3}}));
}

TEST(DatalogTest, SupportsNestedArithmeticHeadExpressions) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  input.insert(3);

  program p(ctx);
  p.rule(output((x * 2) + 1), input(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(output.contains(7));
}

TEST(DatalogTest, SupportsScalarOnLeftSideOfExpression) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  input.insert(4);

  program p(ctx);
  p.rule(output(10 - x), input(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(output.contains(6));
}

TEST(DatalogTest, ComputesClosureForCyclicInputGraph) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");
  edge.insert(1, 2);
  edge.insert(2, 1);

  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(path.rows().size(), 4U);
  EXPECT_TRUE(path.contains(1, 1));
  EXPECT_TRUE(path.contains(2, 2));
}

TEST(DatalogTest, IncorporatesNewExtensionalFactsOnLaterRun) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");
  edge.insert(1, 2);

  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  compiled.run();
  edge.insert(2, 3);
  compiled.run();

  EXPECT_TRUE(path.contains(1, 3));
}

TEST(DatalogTest, RejectsRunningAfterContextSchemaChanges) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  input.insert(1);

  program p(ctx);
  p.rule(output(x), input(x));
  auto compiled = p.compile();
  compiled.run();

  auto unrelated = ctx.relation<int>("unrelated");
  unrelated.insert(2);
  EXPECT_THROW(compiled.run(), std::logic_error);
  EXPECT_TRUE(output.contains(1));
}

TEST(DatalogTest, ReRunOnlyEvaluatesDependentSccBranches) {
  context ctx;
  auto left_input = ctx.relation<int>("left_input");
  auto left_output = ctx.relation<int>("left_output");
  auto right_input = ctx.relation<int>("right_input");
  auto right_output = ctx.relation<int>("right_output");
  auto x = ctx.var<int>("x");
  left_input.insert(1);
  right_input.insert(10);

  program p(ctx);
  p.rule(left_output(x), left_input(x));
  p.rule(right_output(x), right_input(x));
  auto compiled = p.compile();
  compiled.run();

  left_input.insert(2);
  compiled.run();
  EXPECT_TRUE(left_output.contains(2));
  EXPECT_TRUE(right_output.contains(10));
  EXPECT_EQ(compiled.stats().rule_evaluations, 1U);
}

TEST(DatalogTest, EvaluatesThreeRelationRecursiveScc) {
  context ctx;
  auto seed = ctx.relation<int>("seed");
  auto a = ctx.relation<int>("a");
  auto b = ctx.relation<int>("b");
  auto c = ctx.relation<int>("c");
  auto x = ctx.var<int>("x");
  seed.insert(5);

  program p(ctx);
  p.rule(a(x), seed(x));
  p.rule(b(x), a(x));
  p.rule(c(x), b(x));
  p.rule(a(x), c(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(a.contains(5));
  EXPECT_TRUE(b.contains(5));
  EXPECT_TRUE(c.contains(5));
}

TEST(DatalogTest, BuildsMultiColumnRuntimeIndexMask) {
  context ctx;
  auto key = ctx.relation<int, int>("key");
  auto triple = ctx.relation<int, int, int>("triple");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");
  key.insert(1, 3);
  triple.insert(1, 10, 2);
  triple.insert(1, 11, 3);
  triple.insert(2, 12, 3);

  program p(ctx);
  p.rule(output(y), key(x, z) && triple(x, y, z));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(output), (std::set<std::tuple<int>>{{11}}));
  EXPECT_GE(compiled.stats().index_lookups, 1U);
}

TEST(DatalogTest, EmptyInputsProduceNoDerivedFacts) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");

  program p(ctx);
  p.rule(result(x), source(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(result.rows().empty());
}

TEST(DatalogTest, SupportsTypedNullaryPredicates) {
  context ctx;
  auto ready = ctx.relation<>("ready");

  EXPECT_FALSE(ready.contains());
  ready.insert();
  ready.insert();

  EXPECT_TRUE(ready.contains());
  ASSERT_EQ(ready.rows().size(), 1U);
  EXPECT_EQ(ready.rows().front(), std::tuple<>());
}

TEST(DatalogTest, MultipleRulesFormSetUnion) {
  context ctx;
  auto left = ctx.relation<int>("left");
  auto right = ctx.relation<int>("right");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");
  left.insert(1);
  right.insert(2);

  program p(ctx);
  p.rule(result(x), left(x));
  p.rule(result(x), right(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(asSet(result), (std::set<std::tuple<int>>{{1}, {2}}));
}

TEST(DatalogTest, FalseConditionSuppressesHeadInsertion) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");
  source.insert(1);

  program p(ctx);
  p.rule(result(x), source(x) && where(x < 0));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_TRUE(result.rows().empty());
}

TEST(DatalogTest, RejectsWildcardInRuleHead) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int>("result");

  program p(ctx);
  p.rule(result(_), source(_));

  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, RejectsComputedTermsInBodyAtoms) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto result = ctx.relation<int>("result");
  auto x = ctx.var<int>("x");

  program p(ctx);
  p.rule(result(x), source(x + 1));

  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, RejectsDuplicateRelationNames) {
  context ctx;
  ctx.relation<int>("duplicate");

  EXPECT_THROW(ctx.relation<std::string>("duplicate"), std::invalid_argument);
}

TEST(DatalogTest, RejectsNaNInRelationKeys) {
  context ctx;
  auto values = ctx.relation<double>("values");

  EXPECT_THROW(values.insert(std::numeric_limits<double>::quiet_NaN()),
               std::invalid_argument);
  EXPECT_TRUE(values.rows().empty());
}

TEST(DatalogTest, RejectsNaNConstantsInRelationKeys) {
  context ctx;
  auto input = ctx.relation<double>("input");
  auto output = ctx.relation<double>("output");

  program p(ctx);
  p.rule(output(1.0), input(std::numeric_limits<double>::quiet_NaN()));
  EXPECT_THROW(p.compile(), CompileError);
}

TEST(DatalogTest, ExplainsPhysicalPlanAndCollectedOperationProfile) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  edge.insert(1, 2);

  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  auto compiled = p.compile();

  const std::string plan = compiled.explain();
  EXPECT_NE(plan.find("SCC"), std::string::npos);
  EXPECT_NE(plan.find("Scan edge"), std::string::npos);
  EXPECT_NE(plan.find("estimate[input="), std::string::npos);

  ExecutionOptions options;
  options.collect_profile = true;
  compiled.run(options);

  ASSERT_TRUE(compiled.profile().collected);
  ASSERT_EQ(compiled.profile().rules.size(), 1U);
  ASSERT_EQ(compiled.profile().rules[0].operations.size(), 1U);
  EXPECT_EQ(compiled.profile().rules[0].head_candidates, 1U);
  EXPECT_EQ(compiled.profile().rules[0].operations[0].candidate_rows, 1U);
  EXPECT_EQ(compiled.profile().rules[0].operations[0].matched_rows, 1U);

  const std::string analyzed = compiled.explain(ExplainMode::Analyze);
  EXPECT_NE(analyzed.find("actual[invocations="), std::string::npos);
  EXPECT_NE(analyzed.find("head candidates=1"), std::string::npos);
}

TEST(DatalogTest, AnalyzeExplainReportsWhenProfileWasNotCollected) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  input.insert(1);
  program p(ctx);
  p.rule(output(x), input(x));
  auto compiled = p.compile();
  compiled.run();

  EXPECT_FALSE(compiled.profile().collected);
  EXPECT_NE(compiled.explain(ExplainMode::Analyze)
                .find("profile: not collected"),
            std::string::npos);
}

TEST(DatalogTest, TypedColumnsUseLessPayloadStorageThanDynamicCells) {
  context ctx;
  auto facts = ctx.relation<std::uint64_t, std::uint64_t>("facts");
  constexpr std::size_t FACT_COUNT = 1024;
  for (std::size_t index = 0; index < FACT_COUNT; ++index)
    facts.insert(index, index + 1);

  program p(ctx);
  auto compiled = p.compile();
  compiled.run();

  EXPECT_EQ(compiled.stats().total_facts, FACT_COUNT);
  EXPECT_LT(compiled.stats().tuple_memory_bytes,
            FACT_COUNT * 2 * sizeof(std::any));
  EXPECT_GT(compiled.stats().uniqueness_memory_bytes, 0U);
  EXPECT_GE(compiled.stats().base_memory_bytes, FACT_COUNT);
}

TEST(DatalogTest, PromotingDerivedFactToBaseSurvivesRecomputation) {
  context ctx;
  auto source = ctx.relation<int>("source");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  source.insert(1);
  program p(ctx);
  p.rule(output(x), source(x));
  auto compiled = p.compile();
  compiled.run();
  ASSERT_TRUE(output.contains(1));

  output.insert(1);
  source.insert(2);
  compiled.run();

  EXPECT_TRUE(output.contains(1));
  EXPECT_TRUE(output.contains(2));
  EXPECT_EQ(output.rows().size(), 2U);
}

} // namespace
