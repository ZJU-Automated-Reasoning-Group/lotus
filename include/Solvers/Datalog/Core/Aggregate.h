#pragma once

#include "Solvers/Datalog/Core/Atom.h"
#include "Solvers/Datalog/Core/Expr.h"
#include "Solvers/Datalog/Semantic/SemanticIR.h"

#include <any>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace lotus::datalog {

namespace detail {

template <typename T> constexpr ReducerProperties additiveReducerProperties() {
  if constexpr (std::is_integral_v<T> &&
                !std::is_same_v<std::remove_cv_t<T>, bool>)
    return ReducerProperties::parallel();
  return {};
}

template <typename T> constexpr ReducerProperties orderedReducerProperties() {
  if constexpr (std::is_integral_v<T> &&
                !std::is_same_v<std::remove_cv_t<T>, bool>)
    return ReducerProperties::parallel();
  return {};
}

template <typename T> bool preferMinimum(const T &candidate, const T &current) {
  if constexpr (std::is_floating_point_v<T>) {
    // NaN is treated as a missing value when a numeric value is available.
    if (std::isnan(candidate))
      return false;
    if (std::isnan(current))
      return true;
  }
  return candidate < current;
}

template <typename T> bool preferMaximum(const T &candidate, const T &current) {
  if constexpr (std::is_floating_point_v<T>) {
    // NaN is treated as a missing value when a numeric value is available.
    if (std::isnan(candidate))
      return false;
    if (std::isnan(current))
      return true;
  }
  return current < candidate;
}

} // namespace detail

template <typename T> class AggregateRange {
public:
  template <typename Function> void forEach(Function &&function) const {
    for_each_([&](const std::any &value) {
      function(std::any_cast<const T &>(value));
    });
  }

  std::vector<T> collect() const {
    std::vector<T> values;
    forEach([&](const T &value) { values.push_back(value); });
    return values;
  }

private:
  explicit AggregateRange(const AggregateForEach &for_each)
      : for_each_(for_each) {}

  const AggregateForEach &for_each_;

  template <typename Input, typename Output> friend class AggregatorSpec;
};

template <typename Input, typename Output> class AggregatorSpec {
public:
  AggregatorSpec(
      Expr<Input> projection, std::string name,
      std::function<std::vector<Output>(const std::vector<Input> &)> evaluator,
      FunctionProperties properties = {})
      : projection_(std::move(projection)), name_(std::move(name)),
        properties_(properties) {
    evaluator_ = [evaluator =
                      std::move(evaluator)](const AggregateForEach &for_each) {
      std::vector<Input> typed_values;
      for_each([&](const std::any &value) {
        typed_values.push_back(std::any_cast<const Input &>(value));
      });
      std::vector<Output> typed_results = evaluator(typed_values);
      std::vector<std::any> results;
      results.reserve(typed_results.size());
      for (Output &result : typed_results)
        results.emplace_back(std::move(result));
      return results;
    };
  }

  AggregatorSpec(
      Expr<Input> projection, std::string name,
      std::function<std::vector<Output>(const AggregateRange<Input> &)>
          evaluator,
      FunctionProperties properties = {})
      : projection_(std::move(projection)), name_(std::move(name)),
        properties_(properties) {
    evaluator_ = [evaluator =
                      std::move(evaluator)](const AggregateForEach &for_each) {
      AggregateRange<Input> range(for_each);
      std::vector<Output> typed_results = evaluator(range);
      std::vector<std::any> results;
      results.reserve(typed_results.size());
      for (Output &result : typed_results)
        results.emplace_back(std::move(result));
      return results;
    };
  }

  template <typename MakeState, typename Add, typename Merge, typename Finish>
  AggregatorSpec(Expr<Input> projection, std::string name, MakeState make_state,
                 Add add, Merge merge, Finish finish,
                 ReducerProperties properties = {})
      : projection_(std::move(projection)), name_(std::move(name)) {
    using State = std::invoke_result_t<MakeState>;
    ReducerIR reducer;
    reducer.make_state = [make_state] { return std::any(make_state()); };
    reducer.add = [add](std::any &state, const std::any &value) {
      add(std::any_cast<State &>(state), std::any_cast<const Input &>(value));
    };
    reducer.merge = [merge](std::any &state, const std::any &other) {
      merge(std::any_cast<State &>(state), std::any_cast<const State &>(other));
    };
    reducer.finish = [finish](std::any &state) {
      std::vector<Output> typed_results = finish(std::any_cast<State &>(state));
      std::vector<std::any> results;
      results.reserve(typed_results.size());
      for (Output &result : typed_results)
        results.emplace_back(std::move(result));
      return results;
    };
    reducer.properties = properties;
    reducer_ = std::move(reducer);
    properties_ = properties.canRunInParallel() ? FunctionProperties::parallel()
                                                : FunctionProperties{};
    evaluator_ = [reducer = *reducer_](const AggregateForEach &for_each) {
      std::any state = reducer.make_state();
      for_each([&](const std::any &value) { reducer.add(state, value); });
      return reducer.finish(state);
    };
  }

  Context *context() const { return projection_.context(); }

  AggregatorSpec monotone() const {
    AggregatorSpec result = *this;
    result.monotone_ = true;
    return result;
  }

private:
  Expr<Input> projection_;
  std::string name_;
  std::function<std::vector<std::any>(const AggregateForEach &)> evaluator_;
  std::optional<ReducerIR> reducer_;
  FunctionProperties properties_;
  bool monotone_ = false;

  template <typename In, typename Out>
  friend AggregateClause
  aggregate(const Var<Out> &, const AggregatorSpec<In, Out> &, const Atom &);
  template <typename In, typename Out>
  friend AggregateClause
  aggregate(const Var<Out> &, const AggregatorSpec<In, Out> &, const Body &);
};

