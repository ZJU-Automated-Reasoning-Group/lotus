#include "Dataflow/Datalog/Core/Error.h"
#include "Dataflow/Datalog/Core/Lattice.h"
#include "Dataflow/Datalog/Core/TypeSupport.h"
#include "Dataflow/Datalog/Frontend/Frontend.h"
#include "Dataflow/Datalog/Semantic/SemanticProgram.h"
#include "FrontendInternal.h"

#include <algorithm>
#include <any>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/ADT/Optional.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MathExtras.h>

namespace lotus::datalog::frontend {
namespace {

using llvm::StringRef;
using llvm::json::Array;
using llvm::json::Object;
using llvm::json::Value;

constexpr std::int64_t JSON_SCHEMA_VERSION = 1;

enum class ValueKind {
  I64,
  U64,
  F64,
  String,
  Bool,
  MinI64,
  MaxI64,
  MinF64,
  MaxF64,
  SetI64,
};

[[noreturn]] void evaluationFailure(EvaluationErrorCode code,
                                    StringRef expression,
                                    const std::string &message) {
  throw EvaluationError(code, expression.str(), message);
}

template <typename T> T checkedAdd(T lhs, T rhs, StringRef expression) {
  T result{};
  bool overflow = false;
  if constexpr (std::is_signed_v<T>) {
    overflow = llvm::AddOverflow(lhs, rhs, result);
  } else {
    result = lhs + rhs;
    overflow = result < lhs;
  }
  if (overflow) {
    evaluationFailure(EvaluationErrorCode::IntegerOverflow, expression,
                      "integer overflow in addition");
  }
  return result;
}

template <typename T> T checkedSubtract(T lhs, T rhs, StringRef expression) {
  T result{};
  bool overflow = false;
  if constexpr (std::is_signed_v<T>) {
    overflow = llvm::SubOverflow(lhs, rhs, result);
  } else {
    overflow = lhs < rhs;
    result = lhs - rhs;
  }
  if (overflow) {
    evaluationFailure(EvaluationErrorCode::IntegerOverflow, expression,
                      "integer overflow in subtraction");
  }
  return result;
}

template <typename T> T checkedMultiply(T lhs, T rhs, StringRef expression) {
  T result{};
  bool overflow = false;
  if constexpr (std::is_signed_v<T>) {
    overflow = llvm::MulOverflow(lhs, rhs, result);
  } else {
    overflow = rhs != 0 && lhs > std::numeric_limits<T>::max() / rhs;
    result = lhs * rhs;
  }
  if (overflow) {
    evaluationFailure(EvaluationErrorCode::IntegerOverflow, expression,
                      "integer overflow in multiplication");
  }
  return result;
}

template <typename T> T checkedNegate(T value, StringRef expression) {
  return checkedSubtract(T{}, value, expression);
}

template <typename T> T checkedDivide(T lhs, T rhs, StringRef expression) {
  if (rhs == T{}) {
    evaluationFailure(EvaluationErrorCode::DivisionByZero, expression,
                      "division by zero");
  }
  if constexpr (std::is_signed_v<T>) {
    if (lhs == std::numeric_limits<T>::min() && rhs == T{-1}) {
      evaluationFailure(EvaluationErrorCode::IntegerOverflow, expression,
                        "integer overflow in division");
    }
  }
  return lhs / rhs;
}

template <typename T> T checkedRemainder(T lhs, T rhs, StringRef expression) {
  if (rhs == T{}) {
    evaluationFailure(EvaluationErrorCode::RemainderByZero, expression,
                      "remainder by zero");
  }
  if constexpr (std::is_signed_v<T>) {
    if (lhs == std::numeric_limits<T>::min() && rhs == T{-1}) {
      evaluationFailure(EvaluationErrorCode::IntegerOverflow, expression,
                        "integer overflow in remainder");
    }
  }
  return lhs % rhs;
}

double checkedFinite(double value, StringRef expression) {
  if (!std::isfinite(value)) {
    evaluationFailure(EvaluationErrorCode::NonFiniteFloatingPoint, expression,
                      "floating-point operation produced a non-finite value");
  }
  return value;
}

ColumnType portableF64Column() {
  ColumnType result = detail::makeColumnType<double>();
  result.validate = [](const std::any &value) {
    checkedFinite(std::any_cast<const double &>(value), "relation-value");
  };
  return result;
}

template <typename Lattice> ColumnType portableF64LatticeColumn() {
  ColumnType result = detail::makeColumnType<Lattice>();
  result.validate = [](const std::any &value) {
    checkedFinite(std::any_cast<const Lattice &>(value).value(),
                  "lattice-value");
  };
  return result;
}

struct TypeSpec {
  ValueKind kind = ValueKind::I64;

  bool operator==(const TypeSpec &other) const { return kind == other.kind; }
  bool operator!=(const TypeSpec &other) const { return !(*this == other); }

  bool isLattice() const {
    return kind == ValueKind::MinI64 || kind == ValueKind::MaxI64 ||
           kind == ValueKind::MinF64 || kind == ValueKind::MaxF64 ||
           kind == ValueKind::SetI64;
  }

  std::string name() const {
    switch (kind) {
    case ValueKind::I64:
      return "i64";
    case ValueKind::U64:
      return "u64";
    case ValueKind::F64:
      return "f64";
    case ValueKind::String:
      return "string";
    case ValueKind::Bool:
      return "bool";
    case ValueKind::MinI64:
      return "min<i64>";
    case ValueKind::MaxI64:
      return "max<i64>";
    case ValueKind::MinF64:
      return "min<f64>";
    case ValueKind::MaxF64:
      return "max<f64>";
    case ValueKind::SetI64:
      return "set<i64>";
    }
    throw std::logic_error("unknown portable Datalog type");
  }

  std::type_index typeIndex() const {
    switch (kind) {
    case ValueKind::I64:
      return typeid(std::int64_t);
    case ValueKind::U64:
      return typeid(std::uint64_t);
    case ValueKind::F64:
      return typeid(double);
    case ValueKind::String:
      return typeid(std::string);
    case ValueKind::Bool:
      return typeid(bool);
    case ValueKind::MinI64:
      return typeid(MinLattice<std::int64_t>);
    case ValueKind::MaxI64:
      return typeid(MaxLattice<std::int64_t>);
    case ValueKind::MinF64:
      return typeid(MinLattice<double>);
    case ValueKind::MaxF64:
      return typeid(MaxLattice<double>);
    case ValueKind::SetI64:
      return typeid(SetLattice<std::int64_t>);
    }
    throw std::logic_error("unknown portable Datalog type");
  }

