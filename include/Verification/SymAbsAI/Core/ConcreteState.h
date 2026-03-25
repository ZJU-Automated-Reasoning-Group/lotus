/**
 * @file ConcreteState.h
 * @brief Representation of concrete program states from Z3 models.
 *
 * This header defines classes for representing concrete values and complete
 * program states derived from Z3 solver models. These are used during abstract
 * interpretation to concretize abstract values for SMT queries and to update
 * abstract domains with concrete information from counterexamples.
 *
 * Key components:
 * - `ConcreteState::Value`: An optimized representation of concrete values that
 * can store either Z3 expressions or direct 64-bit integer representations.
 * - `ConcreteState`: A complete mapping from all represented LLVM values to
 * their concrete values as determined by a Z3 model.
 *
 * The Value class uses a tagged union representation for efficiency:
 * - For bitvectors <= 64 bits: stores direct integer value
 * - For larger values or expressions: stores Z3 expression
 * - Supports implicit conversion to/from z3::expr, uint64_t, and int64_t
 *
 * @see FunctionContext
 * @see ValueMapping
 */
#pragma once

#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/RepresentedValue.h"
#include "Verification/SymAbsAI/Core/ResultStore.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"

#include <iostream>

#include <z3++.h>

namespace symabs_ai {
/**
 * @brief Container for concrete program states derived from Z3 models.
 *
 * ConcreteState maps each RepresentedValue to its concrete value as determined
 * by a Z3 model. It provides array-like access to values and is used to:
 * - Convert abstract values to concrete for SMT queries
 * - Extract concrete information from counterexamples
 * - Update abstract domains with concrete observations
 *
 * The state can be constructed from:
 * - A Z3 model and ValueMapping (typical use case)
 * - An existing array of Value objects (for efficiency)
 */
class ConcreteState {
public:
  /**
   * Represents a concrete value.
   *
   * Can be implicitly converted to and from a constant `z3::expr`. Provides
   * additional conversion operators to `uint64_t` and `int64_t` that can
   * be used if represented value is a bitvector no bigger than 64 bits.
   *
   * Can exist in an "uninitialized" state. Such instance holds no value and
   * should not be used in any way apart from calling `operator=` to
   * initialize it. This allows `ConcreteState` to keep a sparse array of
   * values.
   */
  class Value {
  private:
    /*
     * The representation of a concrete value has been somewhat optimized
     * since this class is used in dynamic analysis. It can hold a
     * (constant) Z3 expression and/or a direct binary representation if
     * it's a bitvector no bigger than 64 bits. Following invariants always
     * hold:
     *
     * 1. If hasExpr(), then Ptr_.expr points to a constant Z3 expression
     *    representing this concrete value.
     * 2. If hasValue(), then this object represent a concrete bitvector
     *    and the lowest valueBits() of Value_ hold its value. Remaining
     *    bits of Value_ must be zero. Ptr_.ctx in this case points to a
     *    valid Z3 context.
     *
     * Upper bit of Tag_ encodes whether hasExpr() is true, remaining bits
     * encode the bit width of Value_ or are equal to zero if value is
     * unavailable.
     */

    mutable uint8_t Tag_;
    mutable union {
      z3::expr *expr;
      z3::context *ctx;
    } Ptr_;
    mutable uint64_t Value_;

    inline bool hasExpr() const { return Tag_ & 0x80; }
    inline bool hasValue() const { return (Tag_ & 0x7f) != 0; }
    inline unsigned valueBits() const { return Tag_ & 0x7f; }

#ifdef NDEBUG
    inline void checkValid() const {}
#else
    inline void checkValid() const {
      if (hasValue()) {
        assert(valueBits() <= 64);

        if (valueBits() < 64) {
          uint64_t one = 1;
          uint64_t mask = (one << valueBits()) - one;
          assert((Value_ & ~mask) == 0);
        }
      }

      assert(hasExpr() || hasValue());
    }
#endif

  public:
    /**
     * Constructs a value in an "uninitialized" state.
     */
    Value() : Tag_(0) {}

    /**
     * Constructs a concrete value based on a constant Z3 expression.
     */
    Value(const z3::expr &e) {
      Tag_ = 0x80;
      Ptr_.expr = new z3::expr(e);
      assert(hasExpr() && !hasValue());
      checkValid();
    }

    /**
     * Constructs a concrete bitvector value based on the `bits` lowest
     * bits of `value`. Remaining bits should be zero.
     */
    Value(z3::context *ctx, uint64_t value, int bits) {
      assert(bits > 0 && bits <= 64);
      Tag_ = (uint8_t)bits;
      Ptr_.ctx = ctx;
      Value_ = value;
      assert(hasValue() && !hasExpr() && (int)valueBits() == bits);
      checkValid();
    }

    /**
     * Constructs a concrete bitvector value based on the `bits` lowest
     * bits of `value`. Remaining bits should be zero.
     */
    Value(const FunctionContext &fctx, uint64_t value, int bits)
        : Value(&fctx.getZ3(), value, bits) {}

