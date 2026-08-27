#include "Dataflow/Datalog/Frontend/Frontend.h"

#include "Dataflow/Datalog/Core/Error.h"

#include <string>

#include <gtest/gtest.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

using namespace lotus::datalog;

namespace {

std::string executeJson(llvm::StringRef input, bool validate_only = false) {
  std::string output;
  llvm::raw_string_ostream stream(output);
  frontend::RunOptions options;
  options.validate_only = validate_only;
  frontend::executeJson(input, options, stream);
  stream.flush();
  return output;
}

std::string executeInput(llvm::StringRef input, frontend::InputFormat format) {
  std::string output;
  llvm::raw_string_ostream stream(output);
  frontend::RunOptions options;
  frontend::executeInput(input, format, options, stream);
  stream.flush();
  return output;
}

std::string executeInput(llvm::StringRef input, frontend::InputFormat format,
                         const frontend::RunOptions &options) {
  std::string output;
  llvm::raw_string_ostream stream(output);
  frontend::executeInput(input, format, options, stream);
  stream.flush();
  return output;
}

std::string executeInputs(llvm::ArrayRef<frontend::SourceUnit> inputs,
                          frontend::InputFormat format) {
  std::string output;
  llvm::raw_string_ostream stream(output);
  frontend::RunOptions options;
  frontend::executeInputs(inputs, format, options, stream);
  stream.flush();
  return output;
}

llvm::json::Value parseOutput(const std::string &output) {
  auto value = llvm::json::parse(output);
  if (!value)
    throw std::runtime_error("invalid test output JSON");
  return std::move(*value);
}

const llvm::json::Array &outputRows(const llvm::json::Value &output,
                                    llvm::StringRef relation) {
  const llvm::json::Object *root = output.getAsObject();
  if (!root)
    throw std::runtime_error("test output is not an object");
  const llvm::json::Object *relations = root->getObject("relations");
  if (!relations)
    throw std::runtime_error("test output has no relations");
  const llvm::json::Array *rows = relations->getArray(relation);
  if (!rows)
    throw std::runtime_error("test output has no requested relation");
  return *rows;
}

TEST(DatalogJsonFrontendTest, RequiresVersionedSchema) {
  EXPECT_THROW(executeJson(R"json({"relations": []})json", true),
               std::invalid_argument);
  EXPECT_THROW(
      executeJson(R"json({"schema_version": 2, "relations": []})json", true),
      std::invalid_argument);

  const std::string output =
      executeJson(R"json({"schema_version": 1, "relations": []})json", true);
  auto parsed = llvm::json::parse(output);
  ASSERT_TRUE(static_cast<bool>(parsed));
  const llvm::json::Object *root = parsed->getAsObject();
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->getInteger("schema_version").getValueOr(0), 1);
  EXPECT_EQ(root->getString("status").getValueOr(""), "valid");
}

TEST(DatalogJsonFrontendTest, SupportsNullaryPredicates) {
  constexpr llvm::StringLiteral program = R"json({
    "schema_version": 1,
    "relations": [
      {"name": "seed", "columns": ["u64"], "facts": [[1]]},
      {"name": "query_result", "columns": []}
    ],
    "rules": [{
      "head": {"relation": "query_result", "args": []},
      "body": [{"atom": {"relation": "seed", "args": [1]}}]
    }],
    "outputs": ["query_result"]
  })json";

  const std::string output = executeJson(program);
  auto parsed = llvm::json::parse(output);
  ASSERT_TRUE(static_cast<bool>(parsed));
  const llvm::json::Object *root = parsed->getAsObject();
  ASSERT_NE(root, nullptr);
  const llvm::json::Object *relations = root->getObject("relations");
  ASSERT_NE(relations, nullptr);
  const llvm::json::Array *rows = relations->getArray("query_result");
  ASSERT_NE(rows, nullptr);
  ASSERT_EQ(rows->size(), 1U);
  const llvm::json::Array *row = (*rows)[0].getAsArray();
  ASSERT_NE(row, nullptr);
  EXPECT_TRUE(row->empty());
}