  ColumnType columnType() const {
    switch (kind) {
    case ValueKind::I64:
      return detail::makeColumnType<std::int64_t>();
    case ValueKind::U64:
      return detail::makeColumnType<std::uint64_t>();
    case ValueKind::F64:
      return portableF64Column();
    case ValueKind::String:
      return detail::makeColumnType<std::string>();
    case ValueKind::Bool:
      return detail::makeColumnType<bool>();
    case ValueKind::MinI64:
      return detail::makeColumnType<MinLattice<std::int64_t>>();
    case ValueKind::MaxI64:
      return detail::makeColumnType<MaxLattice<std::int64_t>>();
    case ValueKind::MinF64:
      return portableF64LatticeColumn<MinLattice<double>>();
    case ValueKind::MaxF64:
      return portableF64LatticeColumn<MaxLattice<double>>();
    case ValueKind::SetI64:
      return detail::makeColumnType<SetLattice<std::int64_t>>();
    }
    throw std::logic_error("unknown portable Datalog type");
  }
};

TypeSpec parseType(StringRef name) {
  if (name == "i64")
    return {ValueKind::I64};
  if (name == "u64")
    return {ValueKind::U64};
  if (name == "f64")
    return {ValueKind::F64};
  if (name == "string")
    return {ValueKind::String};
  if (name == "bool")
    return {ValueKind::Bool};
  if (name == "min<i64>")
    return {ValueKind::MinI64};
  if (name == "max<i64>")
    return {ValueKind::MaxI64};
  if (name == "min<f64>")
    return {ValueKind::MinF64};
  if (name == "max<f64>")
    return {ValueKind::MaxF64};
  if (name == "set<i64>")
    return {ValueKind::SetI64};
  throw std::invalid_argument("unknown column type '" + name.str() + "'");
}

Value encodeValue(const std::any &value, TypeSpec type) {
  switch (type.kind) {
  case ValueKind::I64:
    return std::any_cast<const std::int64_t &>(value);
  case ValueKind::U64: {
    const std::uint64_t integer = std::any_cast<const std::uint64_t &>(value);
    if (integer >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return std::to_string(integer);
    return static_cast<std::int64_t>(integer);
  }
  case ValueKind::F64:
    return std::any_cast<const double &>(value);
  case ValueKind::String:
    return std::any_cast<const std::string &>(value);
  case ValueKind::Bool:
    return std::any_cast<const bool &>(value);
  case ValueKind::MinI64:
    return std::any_cast<const MinLattice<std::int64_t> &>(value).value();
  case ValueKind::MaxI64:
    return std::any_cast<const MaxLattice<std::int64_t> &>(value).value();
  case ValueKind::MinF64:
    return std::any_cast<const MinLattice<double> &>(value).value();
  case ValueKind::MaxF64:
    return std::any_cast<const MaxLattice<double> &>(value).value();
  case ValueKind::SetI64: {
    Array result;
    for (std::int64_t element :
         std::any_cast<const SetLattice<std::int64_t> &>(value).values())
      result.push_back(element);
    return result;
  }
  }
  throw std::logic_error("unknown portable Datalog type");
}

std::function<bool(std::any &, const std::any &)> latticeJoin(TypeSpec type) {
  switch (type.kind) {
  case ValueKind::MinI64:
    return [](std::any &current, const std::any &candidate) {
      return std::any_cast<MinLattice<std::int64_t> &>(current).joinMut(
          std::any_cast<const MinLattice<std::int64_t> &>(candidate));
    };
  case ValueKind::MaxI64:
    return [](std::any &current, const std::any &candidate) {
      return std::any_cast<MaxLattice<std::int64_t> &>(current).joinMut(
          std::any_cast<const MaxLattice<std::int64_t> &>(candidate));
    };
  case ValueKind::MinF64:
    return [](std::any &current, const std::any &candidate) {
      return std::any_cast<MinLattice<double> &>(current).joinMut(
          std::any_cast<const MinLattice<double> &>(candidate));
    };
  case ValueKind::MaxF64:
    return [](std::any &current, const std::any &candidate) {
      return std::any_cast<MaxLattice<double> &>(current).joinMut(
          std::any_cast<const MaxLattice<double> &>(candidate));
    };
  case ValueKind::SetI64:
    return [](std::any &current, const std::any &candidate) {
      return std::any_cast<SetLattice<std::int64_t> &>(current).joinMut(
          std::any_cast<const SetLattice<std::int64_t> &>(candidate));
    };
  default:
    throw std::invalid_argument("lattice value type must be min, max, or set");
  }
}

struct RelationSpec {
  RelationId id = 0;
  std::string name;
  std::vector<TypeSpec> columns;
};

struct VariableSpec {
  VarId id = 0;
  TypeSpec type;
};

struct RuleScope {
  std::size_t rule_index = 0;
  std::unordered_map<std::string, VariableSpec> variables;
};

struct DynamicExpr {
  TypeSpec type;
  ExprIR ir;
};

template <typename T> DynamicExpr constantExpr(TypeSpec type, T value) {
  ExprIR expression;
  expression.type = type.typeIndex();
  expression.debug_name = "constant";
  expression.evaluate = [value = std::move(value)](const Binding &) {
    return std::any(value);
  };
  return {type, std::move(expression)};
}

DynamicExpr constantAnyExpr(TypeSpec type, std::any value) {
  ExprIR expression;
  expression.type = type.typeIndex();
  expression.debug_name = "constant";
  expression.evaluate = [value = std::move(value)](const Binding &) {
    return value;
  };
  return {type, std::move(expression)};
}

DynamicExpr variableExpr(const VariableSpec &variable) {
  ExprIR expression;
  expression.type = variable.type.typeIndex();
  expression.referenced_vars = {variable.id};
  expression.debug_name = "variable";
  expression.evaluate = [id = variable.id](const Binding &binding) {
    if (id >= binding.size() || !binding[id])
      throw std::logic_error("evaluating an unbound Datalog variable");
    return binding[id].materialize();
  };
  return {variable.type, std::move(expression)};
}

template <typename L, typename R, typename Result, typename Function>
DynamicExpr binaryExpr(TypeSpec result_type, DynamicExpr lhs, DynamicExpr rhs,
                       Function function, std::string name) {
  ExprIR expression;
  expression.type = result_type.typeIndex();
  expression.referenced_vars =
      detail::mergeReferences(lhs.ir.referenced_vars, rhs.ir.referenced_vars);
  expression.debug_name = std::move(name);
  expression.evaluate = [left = std::move(lhs.ir), right = std::move(rhs.ir),
                         function =
                             std::move(function)](const Binding &binding) {
    return std::any(function(std::any_cast<L>(left.evaluate(binding)),
                             std::any_cast<R>(right.evaluate(binding))));
  };
  return {result_type, std::move(expression)};
}

template <typename Input, typename Result, typename Function>
DynamicExpr unaryExpr(TypeSpec result_type, DynamicExpr operand,
                      Function function, std::string name) {
  ExprIR expression;
  expression.type = result_type.typeIndex();
  expression.referenced_vars = operand.ir.referenced_vars;
  expression.debug_name = std::move(name);
  expression.evaluate = [input = std::move(operand.ir),
                         function =
                             std::move(function)](const Binding &binding) {
    return std::any(function(std::any_cast<Input>(input.evaluate(binding))));
  };
  return {result_type, std::move(expression)};
}

template <typename T, typename Function>
DynamicExpr sameTypeBinary(TypeSpec type, DynamicExpr lhs, DynamicExpr rhs,
                           Function function, StringRef name) {
  return binaryExpr<T, T, T>(type, std::move(lhs), std::move(rhs),
                             std::move(function), name.str());
}

template <typename T, typename Function>
DynamicExpr comparison(TypeSpec operand_type, DynamicExpr lhs, DynamicExpr rhs,
                       Function function, StringRef name) {
  (void)operand_type;
  return binaryExpr<T, T, bool>({ValueKind::Bool}, std::move(lhs),
                                std::move(rhs), std::move(function),
                                name.str());
}

class LoweredProgram {
public:
  explicit LoweredProgram(const internal::FrontendIR &root) { parse(root); }

  Object execute(const RunOptions &options) {
    CompiledProgram compiled = program_.compile();
    RunStatus status = RunStatus::Completed;
    ExecutionOptions execution = options.execution;
    if (options.explain_analyze)
      execution.collect_profile = true;
    const bool execute_program =
        !options.validate_only && (!options.explain || options.explain_analyze);
    if (execute_program)
      status = compiled.run(execution);

    Object result;
    result["schema_version"] = JSON_SCHEMA_VERSION;
    result["status"] = options.validate_only            ? "valid"
                       : options.explain                 ? "explained"
                       : status == RunStatus::Completed ? "ok"
                                                        : "cancelled";
    if (options.explain) {
      result["plan"] = compiled.explain(options.explain_analyze
                                             ? ExplainMode::Analyze
                                             : ExplainMode::Plan);
      if (options.explain_analyze)
        result["stats"] = encodeStats(compiled.stats());
    } else if (!options.validate_only) {
      Object relations;
      for (RelationId relation_id : outputs_) {
        const RelationSpec &relation = relations_.at(relation_id);
        std::vector<std::pair<std::string, Array>> encoded_rows;
        for (const std::vector<std::any> &row : program_.rows(relation_id)) {
          Array encoded;
          for (std::size_t column = 0; column < row.size(); ++column)
            encoded.push_back(
                encodeValue(row[column], relation.columns[column]));
          std::string key = llvm::formatv("{0}", Value(Array(encoded))).str();
          encoded_rows.emplace_back(std::move(key), std::move(encoded));
        }
        std::sort(encoded_rows.begin(), encoded_rows.end(),
                  [](const auto &lhs, const auto &rhs) {
                    return lhs.first < rhs.first;
                  });
        Array rows;
        for (auto &entry : encoded_rows)
          rows.push_back(std::move(entry.second));
        relations[relation.name] = std::move(rows);
      }
      result["relations"] = std::move(relations);
      result["stats"] = encodeStats(compiled.stats());
    }
    return result;
  }

private:
  [[noreturn]] static void fail(const internal::SourceLocation &location,
                                const std::string &message,
                                llvm::StringRef code = "invalid_program") {
    throw FrontendError(code.str(), location.source, location.line,
                        location.column, message);
  }

  static std::uint64_t parseUnsignedDecimal(
      llvm::StringRef text, const internal::SourceLocation &location) {
    if (text.empty())
      fail(location, "expected an unsigned decimal integer");
    std::uint64_t value = 0;
    for (char character : text) {
      if (character < '0' || character > '9')
        fail(location, "expected an unsigned decimal integer");
      const unsigned digit = static_cast<unsigned>(character - '0');
      if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
        fail(location, "unsigned integer is outside u64 range");
      value = value * 10 + digit;
    }
    return value;
  }

  static std::any parseFrontendConstant(
      const internal::Scalar &scalar, TypeSpec type,
      const internal::SourceLocation &location) {
    const internal::Scalar::Value &value = scalar.value;
    switch (type.kind) {
    case ValueKind::I64:
      if (const auto *integer = std::get_if<std::int64_t>(&value))
        return *integer;
      if (const auto *integer = std::get_if<std::uint64_t>(&value)) {
        if (*integer <= static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max()))
          return static_cast<std::int64_t>(*integer);
      }
      fail(location, "constant must be an i64");
    case ValueKind::U64:
      if (const auto *integer = std::get_if<std::uint64_t>(&value))
        return *integer;
      if (const auto *integer = std::get_if<std::int64_t>(&value)) {
        if (*integer >= 0)
          return static_cast<std::uint64_t>(*integer);
      }
      if (const auto *string = std::get_if<std::string>(&value))
        return parseUnsignedDecimal(*string, location);
      fail(location, "constant must be a non-negative u64");
    case ValueKind::F64: {
      double number = 0;
      if (const auto *floating = std::get_if<double>(&value))
        number = *floating;
      else if (const auto *integer = std::get_if<std::int64_t>(&value))
        number = static_cast<double>(*integer);
      else if (const auto *integer = std::get_if<std::uint64_t>(&value))
        number = static_cast<double>(*integer);
      else
        fail(location, "constant must be a number");
      return checkedFinite(number, "constant");
    }
    case ValueKind::String:
      if (const auto *string = std::get_if<std::string>(&value))
        return *string;
      fail(location, "constant must be a string");
    case ValueKind::Bool:
      if (const auto *boolean = std::get_if<bool>(&value))
        return *boolean;
      fail(location, "constant must be a boolean");
    case ValueKind::MinI64:
      return MinLattice<std::int64_t>(std::any_cast<std::int64_t>(
          parseFrontendConstant(scalar, {ValueKind::I64}, location)));
    case ValueKind::MaxI64:
      return MaxLattice<std::int64_t>(std::any_cast<std::int64_t>(
          parseFrontendConstant(scalar, {ValueKind::I64}, location)));
    case ValueKind::MinF64:
      return MinLattice<double>(std::any_cast<double>(
          parseFrontendConstant(scalar, {ValueKind::F64}, location)));
    case ValueKind::MaxF64:
      return MaxLattice<double>(std::any_cast<double>(
          parseFrontendConstant(scalar, {ValueKind::F64}, location)));
    case ValueKind::SetI64: {
      const auto *elements = std::get_if<internal::Scalar::IntegerSet>(&value);
      if (!elements)
        fail(location, "constant must be an i64 set");
      return SetLattice<std::int64_t>(
          std::set<std::int64_t>(elements->begin(), elements->end()));
    }
    }
    throw std::logic_error("unknown frontend type");
  }

  static TypeSpec inferConstantType(const internal::Scalar &scalar,
                                    const internal::SourceLocation &location) {
    if (std::holds_alternative<std::int64_t>(scalar.value))
      return {ValueKind::I64};
    if (std::holds_alternative<std::uint64_t>(scalar.value))
      return {ValueKind::U64};
    if (std::holds_alternative<double>(scalar.value))
      return {ValueKind::F64};
    if (std::holds_alternative<std::string>(scalar.value))
      return {ValueKind::String};
    if (std::holds_alternative<bool>(scalar.value))
      return {ValueKind::Bool};
    if (std::holds_alternative<internal::Scalar::IntegerSet>(scalar.value))
      return {ValueKind::SetI64};
    fail(location, "unsupported constant");
  }

  static Object encodeStats(const ExecutionStats &stats) {
    return Object{
        {"rule_evaluations", static_cast<std::int64_t>(stats.rule_evaluations)},
        {"tuples_scanned", static_cast<std::int64_t>(stats.tuples_scanned)},
        {"index_lookups", static_cast<std::int64_t>(stats.index_lookups)},
        {"inserted_facts", static_cast<std::int64_t>(stats.inserted_facts)},
        {"fixpoint_iterations",
         static_cast<std::int64_t>(stats.fixpoint_iterations)},
        {"planned_reorders", static_cast<std::int64_t>(stats.planned_reorders)},
        {"parallel_tasks", static_cast<std::int64_t>(stats.parallel_tasks)},
        {"parallel_rule_tasks",
         static_cast<std::int64_t>(stats.parallel_rule_tasks)},
        {"parallel_merge_tasks",
         static_cast<std::int64_t>(stats.parallel_merge_tasks)},
        {"parallel_aggregate_tasks",
         static_cast<std::int64_t>(stats.parallel_aggregate_tasks)},
        {"scc_count", static_cast<std::int64_t>(stats.scc_count)},
        {"relation_count", static_cast<std::int64_t>(stats.relation_count)},
        {"total_facts", static_cast<std::int64_t>(stats.total_facts)},
        {"peak_delta", static_cast<std::int64_t>(stats.peak_delta)},
        {"index_count", static_cast<std::int64_t>(stats.index_count)},
        {"index_entries", static_cast<std::int64_t>(stats.index_entries)},
        {"index_memory_bytes",
         static_cast<std::int64_t>(stats.index_memory_bytes)},
        {"tuple_memory_bytes",
         static_cast<std::int64_t>(stats.tuple_memory_bytes)},
        {"uniqueness_memory_bytes",
         static_cast<std::int64_t>(stats.uniqueness_memory_bytes)},
        {"base_memory_bytes",
         static_cast<std::int64_t>(stats.base_memory_bytes)},
        {"head_derivations",
         static_cast<std::int64_t>(stats.head_derivations)},
        {"local_unique_candidates",
         static_cast<std::int64_t>(stats.local_unique_candidates)},
        {"global_unique_candidates",
         static_cast<std::int64_t>(stats.global_unique_candidates)},
        {"incremental_sccs",
         static_cast<std::int64_t>(stats.incremental_sccs)},
        {"rebuilt_sccs", static_cast<std::int64_t>(stats.rebuilt_sccs)},
        {"base_delta_facts",
         static_cast<std::int64_t>(stats.base_delta_facts)}};
  }

  void parse(const internal::FrontendIR &root) {
    for (const internal::Relation &relation : root.relations)
      parseRelation(relation);
    for (const internal::Fact &fact : root.facts)
      parseFact(fact);
    for (std::size_t index = 0; index < root.rules.size(); ++index)
      parseRule(root.rules[index], index);

    if (root.outputs.empty()) {
      for (const RelationSpec &relation : relations_)
        outputs_.push_back(relation.id);
    } else {
      std::set<RelationId> seen;
      for (const internal::Output &output : root.outputs) {
        const RelationSpec &relation = findRelation(output.relation,
                                                    output.location);
        if (seen.insert(relation.id).second)
          outputs_.push_back(relation.id);
      }
    }
  }

  void parseRelation(const internal::Relation &relation) {
    if (relation.name.empty())
      fail(relation.location, "relation name must not be empty");
    if (relation_ids_.count(relation.name))
      fail(relation.location, "duplicate relation '" + relation.name + "'");

    std::vector<TypeSpec> types;
    std::vector<ColumnType> columns;
    for (const std::string &column : relation.columns) {
      try {
        TypeSpec type = parseType(column);
        types.push_back(type);
        columns.push_back(type.columnType());
      } catch (const std::invalid_argument &error) {
        fail(relation.location, error.what());
      }
    }
    if (relation.lattice && (types.empty() || !types.back().isLattice())) {
      fail(relation.location, "lattice relation '" + relation.name +
                                  "' requires a lattice final column");
    }

    RelationId id = program_.addRelation(
        relation.name, std::move(columns),
        relation.lattice ? RelationKind::Lattice : RelationKind::Set,
        relation.lattice
            ? latticeJoin(types.back())
            : std::function<bool(std::any &, const std::any &)>{});
    relation_ids_[relation.name] = id;
    relations_.push_back({id, relation.name, std::move(types)});
  }

  const RelationSpec &findRelation(
      StringRef name, const internal::SourceLocation &location) const {
    auto found = relation_ids_.find(name.str());
    if (found == relation_ids_.end())
      fail(location, "unknown relation '" + name.str() + "'");
    return relations_.at(found->second);
  }

  void checkArity(const internal::Atom &atom,
                  const RelationSpec &relation) const {
    if (atom.arguments.size() != relation.columns.size()) {
      fail(atom.location, "arity mismatch for relation '" + relation.name +
                              "': expected " +
                              std::to_string(relation.columns.size()) +
                              ", got " +
                              std::to_string(atom.arguments.size()));
    }
  }

  VariableSpec &variable(RuleScope &scope, StringRef name, TypeSpec type,
                         const internal::SourceLocation &location) {
    name.consume_front("$");
    const std::string normalized = name.str();
    if (normalized.empty())
      fail(location, "variable name must not be empty");
    auto found = scope.variables.find(normalized);
    if (found != scope.variables.end()) {
      if (found->second.type != type)
        fail(location, "variable '" + normalized +
                           "' has inconsistent types");
      return found->second;
    }
    const std::string internal_name =
        "r" + std::to_string(scope.rule_index) + ":" + normalized;
    VarId id = program_.addVariable(internal_name, type.typeIndex());
    return scope.variables.emplace(normalized, VariableSpec{id, type})
        .first->second;
  }

  VariableSpec &findVariable(RuleScope &scope, StringRef name,
                             const internal::SourceLocation &location) {
    name.consume_front("$");
    auto found = scope.variables.find(name.str());
    if (found == scope.variables.end())
      fail(location, "unknown variable '" + name.str() + "'");
    return found->second;
  }

  void parseFact(const internal::Fact &fact) {
    const RelationSpec &relation =
        findRelation(fact.atom.relation, fact.atom.location);
    checkArity(fact.atom, relation);
    std::vector<std::any> row;
    row.reserve(fact.atom.arguments.size());
    for (std::size_t column = 0; column < fact.atom.arguments.size(); ++column) {
      const internal::Expression &expression = fact.atom.arguments[column];
      if (expression.kind != internal::Expression::Kind::Constant)
        fail(expression.location, "facts can contain only constants");
      row.push_back(parseFrontendConstant(expression.constant,
                                          relation.columns[column],
                                          expression.location));
    }
    program_.addFact(relation.id, std::move(row));
  }

  void registerAtomVariables(const internal::Atom &atom, RuleScope &scope) {
    const RelationSpec &relation = findRelation(atom.relation, atom.location);
    checkArity(atom, relation);
    for (std::size_t column = 0; column < atom.arguments.size(); ++column) {
      const internal::Expression &argument = atom.arguments[column];
      if (argument.kind == internal::Expression::Kind::Variable &&
          argument.name != "_")
        variable(scope, argument.name, relation.columns[column],
                 argument.location);
    }
  }

  void registerRuleVariables(const internal::Rule &rule, RuleScope &scope) {
    for (const internal::Atom &head : rule.heads)
      registerAtomVariables(head, scope);
    for (const internal::BodyItem &item : rule.body) {
      std::visit(
          [&](const auto &body_item) {
            using T = std::decay_t<decltype(body_item)>;
            if constexpr (std::is_same_v<T, internal::PositiveItem> ||
                          std::is_same_v<T, internal::NegativeItem>) {
              registerAtomVariables(body_item.atom, scope);
            } else if constexpr (std::is_same_v<T,
                                                internal::AggregateItem>) {
              registerAtomVariables(body_item.source, scope);
            }
          },
          item);
    }
  }

  TermIR parseTerm(const internal::Expression &expression, TypeSpec expected,
                   RuleScope &scope, bool allow_expression) {
    if (expression.kind == internal::Expression::Kind::Variable) {
      TermIR term;
      term.kind = TermIR::Kind::Variable;
      term.type = expected.typeIndex();
      term.debug_name = expression.name;
      if (expression.name == "_") {
        term.variable = program_.addVariable("_", expected.typeIndex(), true);
        term.anonymous = true;
      } else {
        term.variable =
            variable(scope, expression.name, expected, expression.location).id;
      }
      return term;
    }
    if (expression.kind == internal::Expression::Kind::Constant) {
      TermIR term;
      term.kind = TermIR::Kind::Constant;
      term.type = expected.typeIndex();
      term.constant =
          parseFrontendConstant(expression.constant, expected,
                                expression.location);
      term.debug_name = "constant";
      return term;
    }
    if (!allow_expression)
      fail(expression.location,
           "body atoms accept only variables, wildcards, or constants");
    DynamicExpr dynamic = parseExpression(expression, scope, expected);
    TermIR term;
    term.kind = TermIR::Kind::Expression;
    term.type = expected.typeIndex();
    term.expression = std::move(dynamic.ir);
    term.debug_name = "expression";
    return term;
  }

  AtomIR parseAtom(const internal::Atom &atom, RuleScope &scope,
                   bool allow_expressions) {
    const RelationSpec &relation = findRelation(atom.relation, atom.location);
    checkArity(atom, relation);
    AtomIR result;
    result.relation = relation.id;
    result.relation_name = relation.name;
    for (std::size_t column = 0; column < atom.arguments.size(); ++column) {
      result.args.push_back(parseTerm(atom.arguments[column],
                                      relation.columns[column], scope,
                                      allow_expressions));
    }
    return result;
  }

  DynamicExpr parseExpression(
      const internal::Expression &expression, RuleScope &scope,
      std::optional<TypeSpec> expected = std::nullopt) {
    DynamicExpr result;
    try {
      switch (expression.kind) {
      case internal::Expression::Kind::Variable:
        if (expression.name == "_")
          fail(expression.location,
               "anonymous variable cannot be used in an expression");
        result = variableExpr(
            findVariable(scope, expression.name, expression.location));
        break;
      case internal::Expression::Kind::Constant: {
        TypeSpec type = expected.value_or(
            inferConstantType(expression.constant, expression.location));
        result = constantAnyExpr(
            type, parseFrontendConstant(expression.constant, type,
                                        expression.location));
        break;
      }
      case internal::Expression::Kind::Unary:
        if (expression.operands.size() != 1)
          fail(expression.location, "unary expression requires one operand");
        result = makeUnary(
            expression.name,
            parseExpression(expression.operands.front(), scope));
        break;
      case internal::Expression::Kind::Binary: {
        if (expression.operands.size() != 2)
          fail(expression.location, "binary expression requires two operands");
        DynamicExpr lhs = parseExpression(expression.operands[0], scope);
        DynamicExpr rhs = lhs.type.isLattice()
                              ? parseExpression(expression.operands[1], scope)
                              : parseExpression(expression.operands[1], scope,
                                                lhs.type);
        result = makeBinary(expression.name, std::move(lhs), std::move(rhs));
        break;
      }
      }
    } catch (const FrontendError &) {
      throw;
    } catch (const std::invalid_argument &error) {
      fail(expression.location, error.what());
    }
    if (expected && result.type != *expected) {
      fail(expression.location,
           "expression type " + result.type.name() +
               " does not match expected " + expected->name());
    }
    return result;
  }

  AggregateIR parseAggregate(const internal::AggregateItem &item,
                             RuleScope &scope) {
    AtomIR source = parseAtom(item.source, scope, false);
    DynamicExpr projection = constantExpr<std::int64_t>({ValueKind::I64}, 0);
    if (item.operation != "count") {
      if (!item.projection)
        fail(item.location, "aggregate requires a value expression");
      projection = parseExpression(*item.projection, scope);
    }

    AggregateIR aggregate;
    aggregate.source = std::move(source);
    aggregate.projection = std::move(projection.ir);
    aggregate.name = item.operation;
    TypeSpec output_type;
    try {
      output_type =
          configureAggregate(aggregate, item.operation, projection.type);
    } catch (const std::invalid_argument &error) {
      fail(item.location, error.what());
    }
    VariableSpec &output =
        variable(scope, item.output, output_type, item.location);
    aggregate.output_var = output.id;
    aggregate.output_type = output_type.typeIndex();
    return aggregate;
  }

  void parseRule(const internal::Rule &rule, std::size_t rule_index) {
    if (rule.heads.empty())
      fail(rule.location, "rule requires at least one head");
    RuleScope scope;
    scope.rule_index = rule_index;
    registerRuleVariables(rule, scope);

    std::vector<BodyItemIR> body;
    for (const internal::BodyItem &item : rule.body) {
      std::visit(
          [&](const auto &body_item) {
            using T = std::decay_t<decltype(body_item)>;
            if constexpr (std::is_same_v<T, internal::PositiveItem>) {
              body.push_back(parseAtom(body_item.atom, scope, false));
            } else if constexpr (std::is_same_v<T,
                                                internal::NegativeItem>) {
              body.push_back(
                  NegAtomIR{parseAtom(body_item.atom, scope, false)});
            } else if constexpr (std::is_same_v<T, internal::FilterItem>) {
              DynamicExpr predicate = parseExpression(
                  body_item.expression, scope, TypeSpec{ValueKind::Bool});
              body.push_back(FilterIR{std::move(predicate.ir)});
            } else {
              body.push_back(parseAggregate(body_item, scope));
            }
          },
          item);
    }

    for (const internal::Atom &head : rule.heads)
      program_.addRule({parseAtom(head, scope, true), body});
  }

  DynamicExpr makeUnary(StringRef operation, DynamicExpr operand) {
    if (operation == "!" && operand.type.kind == ValueKind::Bool) {
      return unaryExpr<bool, bool>(
          {ValueKind::Bool}, std::move(operand),
          [](bool value) { return !value; }, "not");
    }
    if (operation == "unary-" && operand.type.kind == ValueKind::I64) {
      return unaryExpr<std::int64_t, std::int64_t>(
          {ValueKind::I64}, std::move(operand),
          [](std::int64_t value) { return checkedNegate(value, "unary-"); },
          "unary-minus");
    }
    if (operation == "unary-" && operand.type.kind == ValueKind::F64) {
      return unaryExpr<double, double>(
          {ValueKind::F64}, std::move(operand),
          [](double value) { return checkedFinite(-value, "unary-"); },
          "unary-minus");
    }
    if (operation == "unary+")
      return operand;
    if (operation == "min_lattice" && operand.type.kind == ValueKind::I64) {
      return unaryExpr<std::int64_t, MinLattice<std::int64_t>>(
          {ValueKind::MinI64}, std::move(operand),
          [](std::int64_t value) { return MinLattice<std::int64_t>(value); },
          "min-lattice");
    }
    if (operation == "max_lattice" && operand.type.kind == ValueKind::I64) {
      return unaryExpr<std::int64_t, MaxLattice<std::int64_t>>(
          {ValueKind::MaxI64}, std::move(operand),
          [](std::int64_t value) { return MaxLattice<std::int64_t>(value); },
          "max-lattice");
    }
    if (operation == "min_lattice" && operand.type.kind == ValueKind::F64) {
      return unaryExpr<double, MinLattice<double>>(
          {ValueKind::MinF64}, std::move(operand),
          [](double value) { return MinLattice<double>(value); },
          "min-lattice");
    }
    if (operation == "max_lattice" && operand.type.kind == ValueKind::F64) {
      return unaryExpr<double, MaxLattice<double>>(
          {ValueKind::MaxF64}, std::move(operand),
          [](double value) { return MaxLattice<double>(value); },
          "max-lattice");
    }
    if (operation == "set_lattice" && operand.type.kind == ValueKind::I64) {
      return unaryExpr<std::int64_t, SetLattice<std::int64_t>>(
          {ValueKind::SetI64}, std::move(operand),
          [](std::int64_t value) { return SetLattice<std::int64_t>{value}; },
          "set-lattice");
    }
    throw std::invalid_argument("unsupported unary operation '" +
                                operation.str() + "' for " +
                                operand.type.name());
  }

  DynamicExpr makeBinary(StringRef operation, DynamicExpr lhs,
                         DynamicExpr rhs) {
    if (lhs.type != rhs.type) {
      if (operation == "+" && lhs.type.kind == ValueKind::MinI64 &&
          rhs.type.kind == ValueKind::I64) {
        return binaryExpr<MinLattice<std::int64_t>, std::int64_t,
                          MinLattice<std::int64_t>>(
            lhs.type, std::move(lhs), std::move(rhs),
            [](const auto &left, std::int64_t right) {
              return MinLattice<std::int64_t>(
                  checkedAdd(left.value(), right, "+"));
            },
            "lattice-addition");
      }
      if (operation == "+" && lhs.type.kind == ValueKind::MaxI64 &&
          rhs.type.kind == ValueKind::I64) {
        return binaryExpr<MaxLattice<std::int64_t>, std::int64_t,
                          MaxLattice<std::int64_t>>(
            lhs.type, std::move(lhs), std::move(rhs),
            [](const auto &left, std::int64_t right) {
              return MaxLattice<std::int64_t>(
                  checkedAdd(left.value(), right, "+"));
            },
            "lattice-addition");
      }
      if (operation == "+" && lhs.type.kind == ValueKind::MinF64 &&
          rhs.type.kind == ValueKind::F64) {
        return binaryExpr<MinLattice<double>, double, MinLattice<double>>(
            lhs.type, std::move(lhs), std::move(rhs),
            [](const auto &left, double right) {
              return MinLattice<double>(
                  checkedFinite(left.value() + right, "+"));
            },
            "lattice-addition");
      }
      if (operation == "+" && lhs.type.kind == ValueKind::MaxF64 &&
          rhs.type.kind == ValueKind::F64) {
        return binaryExpr<MaxLattice<double>, double, MaxLattice<double>>(
            lhs.type, std::move(lhs), std::move(rhs),
            [](const auto &left, double right) {
              return MaxLattice<double>(
                  checkedFinite(left.value() + right, "+"));
            },
            "lattice-addition");
      }
      throw std::invalid_argument("binary operands have incompatible types");
    }

    switch (lhs.type.kind) {
    case ValueKind::I64:
      return makeScalarBinary<std::int64_t>(operation, std::move(lhs),
                                            std::move(rhs));
    case ValueKind::U64:
      return makeScalarBinary<std::uint64_t>(operation, std::move(lhs),
                                             std::move(rhs));
    case ValueKind::F64:
      return makeScalarBinary<double>(operation, std::move(lhs),
                                      std::move(rhs));
    case ValueKind::String:
      return makeStringBinary(operation, std::move(lhs), std::move(rhs));
    case ValueKind::Bool:
      return makeBoolBinary(operation, std::move(lhs), std::move(rhs));
    default:
      throw std::invalid_argument("unsupported binary operands of type " +
                                  lhs.type.name());
    }
  }

  template <typename T>
  DynamicExpr makeScalarBinary(StringRef operation, DynamicExpr lhs,
                               DynamicExpr rhs) {
    const TypeSpec type = lhs.type;
    if (operation == "+") {
      if constexpr (std::is_integral_v<T>) {
        return sameTypeBinary<T>(
            type, std::move(lhs), std::move(rhs),
            [](T a, T b) { return checkedAdd(a, b, "+"); }, operation);
      } else {
        return sameTypeBinary<T>(
            type, std::move(lhs), std::move(rhs),
            [](T a, T b) { return checkedFinite(a + b, "+"); }, operation);
      }
    }
    if (operation == "-") {
      if constexpr (std::is_integral_v<T>) {
        return sameTypeBinary<T>(
            type, std::move(lhs), std::move(rhs),
            [](T a, T b) { return checkedSubtract(a, b, "-"); }, operation);
      } else {
        return sameTypeBinary<T>(
            type, std::move(lhs), std::move(rhs),
            [](T a, T b) { return checkedFinite(a - b, "-"); }, operation);
      }
    }
    if (operation == "*") {
      if constexpr (std::is_integral_v<T>) {
        return sameTypeBinary<T>(
            type, std::move(lhs), std::move(rhs),
            [](T a, T b) { return checkedMultiply(a, b, "*"); }, operation);
      } else {
        return sameTypeBinary<T>(
            type, std::move(lhs), std::move(rhs),
            [](T a, T b) { return checkedFinite(a * b, "*"); }, operation);
      }
    }
    if (operation == "/") {
      if constexpr (std::is_integral_v<T>) {
        return sameTypeBinary<T>(
            type, std::move(lhs), std::move(rhs),
            [](T a, T b) { return checkedDivide(a, b, "/"); }, operation);
      } else {
        return sameTypeBinary<T>(
            type, std::move(lhs), std::move(rhs),
            [](T a, T b) {
              if (b == T{}) {
                evaluationFailure(EvaluationErrorCode::DivisionByZero, "/",
                                  "division by zero");
              }
              return checkedFinite(a / b, "/");
            },
            operation);
      }
    }
    if (operation == "%") {
      if constexpr (std::is_integral_v<T>) {
        return sameTypeBinary<T>(
            type, std::move(lhs), std::move(rhs),
            [](T a, T b) { return checkedRemainder(a, b, "%"); }, operation);
      }
    }
    if (operation == "==")
      return comparison<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a == b; },
          operation);
    if (operation == "!=")
      return comparison<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a != b; },
          operation);
    if (operation == "<")
      return comparison<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a < b; },
          operation);
    if (operation == "<=")
      return comparison<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a <= b; },
          operation);
    if (operation == ">")
      return comparison<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a > b; },
          operation);
    if (operation == ">=")
      return comparison<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a >= b; },
          operation);
    throw std::invalid_argument("unsupported numeric operation '" +
                                operation.str() + "'");
  }

  DynamicExpr makeStringBinary(StringRef operation, DynamicExpr lhs,
                               DynamicExpr rhs) {
    const TypeSpec type{ValueKind::String};
    if (operation == "+")
      return sameTypeBinary<std::string>(
          type, std::move(lhs), std::move(rhs),
          [](const std::string &a, const std::string &b) { return a + b; },
          operation);
    if (operation == "==")
      return comparison<std::string>(type, std::move(lhs), std::move(rhs),
                                     std::equal_to<std::string>{}, operation);
    if (operation == "!=")
      return comparison<std::string>(type, std::move(lhs), std::move(rhs),
                                     std::not_equal_to<std::string>{},
                                     operation);
    if (operation == "<")
      return comparison<std::string>(type, std::move(lhs), std::move(rhs),
                                     std::less<std::string>{}, operation);
    throw std::invalid_argument("unsupported string operation '" +
                                operation.str() + "'");
  }

  DynamicExpr makeBoolBinary(StringRef operation, DynamicExpr lhs,
                             DynamicExpr rhs) {
    const TypeSpec type{ValueKind::Bool};
    if (operation == "&&")
      return sameTypeBinary<bool>(type, std::move(lhs), std::move(rhs),
                                  std::logical_and<bool>{}, operation);
    if (operation == "||")
      return sameTypeBinary<bool>(type, std::move(lhs), std::move(rhs),
                                  std::logical_or<bool>{}, operation);
    if (operation == "==")
      return comparison<bool>(type, std::move(lhs), std::move(rhs),
                              std::equal_to<bool>{}, operation);
    if (operation == "!=")
      return comparison<bool>(type, std::move(lhs), std::move(rhs),
                              std::not_equal_to<bool>{}, operation);
    throw std::invalid_argument("unsupported boolean operation '" +
                                operation.str() + "'");
  }

  template <typename Input, typename Output, typename State, typename MakeState,
            typename Add, typename Merge, typename Finish>
  void setReducer(AggregateIR &aggregate, MakeState make_state, Add add,
                  Merge merge, Finish finish) {
    ReducerIR reducer;
    reducer.make_state = [make_state] { return std::any(make_state()); };
    reducer.add = [add](std::any &state, const std::any &value) {
      add(std::any_cast<State &>(state), std::any_cast<const Input &>(value));
    };
    reducer.merge = [merge](std::any &state, const std::any &other) {
      merge(std::any_cast<State &>(state), std::any_cast<const State &>(other));
    };
    reducer.finish = [finish](std::any &state) {
      std::vector<Output> typed = finish(std::any_cast<State &>(state));
      std::vector<std::any> result;
      for (Output &value : typed)
        result.emplace_back(std::move(value));
      return result;
    };
    aggregate.reducer = reducer;
    aggregate.evaluate =
        [reducer = std::move(reducer)](const AggregateForEach &for_each) {
          std::any state = reducer.make_state();
          for_each([&](const std::any &value) { reducer.add(state, value); });
          return reducer.finish(state);
        };
  }

  template <typename T> struct OptionalState {
    std::optional<T> value;
  };

  struct MeanState {
    long double sum = 0;
    std::uint64_t count = 0;
  };

  TypeSpec configureAggregate(AggregateIR &aggregate, StringRef operation,
                              TypeSpec input_type) {
    if (operation == "count") {
      setReducer<std::int64_t, std::uint64_t, std::uint64_t>(
          aggregate, [] { return std::uint64_t{0}; },
          [](std::uint64_t &state, const std::int64_t &) {
            state = checkedAdd(state, std::uint64_t{1}, "aggregate-count");
          },
          [](std::uint64_t &state, const std::uint64_t &other) {
            state = checkedAdd(state, other, "aggregate-count");
          },
          [](std::uint64_t &state) {
            return std::vector<std::uint64_t>{state};
          });
      return {ValueKind::U64};
    }
    if (input_type.kind == ValueKind::I64)
      return configureNumericAggregate<std::int64_t>(aggregate, operation,
                                                     input_type);
    if (input_type.kind == ValueKind::F64)
      return configureNumericAggregate<double>(aggregate, operation,
                                               input_type);
    throw std::invalid_argument("aggregate '" + operation.str() +
                                "' requires an i64 or f64 projection");
  }

  template <typename T>
  TypeSpec configureNumericAggregate(AggregateIR &aggregate,
                                     StringRef operation, TypeSpec input_type) {
    if (operation == "sum") {
      setReducer<T, T, T>(
          aggregate, [] { return T{}; },
          [](T &state, const T &value) {
            if constexpr (std::is_integral_v<T>)
              state = checkedAdd(state, value, "aggregate-sum");
            else
              state = checkedFinite(state + value, "aggregate-sum");
          },
          [](T &state, const T &other) {
            if constexpr (std::is_integral_v<T>)
              state = checkedAdd(state, other, "aggregate-sum");
            else
              state = checkedFinite(state + other, "aggregate-sum");
          },
          [](T &state) { return std::vector<T>{state}; });
      return input_type;
    }
    if (operation == "min" || operation == "max") {
      const bool minimum = operation == "min";
      setReducer<T, T, OptionalState<T>>(
          aggregate, [] { return OptionalState<T>{}; },
          [minimum](OptionalState<T> &state, const T &value) {
            if (!state.value || (minimum ? std::less<T>{}(value, *state.value)
                                         : std::less<T>{}(*state.value, value)))
              state.value = value;
          },
          [minimum](OptionalState<T> &state, const OptionalState<T> &other) {
            if (other.value &&
                (!state.value ||
                 (minimum ? std::less<T>{}(*other.value, *state.value)
                          : std::less<T>{}(*state.value, *other.value))))
              state.value = other.value;
          },
          [](OptionalState<T> &state) {
            return state.value ? std::vector<T>{*state.value}
                               : std::vector<T>{};
          });
      return input_type;
    }
    if (operation == "mean") {
      setReducer<T, double, MeanState>(
          aggregate, [] { return MeanState{}; },
          [](MeanState &state, const T &value) {
            state.sum += static_cast<long double>(value);
            if (!std::isfinite(state.sum)) {
              evaluationFailure(EvaluationErrorCode::NonFiniteFloatingPoint,
                                "aggregate-mean",
                                "mean accumulator became non-finite");
            }
            state.count =
                checkedAdd(state.count, std::uint64_t{1}, "aggregate-mean");
          },
          [](MeanState &state, const MeanState &other) {
            state.sum += other.sum;
            if (!std::isfinite(state.sum)) {
              evaluationFailure(EvaluationErrorCode::NonFiniteFloatingPoint,
                                "aggregate-mean",
                                "mean accumulator became non-finite");
            }
            state.count =
                checkedAdd(state.count, other.count, "aggregate-mean");
          },
          [](MeanState &state) {
            if (state.count == 0)
              return std::vector<double>{};
            return std::vector<double>{
                checkedFinite(static_cast<double>(state.sum / state.count),
                              "aggregate-mean")};
          });
      return {ValueKind::F64};
    }
    throw std::invalid_argument("unknown aggregate '" + operation.str() + "'");
  }

  SemanticProgram program_;
  std::vector<RelationSpec> relations_;
  std::unordered_map<std::string, RelationId> relation_ids_;
  std::vector<RelationId> outputs_;
};

