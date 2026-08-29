#pragma once

#include "Solvers/Datalog/Core/TypeSupport.h"
#include "Solvers/Datalog/Semantic/SemanticIR.h"

#include <any>
#include <functional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace lotus::datalog {

template <typename T> class Expr {
public:
  using value_type = T;

  Expr() = delete;

  Expr(Context *context, std::vector<VarId> referenced_vars,
       std::function<T(const Binding &)> evaluator, std::string debug_name,
       FunctionProperties properties = {}, std::shared_ptr<ExprNode> node = {})
      : context_(context), referenced_vars_(std::move(referenced_vars)),
        evaluator_(std::move(evaluator)), debug_name_(std::move(debug_name)),
        properties_(properties), node_(std::move(node)) {}

  static Expr constant(T value) {
    auto node = std::make_shared<ExprNode>();
    node->opcode = ExprOpcode::Constant;
    node->type = typeid(T);
    node->constant = value;
    return Expr(
        nullptr, {},
        [value = std::move(value)](const Binding &) { return value; },
        "constant", FunctionProperties::parallel(), std::move(node));
  }

  Context *context() const { return context_; }
  const std::vector<VarId> &referencedVars() const { return referenced_vars_; }
  const std::string &debugName() const { return debug_name_; }
  FunctionProperties properties() const { return properties_; }
  const std::shared_ptr<ExprNode> &node() const { return node_; }

  T evaluate(const Binding &binding) const { return evaluator_(binding); }

  ExprIR lower() const {
    ExprIR result;
    result.type = typeid(T);
    result.referenced_vars = referenced_vars_;
    result.debug_name = debug_name_;
    result.properties = properties_;
    result.node = node_;
    result.jit_safe = static_cast<bool>(node_);
    auto evaluator = evaluator_;
    result.evaluate = [evaluator =
                           std::move(evaluator)](const Binding &binding) {
      return std::any(evaluator(binding));
    };
    return result;
  }

protected:
  Context *context_ = nullptr;
  std::vector<VarId> referenced_vars_;
  std::function<T(const Binding &)> evaluator_;
  std::string debug_name_;
  FunctionProperties properties_;
  std::shared_ptr<ExprNode> node_;
};

template <typename T> class Var : public Expr<T> {
public:
  Var() = delete;

  Var(Context *context, VarId id, std::string name)
      : Expr<T>(
            context, {id},
            [id](const Binding &binding) {
              if (id >= binding.size() || !binding[id])
                throw std::logic_error(
                    "evaluating an unbound Datalog variable");
              return binding[id].get<T>();
            },
            name, FunctionProperties::parallel(), makeNode(id)),
        id_(id), name_(std::move(name)) {}

  VarId id() const { return id_; }
  const std::string &name() const { return name_; }

private:
  static std::shared_ptr<ExprNode> makeNode(VarId id) {
    auto node = std::make_shared<ExprNode>();
    node->opcode = ExprOpcode::Variable;
    node->type = typeid(T);
    node->variable = id;
    return node;
  }

  VarId id_ = 0;
  std::string name_;
};

template <typename T> Expr<std::decay_t<T>> val(T &&value) {
  return Expr<std::decay_t<T>>::constant(std::forward<T>(value));
}

#define LOTUS_DATALOG_BINARY_EXPR(OP, LABEL, OPCODE)                           \
  template <typename L, typename R>                                            \
  auto operator OP(const Expr<L> &lhs, const Expr<R> &rhs)                     \
      ->Expr<decltype(std::declval<L>() OP std::declval<R>())> {               \
    using Result = decltype(std::declval<L>() OP std::declval<R>());           \
    Context *context = detail::mergeContexts(lhs.context(), rhs.context());    \
    auto refs =                                                                \
        detail::mergeReferences(lhs.referencedVars(), rhs.referencedVars());   \
    auto node = std::make_shared<ExprNode>();                                  \
    node->opcode = ExprOpcode::OPCODE;                                         \
    node->type = typeid(Result);                                               \
    node->lhs = lhs.node();                                                    \
    node->rhs = rhs.node();                                                    \
    if (!node->lhs || !node->rhs)                                              \
      node.reset();                                                            \
    return Expr<Result>(                                                       \
        context, std::move(refs),                                              \
        [lhs, rhs](const Binding &binding) {                                   \
          return lhs.evaluate(binding) OP rhs.evaluate(binding);               \
        },                                                                     \
        LABEL,                                                                 \
        lhs.properties().canRunInParallel() &&                                 \
                rhs.properties().canRunInParallel()                            \
            ? FunctionProperties::parallel()                                   \
            : FunctionProperties{},                                            \
        std::move(node));                                                      \
  }                                                                            \
  template <typename L, typename R,                                            \
            std::enable_if_t<!detail::IsExpression<std::decay_t<R>>::value,    \
                             int> = 0>                                         \
  auto operator OP(const Expr<L> &lhs, R &&rhs)                                \
      ->decltype(lhs OP val(std::forward<R>(rhs))) {                           \
    return lhs OP val(std::forward<R>(rhs));                                   \
  }                                                                            \
  template <typename L, typename R,                                            \
            std::enable_if_t<!detail::IsExpression<std::decay_t<L>>::value,    \
                             int> = 0>                                         \
  auto operator OP(L &&lhs, const Expr<R> &rhs)                                \
      ->decltype(val(std::forward<L>(lhs)) OP rhs) {                           \
    return val(std::forward<L>(lhs)) OP rhs;                                   \
  }

