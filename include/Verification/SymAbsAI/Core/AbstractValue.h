/**
 * @file AbstractValue.h
 * @brief Pure virtual interface for defining abstract values.
 */

#pragma once

#include "Verification/SymAbsAI/Core/ConcreteState.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"
#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Value.h>
#include <z3++.h>

namespace symabs_ai {

/**
 * Represents an abstract value.
 *
 * By convention, newly created instances (either using a constructor of a
 * particular subclass or some factory functions or methods) should represent
 * bottom. Use havoc() if you want to represent unspecified values.
 */
class AbstractValue {
public:
  /**
   * Pretty-print the abstract value.
   *
   * Simple values should be printed without any line breaks. More complex
   * ones include line breaks but, for consistency, should not end the last
   * line.
   */
  virtual void prettyPrint(PrettyPrinter &out) const = 0;

  /**
   * Perform an in-place join.
   *
   * Joins this abstract value with the argument. The implementation is
   * encouraged to assert() that other is compatible with this object (has
   * the same dynamic type, parameters). The return value indicates whether
   * the join had any effect, i.e. the value represented by this object is
   * different than it was before the operation.
   *
   * Should form a proper lattice together with operator==() and operator<=()
   * so, in particular, if `a <= b` then `b.joinWith(a)` should return false.
   * Note that default implementations of operator==() and operator<=() call
   * this method so you cannot use these operators in your implementation.
   *
   * \param other the abstract value to join with
   * \returns true iff the operation changed the object
   */
  virtual bool joinWith(const AbstractValue &other) = 0;

  /**
   * Perform an in-place meet.
   *
   * Meets this abstract value with the argument. The implementation is
   * encouraged to assert() that other is compatible with this object (has
   * the same dynamic type, parameters). The return value indicates whether
   * the meet had any effect, i.e. the value represented by this object is
   * different than it was before the operation.
   *
   * Should form a proper lattice together with operator==() and operator<=()
   * so, in particular, if `a <= b` then `a.meetWith(b)` should return false.
   *
   * \param other the abstract value to meet with
   * \returns true iff the operation changed the object
   */
  virtual bool meetWith(const AbstractValue &other) = 0;

  /**
   * Perform an in-place join of the object with the AbstractValue belonging
   * to the concrete value that is represented by the specified SMT model.
   *
   * Using the notation present in literature, this corresponds to updating
   * \f$x\f$ to \f$x \sqcup \beta(m)\f$.
   *
   * \param model the model to interpret and join with the object.
   * \returns true iff the join changed the object
   */
  virtual bool updateWith(const ConcreteState &cstate) = 0;

  /**
   * Generates an SMT formula that captures the constraints that are given by
   * the semantics of the abstraction.
   *
   * Corresponds to \f$\hat\gamma\f$ in the literature.
   */
  virtual z3::expr toFormula(const ValueMapping &, z3::context &) const = 0;

  /**
   * Sets the abstract value to top.
   *
   * Causes this abstract value to forget everything. After calling this
   * method, this->isTop() must be true.
   */
  virtual void havoc() = 0;

  /**
   * Reset every result information such that after a call to this function,
   * isBottom returns true.
   */
  virtual void resetToBottom() = 0;

  /**
   * Does the object represents \f$\top\f$?
   */
  virtual bool isTop() const = 0;

  /**
   * Does the object represent \f$\bot\f$?
   */
  virtual bool isBottom() const = 0;

  virtual void widen() {}

  /**
   * Makes an exact copy.
   *
   * Should allocate and return an AbstractValue that is equal to this
   * instance. The created object should be joinable with this one (even
   * after modifications).
   *
   * The caller is responsible for freeing the created object.
   */
  virtual AbstractValue *clone() const = 0;

  /**
   * Determines whether an abstract value is compatible with another value.
   *
   * This operation must be symmetric, i.e. a->isJoinableWith(b) iff
   * b->isJoinableWith(a) and a->join(b) must succeed if a->isJoinableWith(b).
   */
  virtual bool isJoinableWith(const AbstractValue &other) const = 0;

  /**
   * Collects pointers to all subcomponents in a vector.
   *
   * This method can be used to provide a flattened "view" of the underlying
   * hierarchy of abstract values. The default implementation for
   * non-compound values will just add the `this` pointer. Compound abstract
   * values like domains::Product will recursively call this method on their
   * elements.
   *
   * The pointers added to the vector `result` are owned by this abstract
   * value and should not be deallocated or allowed to outlive its lifetime.
   */
  virtual void gatherFlattenedSubcomponents(
      std::vector<const AbstractValue *> *result) const {
    result->push_back(this);
  }

  /**
   * Performs an inplace abstract consequnce operation.
   *
   * With initial value `a == c` and `c < b`, the following should hold after
   * the call `a.abstractConsequence(b)`:
   *      `c <= a` and `! b <= a`
   *
   * Doing nothing is a valid abstract consequence operation.
   */
  virtual void abstractConsequence(const AbstractValue &other) {
    assert(isJoinableWith(other));
  }

  /**
   * Determines the lattice ordering of two abstract values.
   *
   * Should make a proper partial order together with operator<=() and join.
   * Smaller value is more precise (that is, describes a subset of concrete
   * states).
   */
  virtual bool operator<=(const AbstractValue &other) const {
    auto copy = unique_ptr<AbstractValue>(other.clone());
    return !copy->joinWith(*this);
  }

  /**
   * Checks two abstract values for equality.
   *
   * Should make a proper partial order together with operator<=().
   */
  virtual bool operator==(const AbstractValue &other) const {
    return (*this) <= other && other <= (*this);
  }

  friend std::ostream &operator<<(std::ostream &out, const AbstractValue &av) {
    PrettyPrinter pp(false);
    av.prettyPrint(pp);
    return out << pp.str();
  }

  virtual ~AbstractValue() {}
};
} // namespace symabs_ai