Value encodeScalar(const internal::Scalar &scalar) {
  if (const auto *integer = std::get_if<std::int64_t>(&scalar.value))
    return *integer;
  if (const auto *integer = std::get_if<std::uint64_t>(&scalar.value)) {
    if (*integer <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()))
      return static_cast<std::int64_t>(*integer);
    return std::to_string(*integer);
  }
  if (const auto *number = std::get_if<double>(&scalar.value))
    return *number;
  if (const auto *string = std::get_if<std::string>(&scalar.value))
    return *string;
  if (const auto *boolean = std::get_if<bool>(&scalar.value))
    return *boolean;
  Array values;
  for (std::int64_t value :
       std::get<internal::Scalar::IntegerSet>(scalar.value))
    values.push_back(value);
  return values;
}

Value encodeExpression(const internal::Expression &expression) {
  if (expression.kind == internal::Expression::Kind::Variable) {
    return expression.name == "_" ? Value("_")
                                  : Value("$" + expression.name);
  }
  if (expression.kind == internal::Expression::Kind::Constant)
    return Object{{"const", encodeScalar(expression.constant)}};
  Array operands;
  for (const internal::Expression &operand : expression.operands)
    operands.push_back(encodeExpression(operand));
  return Object{{"op", expression.name}, {"args", std::move(operands)}};
}

