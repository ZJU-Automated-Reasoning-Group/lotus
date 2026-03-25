/**
 * @file RepresentedValue.h
 * @brief Wrapper for LLVM values with unique numeric identifiers.
 *
 * This header defines RepresentedValue class, which is a "fat pointer" to an
 * llvm::Value that provides a unique numeric identifier. The ID serves as a
 * perfect hash function for efficient array-based mappings.
 *
 * Key features:
 * - Behaves like an ordinary llvm::Value* pointer
 * - Provides unique numeric ID via `id()`
 * - Supports serialization via Cereal (unlike raw llvm::Value*)
 * - Used extensively by abstract domains for value representation
 *
 * The numeric ID enables O(1) array indexing for value mappings,
 * avoiding the need for hash tables. ConcreteState uses this pattern.
 *
 * @see FunctionContext
 * @see ConcreteState
 */
#pragma once

#include "Verification/SymAbsAI/Core/ResultStore.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <llvm/IR/Function.h>

namespace symabs_ai {
class FunctionContext;

/**
 * @brief A fat pointer to an `llvm::Value` (possibly null).
 *
 * An instance of this class can be used just like an ordinary pointer
 * `llvm::Value*` but additionally stores a numerical identifier that can be
 * retrieved using `id()`. The identifier is guaranteed to be unique within
 * a given function and smaller than
 * `FunctionContext::representedValues().size()`.
 * This can be used to implement a mapping from represented values as a simple
 * array (`id()` behaves like a perfect hash function). See `ConcreteState` for
 * an example.
 *
 * Abstract domains usually get a `RepresentedValue` as an argument to a
 * constructor (see parameterization strategies). To convert an ordinary
 * pointer to a `RepresentedValue` use
 * `FunctionContext::findRepresentedValue()`.
 *
 * An additional benefit is that this class supports serialization via Cereal
 * whereas `llvm::Value` by itself doesn't.
 */
class RepresentedValue {
  friend class FunctionContext;

private:
  unsigned Id_;
  llvm::Value *Value_;

  RepresentedValue(unsigned id, llvm::Value *value) : Id_(id), Value_(value) {
    assert(value != nullptr);
  }

public:
  /**
   * Creates a representation of a null pointer.
   */
  RepresentedValue() : Value_(nullptr) {}

  operator llvm::Value *() const { return Value_; }
  llvm::Value *operator->() const { return Value_; }

  /**
   * Returns the numerical id. `this` must not be null.
   */
  unsigned id() const {
    assert(Value_ != nullptr);
    return Id_;
  }

  RepresentedValue(const RepresentedValue &) = default;
  RepresentedValue &operator=(const RepresentedValue &) = default;

  friend bool operator<(const RepresentedValue &r1,
                        const RepresentedValue &r2) {
    return r1.Id_ < r2.Id_;
  };

  friend std::ostream &operator<<(std::ostream &out,
                                  const RepresentedValue &value);

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    ResultStore::ValueWrapper wrap(Value_);
    archive(wrap);
  }

  template <class Archive> void load(Archive &archive) {
    ResultStore::ValueWrapper wrap;
    archive(wrap);
    auto &fctx = cereal::get_user_data<FunctionContext>(archive);
    if (wrap != nullptr)
      *this = *fctx.findRepresentedValue(wrap);
  }
#endif
};
} // namespace symabs_ai
