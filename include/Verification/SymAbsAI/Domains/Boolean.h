/**
 * @file Boolean.h
 * @brief Base class for boolean abstract domains.
 *
 * This header defines BooleanValue class, which is an abstract base class
 * for domains that describe truthiness/falsiness of some invariant.
 * It provides a four-valued lattice structure for tracking boolean
 * predicates.
 *
 * Lattice structure (Hasse diagram):
 *           TOP
 *         /    \
 *      TRUE   FALSE
 *         \    /
 *         BOTTOM
 *
 * Subclasses must implement `makePredicate()` which generates the Z3
 * formula whose truthiness the domain describes.
 *
 * Common use cases:
 * - Analyzing boolean expressions
 * - Tracking path conditions
 * - Redundant computation detection
 *
 * @see Predicates
 * @see NumRels
 */
#pragma once

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Domains/Combinators.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

namespace symabs_ai {
namespace domains {
/**
 * Abstract base class for domains that describe truthiness/falsiness of some
 * invariant. It can have 4 different states, ordered as follows:
 *
 *           TOP
 *         /    \
 *      TRUE   FALSE
 *         \    /
 *         BOTTOM
 *
 * For inheriting, following methods have to be implemented:
 *  - mkPredicate
 *  - prettyPrint
 *  - clone
 *  - isJoinableWith
 *  - serialization
 */
class BooleanValue : public AbstractValue {
public:
  enum Value { TOP, BOTTOM, TRUE, FALSE };

private:
  const FunctionContext &Fctx_;

protected:
  Value Val_ = BOTTOM;

  /** Constructs the formula whose truthiness should be described.
   */
  virtual z3::expr makePredicate(const ValueMapping &vmap) const = 0;

public:
  Value getValue() const { return Val_; }

  BooleanValue(const FunctionContext &fctx) : Fctx_(fctx) {}

  virtual bool joinWith(const AbstractValue &av_other) override;

  virtual bool meetWith(const AbstractValue &av_other) override;

  virtual z3::expr toFormula(const ValueMapping &,
                             z3::context &) const override;

  virtual bool updateWith(const ConcreteState &cstate) override;

  virtual void havoc() override { Val_ = TOP; }

  virtual bool isTop() const override { return Val_ == TOP; }
  virtual bool isBottom() const override { return Val_ == BOTTOM; }
  virtual void resetToBottom() override { Val_ = BOTTOM; }
};
} // namespace domains
} // namespace symabs_ai
