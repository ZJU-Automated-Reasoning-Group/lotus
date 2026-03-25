/**
 * @file Predicates.h
 * @brief Predicate abstract domain for SMT formula analysis.
 *
 * This header defines the Predicates class, which implements a predicate
 * abstract domain. For a given predicate `p`, it tracks whether `p` is
 * always true, always false, may be either, or never occurs.
 *
 * Lattice structure (Hasse diagram):
 *          TOP
 *        /    \
 *     TRUE   FALSE
 *        \    /
 *        BOTTOM
 *
 * States:
 *   TRUE  - Predicate is true in all concrete executions
 *   FALSE - Predicate is false in all concrete executions
 *   TOP   - Predicate may be true or false (unknown)
 *   BOTTOM - No concrete executions satisfy the predicate (infeasible)
 *
 * Use cases:
 * - Analyzing path conditions
 * - Testing reachability properties
 * - Redundant computation elimination (via equality predicates)
 *
 * PredicatesWrapper provides template-based integration with parameterization
 * strategies to automatically construct predicate expressions.
 *
 * @see Expression
 * @see DomainConstructor
 */
#pragma once

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/ConcreteState.h"
#include "Verification/SymAbsAI/Core/Expression.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Domains/Product.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <memory>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Value.h>

namespace symabs_ai {
class FunctionContext;

namespace domains {
using std::unique_ptr;

/**
 * @brief Predicate abstract domain for a boolean expression.
 *
 * For a given predicate `p`, this domain tracks its state in the
 * following Hasse diagram:
 *
 *          TOP
 *        /    \
 *     TRUE   FALSE
 *        \    /
 *        BOTTOM
 *
 * Here, `TRUE` means that `p` is true in every program run, `FALSE`
 * that `p` is false in every program run, `TOP` that both cases
 * might occur and `BOTTOM` that none of them occur.
 */
class Predicates : public AbstractValue {
public:
  typedef std::function<Expression(Expression, Expression)> pred_t;
  enum Value { BOTTOM, TRUE, FALSE, TOP };

private:
  const FunctionContext &FunctionContext_;
  Expression Predicate_;
  Value Val_ = BOTTOM;

public:
  Predicates(const FunctionContext &fctx, const Expression &predicate)
      : FunctionContext_(fctx), Predicate_(predicate) {}

  virtual bool joinWith(const AbstractValue &av_other) override;

  virtual bool meetWith(const AbstractValue &av_other) override;

  virtual bool updateWith(const ConcreteState &cstate) override;

  virtual z3::expr toFormula(const ValueMapping &vmap,
                             z3::context &zctx) const override;

  virtual void havoc() override { Val_ = TOP; }

  virtual AbstractValue *clone() const override {
    return new Predicates(*this);
  }

  virtual bool isTop() const override { return Val_ == TOP; }
  virtual bool isBottom() const override { return Val_ == BOTTOM; }

  virtual void prettyPrint(PrettyPrinter &out) const override;

  virtual void resetToBottom() override { Val_ = BOTTOM; }

  virtual bool isJoinableWith(const AbstractValue &other) const override;

  Value getValue() const { return Val_; }

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    archive(Predicate_);
    archive(Val_);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void load_and_construct(Archive &archive,
                                 cereal::construct<Predicates> &construct) {
    auto &fctx = cereal::get_user_data<FunctionContext>(archive);
    auto predicate = Expression::LoadFrom(archive);
    construct(fctx, predicate);
    archive(construct->Val_);
  }
#endif
};

/**
 *  A wrapper for `Predicates` to make it compatible with parameterization
 *  strategies.
 *  It uses the PRED function to construct an Expression containing its two
 *  arguments and uses it for a Predicates domain.
 */
template <Predicates::pred_t *PRED>
class PredicatesWrapper : public Predicates {
public:
  PredicatesWrapper(const FunctionContext &fctx, Expression left,
                    Expression right)
      : Predicates(fctx, (*PRED)(left, right)) {}

  virtual ~PredicatesWrapper() {}
};

} // namespace domains
} // namespace symabs_ai

#ifdef ENABLE_DYNAMIC
CEREAL_REGISTER_TYPE(symabs_ai::domains::Predicates);
#endif
