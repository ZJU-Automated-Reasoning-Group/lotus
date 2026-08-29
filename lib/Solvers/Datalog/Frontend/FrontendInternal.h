#pragma once

#include "Solvers/Datalog/Frontend/Frontend.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <llvm/Support/raw_ostream.h>

namespace lotus::datalog::frontend::internal {

struct SourceLocation {
  std::string source;
  std::size_t line = 1;
  std::size_t column = 1;
};

struct Scalar {
  using IntegerSet = std::vector<std::int64_t>;
  using Value = std::variant<std::int64_t, std::uint64_t, double, std::string,
                             bool, IntegerSet>;
  Value value = std::int64_t{0};
};

struct Expression {
  enum class Kind { Variable, Constant, Unary, Binary };

  Kind kind = Kind::Constant;
  std::string name;
  Scalar constant;
  std::vector<Expression> operands;
  SourceLocation location;
};

struct Atom {
  std::string relation;
  std::vector<Expression> arguments;
  SourceLocation location;
};

struct PositiveItem {
  Atom atom;
};

struct NegativeItem {
  Atom atom;
};

struct FilterItem {
  Expression expression;
};

struct AggregateItem {
  std::string output;
  std::string operation;
  std::optional<Expression> projection;
  Atom source;
  SourceLocation location;
};

using BodyItem =
    std::variant<PositiveItem, NegativeItem, FilterItem, AggregateItem>;

struct Rule {
  std::vector<Atom> heads;
  std::vector<BodyItem> body;
  SourceLocation location;
};

struct Relation {
  std::string name;
  std::vector<std::string> columns;
  bool lattice = false;
  SourceLocation location;
};

struct Fact {
  Atom atom;
  SourceLocation location;
};

struct Output {
  std::string relation;
  SourceLocation location;
};

struct Include {
  std::string path;
  SourceLocation location;
};

// Parsed, frontend-neutral syntax. It intentionally performs no name or arity
// resolution, so declarations, facts, rules, and outputs may be distributed
// across source units in any order. Lowering owns semantic validation.
struct FrontendIR {
  void append(FrontendIR other) {
    relations.insert(relations.end(),
                     std::make_move_iterator(other.relations.begin()),
                     std::make_move_iterator(other.relations.end()));
    facts.insert(facts.end(), std::make_move_iterator(other.facts.begin()),
                 std::make_move_iterator(other.facts.end()));
    rules.insert(rules.end(), std::make_move_iterator(other.rules.begin()),
                 std::make_move_iterator(other.rules.end()));
    outputs.insert(outputs.end(),
                   std::make_move_iterator(other.outputs.begin()),
                   std::make_move_iterator(other.outputs.end()));
    includes.insert(includes.end(),
                    std::make_move_iterator(other.includes.begin()),
                    std::make_move_iterator(other.includes.end()));
  }

  std::vector<Relation> relations;
  std::vector<Fact> facts;
  std::vector<Rule> rules;
  std::vector<Output> outputs;
  std::vector<Include> includes;
};

template <typename T>
Expression scalar(T value, SourceLocation location = {}) {
  Expression result;
  result.kind = Expression::Kind::Constant;
  result.constant = Scalar{std::move(value)};
  result.location = std::move(location);
  return result;
}

inline Expression variable(std::string name, SourceLocation location = {}) {
  Expression result;
  result.kind = Expression::Kind::Variable;
  result.name = std::move(name);
  result.location = std::move(location);
  return result;
}

void execute(const FrontendIR &program, const RunOptions &options,
             llvm::raw_ostream &output);
std::string toJson(const FrontendIR &program);

FrontendIR parseJson(SourceUnit input);
FrontendIR parseDatalog(llvm::ArrayRef<SourceUnit> inputs,
                        const SourceResolver &resolver = {});
FrontendIR parseZ3(llvm::ArrayRef<SourceUnit> inputs);

} // namespace lotus::datalog::frontend::internal