TEST(DatalogJsonFrontendTest, EmitsPlanAndAnalyzeDiagnostics) {
  constexpr llvm::StringLiteral program = R"json({
    "schema_version": 1,
    "relations": [
      {"name": "input", "columns": ["i64"], "facts": [[1]]},
      {"name": "output", "columns": ["i64"]}
    ],
    "rules": [{
      "head": {"relation": "output", "args": ["$x"]},
      "body": [{"atom": {"relation": "input", "args": ["$x"]}}]
    }]
  })json";

  frontend::RunOptions explain_options;
  explain_options.explain = true;
  const llvm::json::Value explained = parseOutput(executeInput(
      program, frontend::InputFormat::Json, explain_options));
  const llvm::json::Object *explain_root = explained.getAsObject();
  ASSERT_NE(explain_root, nullptr);
  EXPECT_EQ(explain_root->getString("status").getValueOr(""), "explained");
  EXPECT_NE(explain_root->getString("plan").getValueOr("").find("Scan input"),
            llvm::StringRef::npos);

  explain_options.explain_analyze = true;
  const llvm::json::Value analyzed = parseOutput(executeInput(
      program, frontend::InputFormat::Json, explain_options));
  const llvm::json::Object *analyze_root = analyzed.getAsObject();
  ASSERT_NE(analyze_root, nullptr);
  EXPECT_NE(analyze_root->getString("plan").getValueOr("").find("actual["),
            llvm::StringRef::npos);
  EXPECT_NE(analyze_root->getObject("stats"), nullptr);
}

TEST(DatalogJsonFrontendTest, AcceptsTheCompleteU64RangeAsDecimalStrings) {
  constexpr llvm::StringLiteral program = R"json({
    "schema_version": 1,
    "relations": [
      {"name": "value", "columns": ["u64"],
       "facts": [["18446744073709551615"]]}
    ],
    "outputs": ["value"]
  })json";

  const llvm::json::Value parsed = parseOutput(executeJson(program));
  const llvm::json::Value &value =
      outputRows(parsed, "value")[0].getAsArray()->front();
  EXPECT_EQ(value.getAsString().getValueOr(""), "18446744073709551615");
}

TEST(DatalogJsonFrontendTest, ReportsCheckedIntegerOverflow) {
  constexpr llvm::StringLiteral program = R"json({
    "schema_version": 1,
    "relations": [
      {"name": "input", "columns": ["i64"],
       "facts": [[9223372036854775807]]},
      {"name": "output", "columns": ["i64"]}
    ],
    "rules": [{
      "head": {"relation": "output",
               "args": [{"op": "+", "args": ["$x", 1]}]},
      "body": [{"atom": {"relation": "input", "args": ["$x"]}}]
    }]
  })json";

  try {
    (void)executeJson(program);
    FAIL() << "overflowing JSON expression should fail";
  } catch (const EvaluationError &error) {
    EXPECT_EQ(error.code(), EvaluationErrorCode::IntegerOverflow);
    EXPECT_EQ(error.expression(), "+");

    std::string output;
    llvm::raw_string_ostream stream(output);
    frontend::printJsonError(error, stream);
    stream.flush();
    auto parsed = llvm::json::parse(output);
    ASSERT_TRUE(static_cast<bool>(parsed));
    const llvm::json::Object *root = parsed->getAsObject();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->getString("status").getValueOr(""), "error");
    const llvm::json::Object *detail = root->getObject("error");
    ASSERT_NE(detail, nullptr);
    EXPECT_EQ(detail->getString("category").getValueOr(""), "evaluation");
    EXPECT_EQ(detail->getString("code").getValueOr(""), "integer_overflow");
  }
}