Object encodeAtom(const internal::Atom &atom) {
  Array arguments;
  for (const internal::Expression &expression : atom.arguments)
    arguments.push_back(encodeExpression(expression));
  return Object{{"relation", atom.relation}, {"args", std::move(arguments)}};
}

Object encodeProgram(const internal::FrontendIR &program) {
  Array relations;
  for (const internal::Relation &relation : program.relations) {
    Array columns;
    for (const std::string &column : relation.columns)
      columns.push_back(column);
    Object encoded{{"name", relation.name}, {"columns", std::move(columns)}};
    if (relation.lattice)
      encoded["kind"] = "lattice";

    Array facts;
    for (const internal::Fact &fact : program.facts) {
      if (fact.atom.relation != relation.name)
        continue;
      Array row;
      for (const internal::Expression &argument : fact.atom.arguments) {
        if (argument.kind != internal::Expression::Kind::Constant)
          throw FrontendError("invalid_program", argument.location.source,
                              argument.location.line, argument.location.column,
                              "facts can contain only constants");
        row.push_back(encodeScalar(argument.constant));
      }
      facts.push_back(std::move(row));
    }
    if (!facts.empty())
      encoded["facts"] = std::move(facts);
    relations.push_back(std::move(encoded));
  }

  Array rules;
  for (const internal::Rule &rule : program.rules) {
    Array body;
    for (const internal::BodyItem &item : rule.body) {
      body.push_back(std::visit(
          [](const auto &body_item) -> Value {
            using T = std::decay_t<decltype(body_item)>;
            if constexpr (std::is_same_v<T, internal::PositiveItem>) {
              return Object{{"atom", encodeAtom(body_item.atom)}};
            } else if constexpr (std::is_same_v<T,
                                                internal::NegativeItem>) {
              return Object{{"not", encodeAtom(body_item.atom)}};
            } else if constexpr (std::is_same_v<T, internal::FilterItem>) {
              return Object{
                  {"where", encodeExpression(body_item.expression)}};
            } else {
              Object aggregate{{"op", body_item.operation},
                               {"output", "$" + body_item.output},
                               {"source", encodeAtom(body_item.source)}};
              if (body_item.projection)
                aggregate["value"] =
                    encodeExpression(*body_item.projection);
              return Object{{"aggregate", std::move(aggregate)}};
            }
          },
          item));
    }
    Object encoded_rule{{"body", std::move(body)}};
    if (rule.heads.size() == 1) {
      encoded_rule["head"] = encodeAtom(rule.heads.front());
    } else {
      Array heads;
      for (const internal::Atom &head : rule.heads)
        heads.push_back(encodeAtom(head));
      encoded_rule["heads"] = std::move(heads);
    }
    rules.push_back(std::move(encoded_rule));
  }

  Array outputs;
  for (const internal::Output &output : program.outputs)
    outputs.push_back(output.relation);
  Object root{{"schema_version", JSON_SCHEMA_VERSION},
              {"relations", std::move(relations)},
              {"rules", std::move(rules)}};
  if (!outputs.empty())
    root["outputs"] = std::move(outputs);
  return root;
}

} // namespace