    bool empty() const { return Tag_ == 0; }

    unsigned bits() const {
      if (hasValue())
        return valueBits();

      z3::expr e = *this;
      assert(e.get_sort().is_bv());
      return e.get_sort().bv_size();
    }

    z3::context &getZ3() {
      if (hasExpr())
        return Ptr_.expr->ctx();
      else
        return *Ptr_.ctx;
    }

    operator const z3::expr &() const {
      if (!hasExpr()) {
        assert(hasValue());
        Tag_ |= 0x80;
        Ptr_.expr =
            new z3::expr(Ptr_.ctx->bv_val((uint64_t)Value_, valueBits()));
        checkValid();
      }

      assert(hasExpr());
      return *Ptr_.expr;
    }

    operator uint64_t() const {
      if (!hasValue()) {
        z3::expr *e = Ptr_.expr;
        assert(hasExpr());
        unsigned bits;

        if (e->is_bool()) {
          bool result = Z3_get_bool_value(e->ctx(), *e) == Z3_L_TRUE;
          Value_ = result ? 1 : 0;
          bits = 1;
        } else {
          assert(e->is_bv());
          uint64_t result;
          bool succ = Z3_get_numeral_uint64(e->ctx(), *e, &result);
          assert(succ);
          Value_ = result;
          bits = e->get_sort().bv_size();
        }

        Tag_ |= bits & 0x7f;
      }

      assert(hasValue());
      checkValid();
      return Value_;
    }

    operator int64_t() const {
      uint64_t value = (uint64_t)(*this);
      unsigned bw = valueBits();
      uint64_t one = 1;
      if (bw < 64 && (value & (one << (bw - 1)))) {
        // sign bit set, we need to sign-extend
        value |= ~((one << bw) - one);
      }
      return (int64_t)value;
    }

    Value &operator=(const Value &other) {
      if (hasExpr())
        delete Ptr_.expr;

      Tag_ = other.Tag_;
      Value_ = other.Value_;
      if (other.hasExpr())
        Ptr_.expr = new z3::expr(*other.Ptr_.expr);
      else
        Ptr_.ctx = other.Ptr_.ctx;

      return *this;
    }

    Value &operator=(Value &&other) {
      if (hasExpr())
        delete Ptr_.expr;

      Tag_ = other.Tag_;
      Value_ = other.Value_;
      Ptr_ = other.Ptr_;
      other.Tag_ = 0;
      assert(!other.hasExpr());
      return *this;
    }

    Value(const Value &other) {
      Tag_ = 0;
      *this = other;
    }

    Value(Value &&other) {
      Tag_ = 0;
      *this = std::move(other);
    }

    friend std::ostream &operator<<(std::ostream &out, const Value &val);

    ~Value() {
      if (hasExpr())
        delete Ptr_.expr;
    }

#ifdef ENABLE_DYNAMIC
    template <class Archive> void save(Archive &archive) const {
      // conversion needs to happen before the call to valueBits()
      auto x = static_cast<uint64_t>(*this);
      archive(valueBits(), x);
    }

    template <class Archive> void load(Archive &archive) {
      int bits;
      uint64_t value;
      auto &fctx = cereal::get_user_data<FunctionContext>(archive);
      archive(bits, value);
      *this = Value(fctx, value, bits);
    }
#endif
  };

private:
  const FunctionContext &FunctionContext_;
  std::unique_ptr<const ValueMapping> VMap_;
  std::unique_ptr<const z3::model> Model_;
  Value *Values_;
  std::vector<Value> ManagedValues_;

public:
  /**
   * Creates a concrete state based on a Z3 model.
   */
  ConcreteState(const ValueMapping &vmap, z3::model model);

  /**
   * Creates a concrete state that wraps an existing array of values.
   *
   * The pointer `values` must point to an array of
   * `fctx.representedValues().size()` elements. The callee is responsible
   * for ensuring that the array will outlive this `ConcreteState` and is
   * freed later.
   */
  ConcreteState(const FunctionContext &fctx, Value *values)
      : FunctionContext_(fctx), Values_(values) {}

  ConcreteState(const ConcreteState &other)
      : FunctionContext_(other.FunctionContext_), Values_(other.Values_),
        ManagedValues_(other.ManagedValues_) {
    if (other.Model_ != nullptr)
      Model_.reset(new z3::model(*other.Model_));

    if (other.VMap_ != nullptr)
      VMap_.reset(new ValueMapping(*other.VMap_));
  }

  const z3::model *getModel() const { return Model_.get(); }

  const ValueMapping &getValueMapping() const {
    assert(VMap_ != nullptr);
    return *VMap_;
  }

  const Value &operator[](const RepresentedValue &rvalue) const {
    return Values_[rvalue.id()];
  }

  const Value &operator[](const llvm::Value *value) const {
    auto *rv = FunctionContext_.findRepresentedValue(value);
    assert(rv != nullptr);
    return Values_[rv->id()];
  }

  friend std::ostream &operator<<(std::ostream &out,
                                  const ConcreteState &cstate);
};

} // namespace symabs_ai
