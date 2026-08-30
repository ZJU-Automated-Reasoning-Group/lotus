#include "Solvers/Datalog/Frontend/Frontend.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

namespace lotus::datalog::frontend {
namespace {

using internal::AggregateItem;
using internal::Atom;
using internal::Expression;
using internal::Fact;
using internal::FilterItem;
using internal::FrontendIR;
using internal::NegativeItem;
using internal::Output;
using internal::PositiveItem;
using internal::Relation;
using internal::Scalar;
using internal::SourceLocation;
using llvm::StringRef;
using llvm::json::Array;
using llvm::json::Object;
using llvm::json::Value;

constexpr std::int64_t JSON_SCHEMA_VERSION = 1;

[[noreturn]] void fail(const SourceLocation &location,
                       const std::string &message) {
  throw FrontendError("invalid_json_program", location.source, location.line,
                      location.column, message);
}

StringRef requireString(const Object &object, StringRef key,
                        const SourceLocation &location) {
  llvm::Optional<StringRef> value = object.getString(key);
  if (!value || value->empty())
    fail(location, "missing string field '" + key.str() + "'");
  return *value;
}

const Array &requireArray(const Object &object, StringRef key,
                          const SourceLocation &location) {
  const Array *array = object.getArray(key);
  if (!array)
    fail(location, "missing array field '" + key.str() + "'");
  return *array;
}

const Object &requireObject(const Value &value, StringRef field,
                            const SourceLocation &location) {
  const Object *object = value.getAsObject();
  if (!object)
    fail(location, field.str() + " must be an object");
  return *object;
}

Scalar parseScalar(const Value &value, const SourceLocation &location) {
  if (llvm::Optional<std::int64_t> integer = value.getAsInteger())
    return Scalar{*integer};
  if (llvm::Optional<double> number = value.getAsNumber()) {
    if (!std::isfinite(*number))
      fail(location, "floating constant must be finite");
    return Scalar{*number};
  }
  if (llvm::Optional<StringRef> string = value.getAsString())
    return Scalar{string->str()};
  if (llvm::Optional<bool> boolean = value.getAsBoolean())
    return Scalar{*boolean};
  if (const Array *array = value.getAsArray()) {
    Scalar::IntegerSet elements;
    for (const Value &element : *array) {
      llvm::Optional<std::int64_t> integer = element.getAsInteger();
      if (!integer)
        fail(location, "set constants contain only i64 values");
      elements.push_back(*integer);
    }
    return Scalar{std::move(elements)};
  }
  fail(location, "unsupported constant value");
}

Expression parseExpression(const Value &value,
                           const SourceLocation &location) {
  Expression result;
  result.location = location;
  if (llvm::Optional<StringRef> string = value.getAsString()) {
    if (*string == "_") {
      result.kind = Expression::Kind::Variable;
      result.name = "_";
    } else if (string->startswith("$") && string->size() > 1) {
      result.kind = Expression::Kind::Variable;
      result.name = string->drop_front().str();
    } else {
      result.kind = Expression::Kind::Constant;
      result.constant = Scalar{string->str()};
    }
    return result;
  }
  if (const Object *object = value.getAsObject()) {
    if (llvm::Optional<StringRef> name = object->getString("var")) {
      result.kind = Expression::Kind::Variable;
      result.name = name->startswith("$") ? name->drop_front().str()
                                          : name->str();
      if (result.name.empty())
        fail(location, "variable name must not be empty");
      return result;
    }
    if (const Value *constant = object->get("const")) {
      result.kind = Expression::Kind::Constant;
      result.constant = parseScalar(*constant, location);
      return result;
    }
    result.name = requireString(*object, "op", location).str();
    const Array &arguments = requireArray(*object, "args", location);
    if (arguments.size() == 1)
      result.kind = Expression::Kind::Unary;
    else if (arguments.size() == 2)
      result.kind = Expression::Kind::Binary;
    else
      fail(location, "expression operation requires one or two arguments");
    for (const Value &argument : arguments)
      result.operands.push_back(parseExpression(argument, location));
    return result;
  }
  result.kind = Expression::Kind::Constant;
  result.constant = parseScalar(value, location);
  return result;
}

Atom parseAtom(const Object &object, const SourceLocation &location) {
  Atom atom;
  atom.relation = requireString(object, "relation", location).str();
  atom.location = location;
  for (const Value &argument : requireArray(object, "args", location))
    atom.arguments.push_back(parseExpression(argument, location));
  return atom;
}

class JsonParser {
public:
  JsonParser(const Object &root, SourceLocation location)
      : root_(root), location_(std::move(location)) {}

