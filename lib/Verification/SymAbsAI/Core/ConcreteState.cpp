/**
 * @file ConcreteState.cpp
 * @brief Implementation of ConcreteState, representing a concrete execution
 * state from a Z3 model.
 *
 * ConcreteState extracts concrete values for all represented LLVM values from a
 * Z3 model, providing a bridge between SMT solver results and abstract
 * interpretation. It evaluates each represented value in the model and stores
 * the results for use in updating abstract values.
 *
 * @author rainoftime
 */
#include "Verification/SymAbsAI/Core/ConcreteState.h"

#include "Verification/SymAbsAI/Core/MemoryModel.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"
#include "Verification/SymAbsAI/Core/repr.h"
// #include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

namespace symabs_ai {
/**
 * @brief Construct a ConcreteState from a Z3 model.
 *
 * Evaluates all represented values in the given model using the value mapping
 * and stores the results. The model must satisfy the constraints that were
 * used to generate it.
 *
 * @param vmap Value mapping that defines how LLVM values map to Z3 expressions
 * @param model The Z3 model containing concrete assignments for variables
 */
ConcreteState::ConcreteState(const ValueMapping &vmap, z3::model model)
    : FunctionContext_(vmap.fctx()), VMap_(new ValueMapping(vmap)),
      Model_(new z3::model(model)) {
  ManagedValues_.reserve(FunctionContext_.representedValues().size());
  for (llvm::Value *value : FunctionContext_.representedValues()) {
    z3::expr e = model.eval(vmap[value], true);
    ManagedValues_.push_back(e);
  }
  Values_ = &ManagedValues_[0];
}

/**
 * @brief Output stream operator for ConcreteState::Value.
 *
 * Prints a human-readable representation of a concrete value. If the value
 * is uninitialized (Tag_ == 0), prints "Value(?)", otherwise prints the
 * Z3 expression representation.
 *
 * @param out Output stream to write to
 * @param val The concrete value to print
 * @return Reference to the output stream
 */
std::ostream &operator<<(std::ostream &out, const ConcreteState::Value &val) {
  if (val.Tag_ == 0)
    return out << "Value(?)";
  else
    return out << repr((const z3::expr &)val);
}

/**
 * @brief Output stream operator for ConcreteState.
 *
 * Prints a human-readable representation of the entire concrete state as
 * a comma-separated mapping of represented values to their concrete Z3 values.
 * Format: "{value1: expr1, value2: expr2, ...}"
 *
 * @param out Output stream to write to
 * @param cstate The concrete state to print
 * @return Reference to the output stream
 */
std::ostream &operator<<(std::ostream &out, const ConcreteState &cstate) {
  auto &rvals = cstate.FunctionContext_.representedValues();
  bool needs_comma = false;

  out << "{";
  for (unsigned i = 0; i < rvals.size(); i++) {
    if (needs_comma)
      out << ", ";
    else
      needs_comma = true;

    return out << repr(rvals[i]) << ": " << repr(cstate.Values_[i]);
  }
  out << "}";

  return out;
}
} // namespace symabs_ai
