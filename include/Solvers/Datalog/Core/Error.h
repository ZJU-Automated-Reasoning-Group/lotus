#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace lotus::datalog {

class CompileError : public std::runtime_error {
public:
  explicit CompileError(const std::string &message)
      : std::runtime_error(message) {}
};

enum class EvaluationErrorCode {
  IntegerOverflow,
  DivisionByZero,
  RemainderByZero,
  NonFiniteFloatingPoint,
};

inline const char *toString(EvaluationErrorCode code) {
  switch (code) {
  case EvaluationErrorCode::IntegerOverflow:
    return "integer_overflow";
  case EvaluationErrorCode::DivisionByZero:
    return "division_by_zero";
  case EvaluationErrorCode::RemainderByZero:
    return "remainder_by_zero";
  case EvaluationErrorCode::NonFiniteFloatingPoint:
    return "non_finite_floating_point";
  }
  return "evaluation_error";
}

class EvaluationError : public std::runtime_error {
public:
  EvaluationError(EvaluationErrorCode code, std::string expression,
                  std::string message)
      : std::runtime_error(std::move(message)), code_(code),
        expression_(std::move(expression)) {}

  EvaluationErrorCode code() const { return code_; }
  const std::string &expression() const { return expression_; }

private:
  EvaluationErrorCode code_;
  std::string expression_;
};

} // namespace lotus::datalog
