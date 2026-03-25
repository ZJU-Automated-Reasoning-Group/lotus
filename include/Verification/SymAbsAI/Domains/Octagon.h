/**
 * @file Octagon.h
 * @brief Octagon abstract domain for relational analysis.
 *
 * This header defines the Octagon class, which implements the Octagon
 * abstract domain for relational analysis between two LLVM scalar values.
 * Octagon is more expressive than Zone as it tracks constraints of the
 * form: ±x ± y ≤ c
 *
 * Key concepts:
 * - Tracks four octagonal constraints: x-y, -x+y, x+y, -x-y
 * - More precise than Zone (which only tracks x-y)
 * - Can express absolute value constraints via x+y and x-y bounds
 *
 * Mathematical representation:
 *   c[0]:  +x - y  ≤  c[0]
 *   c[1]:  -x + y  ≤  c[1]
 *   c[2]:  +x + y  ≤  c[2]
 *   c[3]:  -x - y  ≤  c[3]
 *
 * Common use cases:
 * - Analyzing expressions involving absolute values
 * - Relational analysis with additive combinations
 * - More precise array index bounds analysis
 *
 * @see Zones
 * @see Affine
 * @see Intervals
 */
#pragma once

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <limits>

namespace symabs_ai {
namespace domains {

/**
 * Octagon domain for a pair of LLVM scalar values.
 * Tracks constraints of the form:  ±x ± y ≤ c
 *
 * Four directions:
 *   c[0]:  +x - y  ≤  c[0]
 *   c[1]:  -x + y  ≤  c[1]
 *   c[2]:  +x + y  ≤  c[2]
 *   c[3]:  -x - y  ≤  c[3]
 *
 * States:
 *   TOP:      Top_ == true
 *   BOTTOM:   Bottom_ == true  OR  inconsistent bounds
 *   VALUE:    !Top_ && !Bottom_ with finite bounds
 */
class Octagon : public AbstractValue {
  const FunctionContext &Fctx_;
  RepresentedValue X_;
  RepresentedValue Y_;

  bool Top_ = true;
  bool Bottom_ = true;

  // Bounds for the four octagonal directions
  // c[0]: x-y, c[1]: -x+y, c[2]: x+y, c[3]: -x-y
  int64_t C_[4];

  static constexpr int64_t INF = std::numeric_limits<int64_t>::max();

  void initializeToTop() {
    for (int i = 0; i < 4; ++i)
      C_[i] = INF;
  }

  bool isInconsistent() const;
  void checkConsistency();

public:
  Octagon(const FunctionContext &fctx, RepresentedValue x, RepresentedValue y)
      : Fctx_(fctx), X_(x), Y_(y) {
    initializeToTop();
  }

  // AbstractValue interface
  bool joinWith(const AbstractValue &other) override;
  bool meetWith(const AbstractValue &other) override;
  bool updateWith(const ConcreteState &cstate) override;
  z3::expr toFormula(const ValueMapping &vmap, z3::context &ctx) const override;

  void havoc() override {
    Top_ = true;
    Bottom_ = false;
    initializeToTop();
  }

  void resetToBottom() override {
    Top_ = false;
    Bottom_ = true;
  }

  bool isTop() const override { return Top_ && !Bottom_; }
  bool isBottom() const override { return Bottom_; }

  AbstractValue *clone() const override { return new Octagon(*this); }
  bool isJoinableWith(const AbstractValue &other) const override;

  void prettyPrint(PrettyPrinter &out) const override;
};

} // namespace domains
} // namespace symabs_ai