TEST(DatalogJsonFrontendTest, ChecksDivisionErrorsWithoutCppUndefinedBehavior) {
  constexpr llvm::StringLiteral program = R"json({
    "schema_version": 1,
    "relations": [
      {"name": "input", "columns": ["i64"],
       "facts": [[-9223372036854775808]]},
      {"name": "output", "columns": ["i64"]}
    ],
    "rules": [{
      "head": {"relation": "output",
               "args": [{"op": "/", "args": ["$x", -1]}]},
      "body": [{"atom": {"relation": "input", "args": ["$x"]}}]
    }]
  })json";

  try {
    (void)executeJson(program);
    FAIL() << "overflowing JSON division should fail";
  } catch (const EvaluationError &error) {
    EXPECT_EQ(error.code(), EvaluationErrorCode::IntegerOverflow);
    EXPECT_EQ(error.expression(), "/");
  }
}

TEST(DatalogJsonFrontendTest, ReportsDivisionByZero) {
  constexpr llvm::StringLiteral program = R"json({
    "schema_version": 1,
    "relations": [
      {"name": "input", "columns": ["i64"], "facts": [[7]]},
      {"name": "output", "columns": ["i64"]}
    ],
    "rules": [{
      "head": {"relation": "output",
               "args": [{"op": "/", "args": ["$x", 0]}]},
      "body": [{"atom": {"relation": "input", "args": ["$x"]}}]
    }]
  })json";

  try {
    (void)executeJson(program);
    FAIL() << "JSON division by zero should fail";
  } catch (const EvaluationError &error) {
    EXPECT_EQ(error.code(), EvaluationErrorCode::DivisionByZero);
    EXPECT_EQ(error.expression(), "/");
  }
}

TEST(DatalogJsonFrontendTest, ChecksAggregateArithmetic) {
  constexpr llvm::StringLiteral program = R"json({
    "schema_version": 1,
    "relations": [
      {"name": "input", "columns": ["i64"],
       "facts": [[9223372036854775807], [1]]},
      {"name": "output", "columns": ["i64"]}
    ],
    "rules": [{
      "head": {"relation": "output", "args": ["$sum"]},
      "body": [{"aggregate": {
        "op": "sum", "output": "$sum", "value": "$x",
        "source": {"relation": "input", "args": ["$x"]}
      }}]
    }]
  })json";

  try {
    (void)executeJson(program);
    FAIL() << "overflowing JSON aggregate should fail";
  } catch (const EvaluationError &error) {
    EXPECT_EQ(error.code(), EvaluationErrorCode::IntegerOverflow);
    EXPECT_EQ(error.expression(), "aggregate-sum");
  }
}

TEST(DatalogJsonFrontendTest, RejectsNonFiniteFloatingResults) {
  constexpr llvm::StringLiteral program = R"json({
    "schema_version": 1,
    "relations": [
      {"name": "input", "columns": ["f64"], "facts": [[1e308]]},
      {"name": "output", "columns": ["f64"]}
    ],
    "rules": [{
      "head": {"relation": "output",
               "args": [{"op": "*", "args": ["$x", "$x"]}]},
      "body": [{"atom": {"relation": "input", "args": ["$x"]}}]
    }]
  })json";

  try {
    (void)executeJson(program);
    FAIL() << "non-finite JSON expression should fail";
  } catch (const EvaluationError &error) {
    EXPECT_EQ(error.code(), EvaluationErrorCode::NonFiniteFloatingPoint);
    EXPECT_EQ(error.expression(), "*");
  }
}

TEST(DatalogLotusFrontendTest, RunsNativeDatalogSyntax) {
  constexpr llvm::StringLiteral program = R"dl(
    .decl edge(src: u64, dst: u64)
    .decl path(src: u64, dst: u64)
    .decl reachable()

    edge(1, 2).
    edge(2, 4).
    path(X, Y) :- edge(X, Y).
    path(X, Z) :- path(X, Y), edge(Y, Z).
    reachable :- path(1, 4).

    .output path
    .output reachable
  )dl";

  const std::string output =
      executeInput(program, frontend::InputFormat::Datalog);
  const llvm::json::Value parsed = parseOutput(output);
  EXPECT_EQ(outputRows(parsed, "path").size(), 3U);
  const llvm::json::Array &reachable = outputRows(parsed, "reachable");
  ASSERT_EQ(reachable.size(), 1U);
  ASSERT_NE(reachable[0].getAsArray(), nullptr);
  EXPECT_TRUE(reachable[0].getAsArray()->empty());
}

