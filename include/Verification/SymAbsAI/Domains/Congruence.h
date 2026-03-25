/**
 * @file Congruence.h
 * @brief Congruence abstract domain for modular arithmetic.
 *
 * This header defines the Congruence class, which implements a congruence
 * abstract domain. It tracks values of the form x ≡ r (mod m) where
 * values are congruent to a remainder modulo a modulus.
 *
 * Representation:
 *   Modulus_ == 0   => Singleton value {Remainder_}
 *   Modulus_ > 0    => Set {Remainder_ + k*Modulus_} for all integers k
 *   Top_ == true    => Unrestricted (any value possible)
 *
 * Common use cases:
 * - Analyzing expressions involving modulo operations
 * - Loop induction variable analysis
 * - Detecting divisibility properties
 *
 * More precise than intervals for tracking periodic/repeating patterns.
 *
 * @see Intervals
 * @see Affine
 */
#pragma once

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <algorithm>

namespace symabs_ai {
namespace domains {

/**
 * Congruence domain for a single LLVM value: x ≡ r (mod m).
 *
 * Representation: Modulus_ == 0   => singleton value {Remainder_}
 *                Modulus_ > 0    => set {Remainder_ + k*Modulus_}
 *                Top_ == true    => unrestricted
 */
class Congruence : public AbstractValue {
  const FunctionContext &Fctx_;
  RepresentedValue Value_;

  uint64_t Modulus_ = 1;   // m > 0
  uint64_t Remainder_ = 0; // 0 <= r < m

  bool Top_ = true;
  bool Bottom_ = true;

  static uint64_t gcd_u64(uint64_t a, uint64_t b) {
    while (b) {
      uint64_t t = b;
      b = a % b;
      a = t;
    }
    return a;
  }
  bool isValid() const { return (Top_ || Bottom_) || Modulus_ > 0; }

public:
  Congruence(const FunctionContext &fctx, RepresentedValue val)
      : Fctx_(fctx), Value_(val) {}

  /* lattice operations */
  bool joinWith(const AbstractValue &av_other) override;
  bool meetWith(const AbstractValue &av_other) override;
  bool updateWith(const ConcreteState &cstate) override;

  z3::expr toFormula(const ValueMapping &vmap, z3::context &ctx) const override;

  void havoc() override {
    Top_ = true;
    Bottom_ = false;
  }

  bool isTop() const override { return Top_ && !Bottom_; }
  bool isBottom() const override { return Bottom_; }

  void resetToBottom() override {
    Top_ = false;
    Bottom_ = true;
  }

  AbstractValue *clone() const override { return new Congruence(*this); }
  bool isJoinableWith(const AbstractValue &other) const override;

  uint64_t modulus() const { return Modulus_; }
  uint64_t remainder() const { return Remainder_; }

  void prettyPrint(PrettyPrinter &out) const override;
};

} // namespace domains
} // namespace symabs_ai
