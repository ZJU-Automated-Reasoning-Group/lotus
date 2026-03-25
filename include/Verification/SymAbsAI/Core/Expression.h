/**
 * @file Expression.h
 * @brief Type-safe wrapper for symbolic expressions used in abstract domains.
 *
 * This header defines a polymorphic expression system that allows abstract
 * domains to work with symbolic expressions while maintaining type safety. The
 * Expression class serves as a value type wrapper around ExpressionBase,
 * enabling efficient copying and passing while hiding implementation details.
 *
 * Expressions can be constructed from:
 * - RepresentedValue (variables in the SMT encoding)
 * - ConcreteState::Value (concrete values)
 * - Literal constants (booleans)
 *
 * Expressions support:
 * - Arithmetic operations (addition, subtraction, multiplication)
 * - Bitwise extensions (zero-extend, sign-extend)
 * - Comparisons (unsigned less-or-equal, equality)
 * - Conversion to SMT formulas via toFormula()
 * - Evaluation against concrete states via eval()
 *
 * @see RepresentedValue
 * @see ConcreteState
 */
#pragma once
#include "Verification/SymAbsAI/Core/ConcreteState.h"
#include "Verification/SymAbsAI/Core/ResultStore.h"
#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

namespace symabs_ai {
/**
 * @brief Abstract base class for symbolic expressions.
 *
 * This interface defines the operations that all expression implementations
 * must support. Concrete implementations represent different kinds of
 * expressions (e.g., variables, constants, binary operations).
 *
 * All operations are virtual and must be implemented by derived classes.
 * ExpressionBase objects should not be used directly - use the Expression
 * wrapper class instead.
 */
class ExpressionBase {
public:
  virtual unsigned bits() const = 0;
  virtual z3::expr toFormula(const ValueMapping &) const = 0;
  virtual ConcreteState::Value eval(const ConcreteState &) const = 0;
  virtual void prettyPrint(PrettyPrinter &out) const = 0;
  virtual bool operator==(const ExpressionBase &other) const = 0;
  virtual ~ExpressionBase() = default;
};

class Expression : public ExpressionBase {
private:
  shared_ptr<ExpressionBase> Instance_;
  Expression(shared_ptr<ExpressionBase> ptr) : Instance_(ptr) {}

public:
  Expression(const RepresentedValue &rv);
  Expression(const ConcreteState::Value &value);

  Expression(const FunctionContext &fctx, bool x)
      : Expression(ConcreteState::Value(fctx, x, 1)) {}

  Expression operator-(const Expression &other) const;
  Expression operator+(const Expression &other) const;
  Expression operator*(const Expression &other) const;
  Expression zeroExtend(unsigned bits) const;
  Expression signExtend(unsigned bits) const;
  Expression ule(const Expression &other) const;
  Expression equals(const Expression &other) const;

  Expression(const Expression &other) = default;
  Expression &operator=(const Expression &other) = default;

  friend PrettyPrinter &operator<<(PrettyPrinter &out, const Expression &expr) {
    expr.prettyPrint(out);
    return out;
  }

  friend std::ostream &operator<<(std::ostream &out, const Expression &expr) {
    PrettyPrinter pp(false);
    expr.prettyPrint(pp);
    return out << pp.str();
  }

  unsigned bits() const override { return Instance_->bits(); }
  unsigned bits(const FunctionContext &fctx) const;

  z3::expr toFormula(const ValueMapping &vmap) const override {
    return Instance_->toFormula(vmap);
  }

  ConcreteState::Value eval(const ConcreteState &cstate) const override {
    return Instance_->eval(cstate);
  }

  void prettyPrint(PrettyPrinter &out) const override {
    Instance_->prettyPrint(out);
  }

  bool operator==(const ExpressionBase &other) const override;

  /**
   * Returns a `RepresentedValue` equal to this expression.
   *
   * If this is an atomic expression consisting of a single value, this method
   * returns that value. Otherwise, it panics.
   */
  RepresentedValue asRepresentedValue() const;

#ifdef ENABLE_DYNAMIC
  // handy shortcut since Expression has no default constructor
  template <class Archive> static inline Expression LoadFrom(Archive &archive) {
    Expression e(nullptr);
    archive(e);
    return e;
  }

  template <class Archive> void serialize(Archive &archive) {
    archive(Instance_);
  }
#endif
};
} // namespace symabs_ai
