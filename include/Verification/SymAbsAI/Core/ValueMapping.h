/**
 * @file ValueMapping.h
 * @brief Mapping between LLVM values and their SMT variable representations.
 *
 * This header defines the ValueMapping class that provides context-sensitive
 * mappings from LLVM IR values to their corresponding Z3 expressions. The
 * mapping is location-aware: it considers the position within a Fragment
 * to return the correct SMT variable (pre-definition or post-definition).
 *
 * Key use cases:
 * - Generating SMT formulas for program semantics
 * - Mapping concrete values back to abstract domains
 * - Handling SSA phi-nodes correctly (using different variables before/after
 * definitions)
 *
 * @see Fragment
 * @see FunctionContext
 */
#pragma once

#include "Verification/SymAbsAI/Core/Fragment.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>
#include <z3++.h>

namespace symabs_ai {
/**
 * @brief Provides a mapping between LLVM Values and SMT variables.
 *
 * This mapping depends on precise location inside a particular fragment. In
 * particular, if fragment includes definition of a value, a different
 * ("pre") variable has to be used before the instruction defining it.
 *
 * The mapping provides two main query methods:
 * - `operator[]`: Returns bit-vector representation (for non-pointers)
 * - `getFullRepresentation()`: Returns appropriately-typed expression (for
 * pointers)
 */
class ValueMapping {
private:
  const FunctionContext &FunctionContext_;
  const Fragment &Fragment_;

  // This ValueMapping corresponds to a mapping just before this instruction.
  // Should be null if AtBeginning_ || AtEnd_.
  llvm::Instruction *Point_;

  // mutually exclusive
  bool AtBeginning_;
  bool AtEnd_;

  bool IncludesEndBody_;

  ValueMapping(const FunctionContext &fctx, const Fragment &frag,
               llvm::Instruction *instr)
      : FunctionContext_(fctx), Fragment_(frag), Point_(instr),
        AtBeginning_(false), AtEnd_(false),
        IncludesEndBody_(frag.includesEndBody()) {}

public:
  /**
   * Returns an SMT expression corresponding to the actual value of the
   * given LLVM value at the program point corresponding to this ValueMapping
   * instance. If it has pointer type, an expression representing the actual
   * BitVector value of the pointer is returned.
   */
  z3::expr operator[](llvm::Value *value) const;

  /**
   * Returns an SMT expression corresponding to the given LLVM value at the
   * program point corresponding to this ValueMapping instance. Especially,
   * for values with pointer type, an expression with the pointer sort that is
   * specified in the MemoryModel is returned.
   */
  z3::expr getFullRepresentation(llvm::Value *value) const;

  /**
   * Returns an expression corresponding to the memory array at this program
   * point.
   */
  z3::expr memory() const;

  const FunctionContext &fctx() const { return FunctionContext_; }

  /**
   * Constructs a ValueMapping for a BasicBlock.
   */
  static ValueMapping atLocation(const FunctionContext &fctx,
                                 const Fragment &frag, llvm::BasicBlock *bb);

  /**
   * Constructs a ValueMapping corresponding to a location at the beginning
   * of a fragment.
   */
  static ValueMapping atBeginning(const FunctionContext &fctx,
                                  const Fragment &frag);

  /**
   * Constructs a ValueMapping corresponding to a location after the execution
   * of the fragment.
   */
  static ValueMapping atEnd(const FunctionContext &fctx, const Fragment &frag);

  /**
   * Constructs a ValueMapping corresponding to a location before a given
   * instruction.
   *
   * The instruction `inst' must be defined inside the fragment `frag'.
   */
  static ValueMapping before(const FunctionContext &fctx, const Fragment &frag,
                             llvm::Instruction *inst);

  /**
   * Constructs a ValueMapping corresponding to a location after a given
   * instruction.
   *
   * This instruction `inst' must be defined inside the fragment `frag' and
   * must *not* be a terminator instruction as such program point would not
   * be properly defined.
   */
  static ValueMapping after(const FunctionContext &fctx, const Fragment &frag,
                            llvm::Instruction *inst);
};
} // namespace symabs_ai