LOTUS_DATALOG_BINARY_EXPR(+, "addition", Add)
LOTUS_DATALOG_BINARY_EXPR(-, "subtraction", Subtract)
LOTUS_DATALOG_BINARY_EXPR(*, "multiplication", Multiply)
LOTUS_DATALOG_BINARY_EXPR(/, "division", Divide)
LOTUS_DATALOG_BINARY_EXPR(%, "remainder", Remainder)
LOTUS_DATALOG_BINARY_EXPR(==, "equality", Equal)
LOTUS_DATALOG_BINARY_EXPR(!=, "inequality", NotEqual)
LOTUS_DATALOG_BINARY_EXPR(<, "less-than", Less)
LOTUS_DATALOG_BINARY_EXPR(<=, "less-equal", LessEqual)
LOTUS_DATALOG_BINARY_EXPR(>, "greater-than", Greater)
LOTUS_DATALOG_BINARY_EXPR(>=, "greater-equal", GreaterEqual)
LOTUS_DATALOG_BINARY_EXPR(&&, "logical-and", LogicalAnd)
LOTUS_DATALOG_BINARY_EXPR(||, "logical-or", LogicalOr)

#undef LOTUS_DATALOG_BINARY_EXPR

template <typename T>
auto operator-(const Expr<T> &expr) -> Expr<decltype(-T{})> {
  using Result = decltype(-T{});
  auto node = std::make_shared<ExprNode>();
  node->opcode = ExprOpcode::Negate;
  node->type = typeid(Result);
  node->lhs = expr.node();
  if (!node->lhs)
    node.reset();
  return Expr<Result>(
      expr.context(), expr.referencedVars(),
      [expr](const Binding &binding) { return -expr.evaluate(binding); },
      "unary-minus", expr.properties(), std::move(node));
}

template <typename T>
auto operator+(const Expr<T> &expr) -> Expr<decltype(+T{})> {
  using Result = decltype(+T{});
  auto node = std::make_shared<ExprNode>();
  node->opcode = ExprOpcode::Positive;
  node->type = typeid(Result);
  node->lhs = expr.node();
  if (!node->lhs)
    node.reset();
  return Expr<Result>(
      expr.context(), expr.referencedVars(),
      [expr](const Binding &binding) { return +expr.evaluate(binding); },
      "unary-plus", expr.properties(), std::move(node));
}

inline Expr<bool> operator!(const Expr<bool> &expr) {
  auto node = std::make_shared<ExprNode>();
  node->opcode = ExprOpcode::LogicalNot;
  node->type = typeid(bool);
  node->lhs = expr.node();
  if (!node->lhs)
    node.reset();
  return Expr<bool>(
      expr.context(), expr.referencedVars(),
      [expr](const Binding &binding) { return !expr.evaluate(binding); },
      "logical-not", expr.properties(), std::move(node));
}

template <typename Function, typename... Ts>
auto lift(FunctionProperties properties, Function function,
          const Expr<Ts> &...args)
    -> Expr<std::invoke_result_t<Function, Ts...>> {
  using Result = std::invoke_result_t<Function, Ts...>;
  Context *context = nullptr;
  ((context = detail::mergeContexts(context, args.context())), ...);
  std::vector<VarId> references;
  ((references = detail::mergeReferences(references, args.referencedVars())),
   ...);
  auto expressions = std::make_tuple(args...);
  return Expr<Result>(
      context, std::move(references),
      [function = std::move(function),
       expressions = std::move(expressions)](const Binding &binding) {
        return std::apply(
            [&](const auto &...expression) {
              return std::invoke(function, expression.evaluate(binding)...);
            },
            expressions);
      },
      "lift", properties);
}

template <typename Function, typename... Ts>
auto lift(Function function, const Expr<Ts> &...args)
    -> Expr<std::invoke_result_t<Function, Ts...>> {
  return lift(FunctionProperties{}, std::move(function), args...);
}

template <typename T> using var = Var<T>;
template <typename T> using expr = Expr<T>;
template <typename T> using value = Expr<T>;

} // namespace lotus::datalog