  FrontendIR parse() {
    llvm::Optional<std::int64_t> schema_version =
        root_.getInteger("schema_version");
    if (!schema_version)
      fail(location_, "missing integer 'schema_version'");
    if (*schema_version != JSON_SCHEMA_VERSION) {
      fail(location_, "unsupported Datalog JSON schema_version " +
                          std::to_string(*schema_version));
    }

    for (const Value &value : requireArray(root_, "relations", location_))
      parseRelation(requireObject(value, "relation", location_));
    if (const Array *rules = root_.getArray("rules")) {
      for (const Value &value : *rules)
        parseRule(requireObject(value, "rule", location_));
    }
    if (const Array *outputs = root_.getArray("outputs")) {
      for (const Value &value : *outputs) {
        llvm::Optional<StringRef> name = value.getAsString();
        if (!name)
          fail(location_, "output relation names must be strings");
        program_.outputs.push_back(Output{name->str(), location_});
      }
    }
    return std::move(program_);
  }

private:
  void parseRelation(const Object &object) {
    Relation relation;
    relation.name = requireString(object, "name", location_).str();
    relation.location = location_;
    for (const Value &column : requireArray(object, "columns", location_)) {
      llvm::Optional<StringRef> type = column.getAsString();
      if (!type)
        fail(location_, "relation column types must be strings");
      relation.columns.push_back(type->str());
    }
    const StringRef kind = object.getString("kind").getValueOr("relation");
    if (kind != "relation" && kind != "lattice")
      fail(location_, "relation kind must be relation or lattice");
    relation.lattice = kind == "lattice";
    program_.relations.push_back(std::move(relation));

    if (const Array *facts = object.getArray("facts")) {
      for (const Value &fact_value : *facts) {
        const Array *row = fact_value.getAsArray();
        if (!row)
          fail(location_, "fact must be an array");
        Atom atom;
        atom.relation = program_.relations.back().name;
        atom.location = location_;
        for (const Value &element : *row) {
          Expression expression;
          expression.kind = Expression::Kind::Constant;
          expression.constant = parseScalar(element, location_);
          expression.location = location_;
          atom.arguments.push_back(std::move(expression));
        }
        program_.facts.push_back(Fact{std::move(atom), location_});
      }
    }
  }

  AggregateItem parseAggregate(const Object &object) {
    AggregateItem aggregate;
    aggregate.operation = requireString(object, "op", location_).str();
    StringRef output = requireString(object, "output", location_);
    output.consume_front("$");
    if (output.empty())
      fail(location_, "aggregate output variable must not be empty");
    aggregate.output = output.str();
    const Object *source = object.getObject("source");
    if (!source)
      fail(location_, "aggregate requires a source atom");
    aggregate.source = parseAtom(*source, location_);
    if (const Value *projection = object.get("value"))
      aggregate.projection = parseExpression(*projection, location_);
    aggregate.location = location_;
    return aggregate;
  }

  void parseRule(const Object &object) {
    internal::Rule rule;
    rule.location = location_;
    if (const Object *head = object.getObject("head"))
      rule.heads.push_back(parseAtom(*head, location_));
    if (const Array *heads = object.getArray("heads")) {
      for (const Value &head : *heads)
        rule.heads.push_back(
            parseAtom(requireObject(head, "head", location_), location_));
    }
    if (rule.heads.empty())
      fail(location_, "rule requires head or heads");

    for (const Value &item_value : requireArray(object, "body", location_)) {
      const Object &item =
          requireObject(item_value, "body item", location_);
      if (const Object *atom = item.getObject("atom")) {
        rule.body.push_back(PositiveItem{parseAtom(*atom, location_)});
      } else if (const Object *negation = item.getObject("not")) {
        rule.body.push_back(NegativeItem{parseAtom(*negation, location_)});
      } else if (const Value *condition = item.get("where")) {
        rule.body.push_back(
            FilterItem{parseExpression(*condition, location_)});
      } else if (const Object *aggregate = item.getObject("aggregate")) {
        rule.body.push_back(parseAggregate(*aggregate));
      } else {
        fail(location_, "unknown rule body item");
      }
    }
    program_.rules.push_back(std::move(rule));
  }

  const Object &root_;
  SourceLocation location_;
  FrontendIR program_;
};

} // namespace

internal::FrontendIR internal::parseJson(SourceUnit input) {
  const std::string source = input.name.empty() ? "<json>" : input.name.str();
  llvm::Expected<Value> parsed = llvm::json::parse(input.content);
  if (!parsed) {
    throw FrontendError("json_syntax_error", source, 1, 1,
                        llvm::toString(parsed.takeError()));
  }
  const Object *root = parsed->getAsObject();
  if (!root)
    throw FrontendError("invalid_json_program", source, 1, 1,
                        "JSON program root must be an object");
  return JsonParser(*root, SourceLocation{source, 1, 1}).parse();
}

} // namespace lotus::datalog::frontend