TEST(DatalogLotusFrontendTest, ComposesMultipleSourceUnits) {
  constexpr llvm::StringLiteral schema = R"dl(
    .decl edge(src: u64, dst: u64)
    .decl path(src: u64, dst: u64)
  )dl";
  constexpr llvm::StringLiteral facts = R"dl(
    edge(1, 2).
    edge(2, 3).
  )dl";
  constexpr llvm::StringLiteral rules = R"dl(
    path(X, Y) :- edge(X, Y).
    path(X, Z) :- path(X, Y), edge(Y, Z).
    .output path
  )dl";
  const frontend::SourceUnit sources[] = {
      {"schema.dl", schema},
      {"facts/graph.dl", facts},
      {"rules/path.dl", rules},
  };

  const llvm::json::Value parsed =
      parseOutput(executeInputs(sources, frontend::InputFormat::Datalog));
  EXPECT_EQ(outputRows(parsed, "path").size(), 3U);
}

TEST(DatalogLotusFrontendTest,
     ResolvesDeclarationsAfterFactsAndRulesAcrossSourceUnits) {
  constexpr llvm::StringLiteral facts = R"dl(edge(1, 2).)dl";
  constexpr llvm::StringLiteral rules = R"dl(
    path(X, Y) :- edge(X, Y).
    .output path
  )dl";
  constexpr llvm::StringLiteral schema = R"dl(
    .decl edge(src: u64, dst: u64)
    .decl path(src: u64, dst: u64)
  )dl";
  const frontend::SourceUnit sources[] = {
      {"facts/graph.dl", facts},
      {"rules/path.dl", rules},
      {"schema.dl", schema},
  };

  const llvm::json::Value parsed =
      parseOutput(executeInputs(sources, frontend::InputFormat::Datalog));
  ASSERT_EQ(outputRows(parsed, "path").size(), 1U);
}

TEST(DatalogLotusFrontendTest, ReportsTheOriginatingSourceLocation) {
  constexpr llvm::StringLiteral schema = R"dl(
    .decl edge(x: i64)
    .decl path(x: i64)
  )dl";
  constexpr llvm::StringLiteral rules = R"dl(

    path(X) :- missing(X).
  )dl";
  const frontend::SourceUnit sources[] = {
      {"schema.dl", schema},
      {"rules/reachability.dl", rules},
  };

  try {
    (void)executeInputs(sources, frontend::InputFormat::Datalog);
    FAIL() << "unknown relation should fail";
  } catch (const frontend::FrontendError &error) {
    EXPECT_EQ(error.code(), "invalid_program");
    EXPECT_EQ(error.source(), "rules/reachability.dl");
    EXPECT_EQ(error.line(), 3U);
    EXPECT_EQ(error.column(), 16U);

    std::string output;
    llvm::raw_string_ostream stream(output);
    frontend::printJsonError(error, stream);
    stream.flush();
    const llvm::json::Value parsed = parseOutput(output);
    const llvm::json::Object *root = parsed.getAsObject();
    ASSERT_NE(root, nullptr);
    const llvm::json::Object *detail = root->getObject("error");
    ASSERT_NE(detail, nullptr);
    EXPECT_EQ(detail->getString("source").getValueOr(""),
              "rules/reachability.dl");
    EXPECT_EQ(detail->getInteger("line").getValueOr(0), 3);
  }
}

TEST(DatalogLotusFrontendTest, AcceptsTheCompleteU64Range) {
  constexpr llvm::StringLiteral program = R"dl(
    .decl value(x: u64)
    value(18446744073709551615).
    .output value
  )dl";

  const llvm::json::Value parsed =
      parseOutput(executeInput(program, frontend::InputFormat::Datalog));
  EXPECT_EQ(outputRows(parsed, "value")[0]
                .getAsArray()
                ->front()
                .getAsString()
                .getValueOr(""),
            "18446744073709551615");
}

