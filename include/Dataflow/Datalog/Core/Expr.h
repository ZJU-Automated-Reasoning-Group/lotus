#pragma once

#include "Dataflow/Datalog/Semantic/SemanticIR.h"
#include "Dataflow/Datalog/Core/TypeSupport.h"

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
       std::function<T(const Binding &)> evaluator, std::string debug_name)
      : context_(context), referenced_vars_(std::move(referenced_vars)),
        evaluator_(std::move(evaluator)), debug_name_(std::move(debug_name)) {}

  static Expr constant(T value) {
    return Expr(
        nullptr, {},
        [value = std::move(value)](const Binding &) { return value; },
        "constant");
  }

  Context *context() const { return context_; }
  const std::vector<VarId> &referencedVars() const { return referenced_vars_; }
  const std::string &debugName() const { return debug_name_; }

  T evaluate(const Binding &binding) const { return evaluator_(binding); }

  ExprIR lower() const {
    ExprIR result;
    result.type = typeid(T);
    result.referenced_vars = referenced_vars_;
    result.debug_name = debug_name_;
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
            name),
        id_(id), name_(std::move(name)) {}

  VarId id() const { return id_; }
  const std::string &name() const { return name_; }

private:
  VarId id_ = 0;
  std::string name_;
};

template <typename T> Expr<std::decay_t<T>> val(T &&value) {
  return Expr<std::decay_t<T>>::constant(std::forward<T>(value));
}

#define LOTUS_DATALOG_BINARY_EXPR(OP, LABEL)                                   \
  template <typename L, typename R>                                            \
  auto operator OP(const Expr<L> &lhs, const Expr<R> &rhs)                     \
      ->Expr<decltype(std::declval<L>() OP std::declval<R>())> {               \
    using Result = decltype(std::declval<L>() OP std::declval<R>());           \
    Context *context = detail::mergeContexts(lhs.context(), rhs.context());    \
    auto refs =                                                                \
        detail::mergeReferences(lhs.referencedVars(), rhs.referencedVars());   \
    return Expr<Result>(                                                       \
        context, std::move(refs),                                              \
        [lhs, rhs](const Binding &binding) {                                   \
          return lhs.evaluate(binding) OP rhs.evaluate(binding);               \
        },                                                                     \
        LABEL);                                                                \
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

LOTUS_DATALOG_BINARY_EXPR(+, "addition")
LOTUS_DATALOG_BINARY_EXPR(-, "subtraction")
LOTUS_DATALOG_BINARY_EXPR(*, "multiplication")
LOTUS_DATALOG_BINARY_EXPR(/, "division")
LOTUS_DATALOG_BINARY_EXPR(%, "remainder")
LOTUS_DATALOG_BINARY_EXPR(==, "equality")
LOTUS_DATALOG_BINARY_EXPR(!=, "inequality")
LOTUS_DATALOG_BINARY_EXPR(<, "less-than")
LOTUS_DATALOG_BINARY_EXPR(<=, "less-equal")
LOTUS_DATALOG_BINARY_EXPR(>, "greater-than")
LOTUS_DATALOG_BINARY_EXPR(>=, "greater-equal")
LOTUS_DATALOG_BINARY_EXPR(&&, "logical-and")
LOTUS_DATALOG_BINARY_EXPR(||, "logical-or")

#undef LOTUS_DATALOG_BINARY_EXPR

template <typename T> auto operator-(const Expr<T> &expr) -> Expr<decltype(-T{})> {
  using Result = decltype(-T{});
  return Expr<Result>(
      expr.context(), expr.referencedVars(),
      [expr](const Binding &binding) { return -expr.evaluate(binding); },
      "unary-minus");
}

template <typename T> auto operator+(const Expr<T> &expr) -> Expr<decltype(+T{})> {
  using Result = decltype(+T{});
  return Expr<Result>(
      expr.context(), expr.referencedVars(),
      [expr](const Binding &binding) { return +expr.evaluate(binding); },
      "unary-plus");
}

inline Expr<bool> operator!(const Expr<bool> &expr) {
  return Expr<bool>(
      expr.context(), expr.referencedVars(),
      [expr](const Binding &binding) { return !expr.evaluate(binding); },
      "logical-not");
}

template <typename Function, typename... Ts>
auto lift(Function function, const Expr<Ts> &...args)
    -> Expr<std::invoke_result_t<Function, Ts...>> {
  using Result = std::invoke_result_t<Function, Ts...>;
  Context *context = nullptr;
  ((context = detail::mergeContexts(context, args.context())), ...);
  std::vector<VarId> references;
  ((references =
        detail::mergeReferences(references, args.referencedVars())), ...);
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
      "lift");
}

template <typename T> using var = Var<T>;
template <typename T> using expr = Expr<T>;
template <typename T> using value = Expr<T>;

} // namespace lotus::datalog