void executeJson(StringRef input, const RunOptions &options,
                 llvm::raw_ostream &output) {
  internal::execute(internal::parseJson(SourceUnit{"<json>", input}), options,
                    output);
}

void internal::execute(const internal::FrontendIR &root,
                       const RunOptions &options,
                       llvm::raw_ostream &output) {
  LoweredProgram program(root);
  Object result = program.execute(options);
  if (options.pretty)
    output << llvm::formatv("{0:2}\n", Value(std::move(result)));
  else
    output << llvm::formatv("{0}\n", Value(std::move(result)));
}

std::string internal::toJson(const internal::FrontendIR &program) {
  return llvm::formatv("{0}", Value(encodeProgram(program))).str();
}

void printJsonError(const std::exception &error, llvm::raw_ostream &output,
                    bool pretty) {
  Object detail;
  detail["message"] = error.what();
  if (const auto *evaluation = dynamic_cast<const EvaluationError *>(&error)) {
    detail["category"] = "evaluation";
    detail["code"] = toString(evaluation->code());
    detail["expression"] = evaluation->expression();
  } else if (dynamic_cast<const CompileError *>(&error)) {
    detail["category"] = "semantic";
    detail["code"] = "compile_error";
  } else if (const auto *frontend =
                 dynamic_cast<const FrontendError *>(&error)) {
    detail["category"] = "input";
    detail["code"] = frontend->code();
    detail["source"] = frontend->source();
    detail["line"] = static_cast<std::int64_t>(frontend->line());
    detail["column"] = static_cast<std::int64_t>(frontend->column());
  } else if (dynamic_cast<const std::invalid_argument *>(&error)) {
    detail["category"] = "input";
    detail["code"] = "invalid_program";
  } else {
    detail["category"] = "runtime";
    detail["code"] = "runtime_error";
  }

  Object result;
  result["schema_version"] = JSON_SCHEMA_VERSION;
  result["status"] = "error";
  result["error"] = std::move(detail);
  if (pretty)
    output << llvm::formatv("{0:2}\n", Value(std::move(result)));
  else
    output << llvm::formatv("{0}\n", Value(std::move(result)));
}

void printSchema(llvm::raw_ostream &output) {
  output << R"json({
  "schema_version": 1,
  "relations": [
    {
      "name": "edge",
      "columns": ["i64", "i64"],
      "facts": [[1, 2], [2, 3]]
    },
    {
      "name": "path",
      "columns": ["i64", "i64"]
    }
  ],
  "rules": [
    {
      "head": {"relation": "path", "args": ["$x", "$y"]},
      "body": [
        {"atom": {"relation": "edge", "args": ["$x", "$y"]}}
      ]
    },
    {
      "head": {"relation": "path", "args": ["$x", "$z"]},
      "body": [
        {"atom": {"relation": "path", "args": ["$x", "$y"]}},
        {"atom": {"relation": "edge", "args": ["$y", "$z"]}}
      ]
    }
  ],
  "outputs": ["path"]
})json";
}

} // namespace lotus::datalog::frontend