TEST(DatalogLotusFrontendTest, ResolvesIncludesWithoutLibraryFilesystemAccess) {
  constexpr llvm::StringLiteral root = R"dl(
    .include "schema.dl"
    .include "facts/graph.dl"
    .include "rules/path.dl"
  )dl";
  frontend::RunOptions options;
  options.source_resolver =
      [](llvm::StringRef, llvm::StringRef requested)
      -> std::optional<frontend::OwnedSourceUnit> {
    if (requested == "schema.dl")
      return frontend::OwnedSourceUnit{
          "project/schema.dl",
          ".decl edge(x: u64, y: u64)\n.decl path(x: u64, y: u64)"};
    if (requested == "facts/graph.dl")
      return frontend::OwnedSourceUnit{"project/facts/graph.dl",
                                       "edge(1, 2)."};
    if (requested == "rules/path.dl")
      return frontend::OwnedSourceUnit{
          "project/rules/path.dl",
          "path(X, Y) :- edge(X, Y).\n.output path"};
    return std::nullopt;
  };
  std::string output;
  llvm::raw_string_ostream stream(output);
  frontend::executeInputs(frontend::SourceUnit{"project/main.dl", root},
                          frontend::InputFormat::Datalog, options, stream);
  stream.flush();

  const llvm::json::Value parsed = parseOutput(output);
  EXPECT_EQ(outputRows(parsed, "path").size(), 1U);
}

TEST(DatalogLotusFrontendTest,
     SupportsExpressionsFiltersNegationAndMultiHeads) {
  constexpr llvm::StringLiteral program = R"dl(
    .decl input(value: i64)
    .decl blocked(value: i64)
    .decl adjusted(value: i64)
    .decl copied(value: i64)
    .decl text(value: string)
    .decl text_out(value: string)
    .decl flag(value: bool)
    .decl flag_out(value: bool)

    input(-2).
    input(0).
    input(1).
    input(2).
    blocked(2).
    text("$lotus").
    flag(true).

    adjusted(X * 2 + 1); copied(X) :-
      input(X), not blocked(X), where X >= 0 && X < 2.
    text_out(S + "-datalog") :- text(S), where "$lotus" == S.
    flag_out(!B) :- flag(B), where B == true || false.

    .output adjusted
    .output copied
    .output text_out
    .output flag_out
  )dl";

  const llvm::json::Value parsed =
      parseOutput(executeInput(program, frontend::InputFormat::Datalog));
  const llvm::json::Array &adjusted = outputRows(parsed, "adjusted");
  ASSERT_EQ(adjusted.size(), 2U);
  EXPECT_EQ(adjusted[0].getAsArray()->front().getAsInteger().getValueOr(-1), 1);
  EXPECT_EQ(adjusted[1].getAsArray()->front().getAsInteger().getValueOr(-1), 3);
  EXPECT_EQ(outputRows(parsed, "copied").size(), 2U);
  EXPECT_EQ(outputRows(parsed, "text_out").size(), 1U);
  EXPECT_EQ(outputRows(parsed, "flag_out").size(), 1U);
  EXPECT_FALSE(outputRows(parsed, "flag_out")[0]
                   .getAsArray()
                   ->front()
                   .getAsBoolean()
                   .getValueOr(true));
}