template <typename Output, typename Input, typename Function>
AggregatorSpec<Input, Output>
make_aggregator(const Expr<Input> &projection, std::string name,
                Function evaluator, FunctionProperties properties = {}) {
  return AggregatorSpec<Input, Output>(
      projection, std::move(name),
      std::function<std::vector<Output>(const std::vector<Input> &)>(
          std::move(evaluator)),
      properties);
}

template <typename Output, typename Input, typename Function>
AggregatorSpec<Input, Output>
make_streaming_aggregator(const Expr<Input> &projection, std::string name,
                          Function evaluator,
                          FunctionProperties properties = {}) {
  using Evaluator =
      std::function<std::vector<Output>(const AggregateRange<Input> &)>;
  return AggregatorSpec<Input, Output>(
      projection, std::move(name), Evaluator(std::move(evaluator)), properties);
}

template <typename Output, typename Input, typename MakeState, typename Add,
          typename Merge, typename Finish>
AggregatorSpec<Input, Output>
make_reducible_aggregator(const Expr<Input> &projection, std::string name,
                          MakeState make_state, Add add, Merge merge,
                          Finish finish, ReducerProperties properties = {}) {
  return AggregatorSpec<Input, Output>(
      projection, std::move(name), std::move(make_state), std::move(add),
      std::move(merge), std::move(finish), properties);
}

template <typename T> AggregatorSpec<T, T> sum(const Expr<T> &projection) {
  return AggregatorSpec<T, T>(
      projection, "sum", [] { return T{}; },
      [](T &state, const T &value) { state += value; },
      [](T &state, const T &other) { state += other; },
      [](T &state) { return std::vector<T>{state}; },
      detail::additiveReducerProperties<T>());
}

inline AggregatorSpec<int, std::size_t> count() {
  return AggregatorSpec<int, std::size_t>(
      Expr<int>::constant(0), "count", [] { return std::size_t{0}; },
      [](std::size_t &state, const int &) { ++state; },
      [](std::size_t &state, const std::size_t &other) { state += other; },
      [](std::size_t &state) { return std::vector<std::size_t>{state}; },
      ReducerProperties::parallel());
}

template <typename T> AggregatorSpec<T, T> minimum(const Expr<T> &projection) {
  struct State {
    std::optional<T> value;
  };
  return AggregatorSpec<T, T>(
      projection, "minimum", [] { return State{}; },
      [](State &state, const T &value) {
        if (!state.value || detail::preferMinimum(value, *state.value))
          state.value = value;
      },
      [](State &state, const State &other) {
        if (other.value &&
            (!state.value || detail::preferMinimum(*other.value, *state.value)))
          state.value = other.value;
      },
      [](State &state) {
        return state.value ? std::vector<T>{*state.value} : std::vector<T>{};
      },
      detail::orderedReducerProperties<T>());
}

template <typename T> AggregatorSpec<T, T> maximum(const Expr<T> &projection) {
  struct State {
    std::optional<T> value;
  };
  return AggregatorSpec<T, T>(
      projection, "maximum", [] { return State{}; },
      [](State &state, const T &value) {
        if (!state.value || detail::preferMaximum(value, *state.value))
          state.value = value;
      },
      [](State &state, const State &other) {
        if (other.value &&
            (!state.value || detail::preferMaximum(*other.value, *state.value)))
          state.value = other.value;
      },
      [](State &state) {
        return state.value ? std::vector<T>{*state.value} : std::vector<T>{};
      },
      detail::orderedReducerProperties<T>());
}

template <typename T>
AggregatorSpec<T, double> mean(const Expr<T> &projection) {
  struct State {
    long double sum = 0;
    std::size_t count = 0;
  };
  return AggregatorSpec<T, double>(
      projection, "mean", [] { return State{}; },
      [](State &state, const T &value) {
        state.sum += static_cast<long double>(value);
        ++state.count;
      },
      [](State &state, const State &other) {
        state.sum += other.sum;
        state.count += other.count;
      },
      [](State &state) {
        if (state.count == 0)
          return std::vector<double>{};
        return std::vector<double>{
            static_cast<double>(state.sum / state.count)};
      },
      {});
}

class AggregateClause {
public:
  Context *context() const { return context_; }
  const AggregateIR &ir() const { return ir_; }

private:
  AggregateClause(Context *context, AggregateIR ir)
      : context_(context), ir_(std::move(ir)) {}

  Context *context_ = nullptr;
  AggregateIR ir_;

  template <typename Input, typename Output>
  friend AggregateClause aggregate(const Var<Output> &,
                                   const AggregatorSpec<Input, Output> &,
                                   const Atom &);
  template <typename Input, typename Output>
  friend AggregateClause aggregate(const Var<Output> &,
                                   const AggregatorSpec<Input, Output> &,
                                   const Body &);
  friend class Body;
  friend class Program;
};

template <typename Input, typename Output>
AggregateClause aggregate(const Var<Output> &output,
                          const AggregatorSpec<Input, Output> &aggregator,
                          const Atom &source) {
  Context *context =
      detail::mergeContexts(output.context(), aggregator.context());
  context = detail::mergeContexts(context, source.context());
  AggregateIR ir;
  ir.output_var = output.id();
  ir.output_type = typeid(Output);
  ir.source = source.ir();
  ir.source_body.push_back(source.ir());
  ir.projection = aggregator.projection_.lower();
  ir.name = aggregator.name_;
  ir.evaluate = aggregator.evaluator_;
  ir.reducer = aggregator.reducer_;
  ir.properties = aggregator.properties_;
  ir.monotone = aggregator.monotone_;
  return AggregateClause(context, std::move(ir));
}

} // namespace lotus::datalog