TEST(DatalogLotusFrontendTest, SupportsTheCompleteBuiltInExpressionSet) {
  constexpr llvm::StringLiteral program = R"dl(
    .decl input(value: i64)
    .decl arithmetic(add: i64, sub: i64, mul: i64, div: i64, rem: i64,
                     neg: i64, pos: i64)
    .decl seed(key: u64, value: i64)
    .decl min_value(key: u64, value: min<i64>)
    .decl max_value(key: u64, value: max<i64>)
    .decl singleton(key: u64, value: set<i64>)

    input(6).
    seed(1, 7).

    arithmetic(X + 1, X - 1, X * 2, X / 2, X % 4, -X, +X) :-
      input(X), where X != 0 && X <= 6 && X > 5 && X >= 6.
    min_value(K, min_lattice(V));
    max_value(K, max_lattice(V));
    singleton(K, set_lattice(V)) :- seed(K, V).

    .output arithmetic
    .output min_value
    .output max_value
    .output singleton
  )dl";

  const llvm::json::Value parsed =
      parseOutput(executeInput(program, frontend::InputFormat::Datalog));
  const llvm::json::Array *arithmetic =
      outputRows(parsed, "arithmetic")[0].getAsArray();
  ASSERT_NE(arithmetic, nullptr);
  ASSERT_EQ(arithmetic->size(), 7U);
  EXPECT_EQ((*arithmetic)[0].getAsInteger().getValueOr(-1), 7);
  EXPECT_EQ((*arithmetic)[1].getAsInteger().getValueOr(-1), 5);
  EXPECT_EQ((*arithmetic)[2].getAsInteger().getValueOr(-1), 12);
  EXPECT_EQ((*arithmetic)[3].getAsInteger().getValueOr(-1), 3);
  EXPECT_EQ((*arithmetic)[4].getAsInteger().getValueOr(-1), 2);
  EXPECT_EQ((*arithmetic)[5].getAsInteger().getValueOr(0), -6);
  EXPECT_EQ((*arithmetic)[6].getAsInteger().getValueOr(-1), 6);
  EXPECT_EQ(outputRows(parsed, "min_value").size(), 1U);
  EXPECT_EQ(outputRows(parsed, "max_value").size(), 1U);
  EXPECT_EQ(outputRows(parsed, "singleton").size(), 1U);
}

TEST(DatalogLotusFrontendTest, SupportsAllBuiltInAggregates) {
  constexpr llvm::StringLiteral program = R"dl(
    .decl group(id: i64)
    .decl value(group: i64, value: i64)
    .decl total(group: i64, value: i64)
    .decl count(group: i64, value: u64)
    .decl minimum(group: i64, value: i64)
    .decl maximum(group: i64, value: i64)
    .decl mean(group: i64, value: f64)

    group(1).
    value(1, 2).
    value(1, 4).
    value(1, 6).

    total(G, S) :- group(G), aggregate S = sum(V) : value(G, V).
    count(G, N) :- group(G), aggregate N = count : value(G, _).
    minimum(G, M) :- group(G), aggregate M = min(V) : value(G, V).
    maximum(G, M) :- group(G), aggregate M = max(V) : value(G, V).
    mean(G, M) :- group(G), aggregate M = mean(V) : value(G, V).

    .output total
    .output count
    .output minimum
    .output maximum
    .output mean
  )dl";

  const llvm::json::Value parsed =
      parseOutput(executeInput(program, frontend::InputFormat::Datalog));
  EXPECT_EQ(outputRows(parsed, "total")[0]
                .getAsArray()
                ->back()
                .getAsInteger()
                .getValueOr(-1),
            12);
  EXPECT_EQ(outputRows(parsed, "count")[0]
                .getAsArray()
                ->back()
                .getAsInteger()
                .getValueOr(-1),
            3);
  EXPECT_EQ(outputRows(parsed, "minimum")[0]
                .getAsArray()
                ->back()
                .getAsInteger()
                .getValueOr(-1),
            2);
  EXPECT_EQ(outputRows(parsed, "maximum")[0]
                .getAsArray()
                ->back()
                .getAsInteger()
                .getValueOr(-1),
            6);
  EXPECT_DOUBLE_EQ(outputRows(parsed, "mean")[0]
                       .getAsArray()
                       ->back()
                       .getAsNumber()
                       .getValueOr(-1),
                   4.0);
}

TEST(DatalogLotusFrontendTest, SupportsBuiltInLatticeRelations) {
  constexpr llvm::StringLiteral program = R"dl(
    .decl edge(src: u64, dst: u64, weight: i64)
    .decl distance(node: u64, value: min<i64>)
    .decl priority(node: u64, value: max<i64>)
    .decl score(node: u64, value: min<f64>)
    .decl ceiling(node: u64, value: max<f64>)
    .decl tags(node: u64, value: set<i64>)

    edge(1, 2, 5).
    edge(2, 3, 7).
    distance(1, 0).
    priority(1, 2).
    priority(1, 5).
    score(1, 3.5).
    score(1, 2.5).
    ceiling(1, 3.5).
    ceiling(1, 4.5).
    tags(1, [1, 2]).
    tags(1, [2, 3]).

    distance(Y, D + W) :- distance(X, D), edge(X, Y, W).

    .output distance
    .output priority
    .output score
    .output ceiling
    .output tags
  )dl";

  const llvm::json::Value parsed =
      parseOutput(executeInput(program, frontend::InputFormat::Datalog));
  const llvm::json::Array &distance = outputRows(parsed, "distance");
  ASSERT_EQ(distance.size(), 3U);
  EXPECT_EQ(distance[2].getAsArray()->back().getAsInteger().getValueOr(-1), 12);
  EXPECT_EQ(outputRows(parsed, "priority")[0]
                .getAsArray()
                ->back()
                .getAsInteger()
                .getValueOr(-1),
            5);
  EXPECT_DOUBLE_EQ(outputRows(parsed, "score")[0]
                       .getAsArray()
                       ->back()
                       .getAsNumber()
                       .getValueOr(-1),
                   2.5);
  EXPECT_DOUBLE_EQ(outputRows(parsed, "ceiling")[0]
                       .getAsArray()
                       ->back()
                       .getAsNumber()
                       .getValueOr(-1),
                   4.5);
  const llvm::json::Array *tags =
      outputRows(parsed, "tags")[0].getAsArray()->back().getAsArray();
  ASSERT_NE(tags, nullptr);
  EXPECT_EQ(tags->size(), 3U);
}

TEST(DatalogZ3FrontendTest, RunsZ3FixedpointSubset) {
  constexpr llvm::StringLiteral program = R"smt2(
    ;(set-option :fixedpoint.engine datalog)
    (define-sort s () (_ BitVec 3))
    (declare-rel edge (s s))
    (declare-rel path (s s))
    (declare-var a s)
    (declare-var b s)
    (declare-var c s)

    (rule (=> (edge a b) (path a b)))
    (rule (=> (and (path a b) (path b c)) (path a c)))

    (rule (edge #b001 #b010))
    (rule (edge #b001 #b011))
    (rule (edge #b010 #b100))

    (declare-rel q1 ())
    (declare-rel q2 ())
    (declare-rel q3 (s))
    (rule (=> (path #b001 #b100) q1))
    (rule (=> (path #b011 #b100) q2))
    (rule (=> (path #b001 b) (q3 b)))

    (query q1)
    (query q2)
    (query q3 :print-answer true)
  )smt2";

  const std::string output = executeInput(program, frontend::InputFormat::Z3);
  const llvm::json::Value parsed = parseOutput(output);
  EXPECT_EQ(outputRows(parsed, "q1").size(), 1U);
  EXPECT_TRUE(outputRows(parsed, "q2").empty());

  const llvm::json::Array &q3 = outputRows(parsed, "q3");
  ASSERT_EQ(q3.size(), 3U);
  EXPECT_EQ(q3[0].getAsArray()->front().getAsInteger().getValueOr(-1), 2);
  EXPECT_EQ(q3[1].getAsArray()->front().getAsInteger().getValueOr(-1), 3);
  EXPECT_EQ(q3[2].getAsArray()->front().getAsInteger().getValueOr(-1), 4);
}

TEST(DatalogZ3FrontendTest, SupportsTrueBodiesAndStratifiedNegation) {
  constexpr llvm::StringLiteral program = R"smt2(
    (define-sort s () (_ BitVec 2))
    (declare-rel item (s))
    (declare-rel blocked (s))
    (declare-rel accepted (s))
    (declare-rel ready ())
    (declare-var x s)

    (rule (item #b01))
    (rule (item #b10))
    (rule (blocked #b10))
    (rule (=> (and (item x) (not (blocked x))) (accepted x)))
    (rule (=> true ready))

    (query accepted)
    (query ready)
  )smt2";

  const llvm::json::Value parsed =
      parseOutput(executeInput(program, frontend::InputFormat::Z3));
  ASSERT_EQ(outputRows(parsed, "accepted").size(), 1U);
  EXPECT_EQ(outputRows(parsed, "accepted")[0]
                .getAsArray()
                ->front()
                .getAsInteger()
                .getValueOr(-1),
            1);
  EXPECT_EQ(outputRows(parsed, "ready").size(), 1U);
}

TEST(DatalogZ3FrontendTest, SupportsFiniteDomainAndBoolSorts) {
  constexpr llvm::StringLiteral program = R"smt2(
    (define-sort fd () (_ FiniteDomain 4))
    (declare-rel finite (fd))
    (declare-rel flag (Bool))
    (rule (finite 2))
    (rule (flag true))
    (query finite)
    (query flag)
  )smt2";

  const llvm::json::Value parsed =
      parseOutput(executeInput(program, frontend::InputFormat::Z3));
  EXPECT_EQ(outputRows(parsed, "finite").size(), 1U);
  EXPECT_EQ(outputRows(parsed, "flag").size(), 1U);
}

TEST(DatalogZ3FrontendTest, Supports64BitBitVectorsAndUnorderedSources) {
  constexpr llvm::StringLiteral facts = R"smt2(
    (rule (value #xffffffffffffffff))
    (query value)
  )smt2";
  constexpr llvm::StringLiteral schema = R"smt2(
    (define-sort word () (_ BitVec 64))
    (declare-rel value (word))
  )smt2";
  const frontend::SourceUnit sources[] = {
      {"facts/value.smt2", facts},
      {"schema.smt2", schema},
  };

  const llvm::json::Value parsed =
      parseOutput(executeInputs(sources, frontend::InputFormat::Z3));
  EXPECT_EQ(outputRows(parsed, "value")[0]
                .getAsArray()
                ->front()
                .getAsString()
                .getValueOr(""),
            "18446744073709551615");
}

TEST(DatalogFrontendDispatchTest, AutoDetectsAllInputFormats) {
  constexpr llvm::StringLiteral datalog = R"dl(
    .decl answer()
    answer.
    .output answer
  )dl";
  constexpr llvm::StringLiteral z3 = R"smt2(
    ; leading comment
    (declare-rel answer ())
    (rule answer)
    (query answer)
  )smt2";
  constexpr llvm::StringLiteral json = R"json({
    "schema_version": 1,
    "relations": [{"name": "answer", "columns": [], "facts": [[]]}],
    "outputs": ["answer"]
  })json";

  const llvm::json::Value datalog_output =
      parseOutput(executeInput(datalog, frontend::InputFormat::Auto));
  const llvm::json::Value z3_output =
      parseOutput(executeInput(z3, frontend::InputFormat::Auto));
  const llvm::json::Value json_output =
      parseOutput(executeInput(json, frontend::InputFormat::Auto));
  EXPECT_EQ(outputRows(datalog_output, "answer").size(), 1U);
  EXPECT_EQ(outputRows(z3_output, "answer").size(), 1U);
  EXPECT_EQ(outputRows(json_output, "answer").size(), 1U);
}

TEST(DatalogZ3FrontendTest, RejectsUnsupportedConstraints) {
  constexpr llvm::StringLiteral program = R"smt2(
    (define-sort s () (_ BitVec 3))
    (declare-rel p (s))
    (declare-var x s)
    (rule (=> (= x #b001) (p x)))
  )smt2";
  EXPECT_THROW(executeInput(program, frontend::InputFormat::Z3),
               std::invalid_argument);
}

TEST(DatalogZ3FrontendTest, RejectsIllSortedConstants) {
  constexpr llvm::StringLiteral wrong_width = R"smt2(
    (define-sort s () (_ BitVec 3))
    (declare-rel p (s))
    (rule (p #b01))
  )smt2";
  constexpr llvm::StringLiteral outside_domain = R"smt2(
    (define-sort s () (_ FiniteDomain 4))
    (declare-rel p (s))
    (rule (p 4))
  )smt2";

  EXPECT_THROW(executeInput(wrong_width, frontend::InputFormat::Z3),
               std::invalid_argument);
  EXPECT_THROW(executeInput(outside_domain, frontend::InputFormat::Z3),
               std::invalid_argument);
}

} // namespace
