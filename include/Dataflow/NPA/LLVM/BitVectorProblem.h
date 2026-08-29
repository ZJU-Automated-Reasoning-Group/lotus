#ifndef NPA_BIT_VECTOR_PROBLEM_H
#define NPA_BIT_VECTOR_PROBLEM_H

#include "Utils/LLVM/SystemHeaders.h"

#include <llvm/ADT/APInt.h>

namespace npa {

/**
 * @brief Abstract interface for defining a bit-vector dataflow problem.
 */
struct BitVectorProblem {
  virtual ~BitVectorProblem() = default;

  /// Returns the number of bits in the vector (domain width)
  virtual unsigned getBitWidth() const = 0;

  /// Returns the GEN set for a basic block
  virtual llvm::APInt getGen(const llvm::BasicBlock *BB) const = 0;

  /// Returns the KILL set for a basic block
  virtual llvm::APInt getKill(const llvm::BasicBlock *BB) const = 0;

  /// Returns true if the analysis is forward, false if backward
  virtual bool isForward() const = 0;

  /// Returns the boundary value (e.g., for Entry in forward analysis)
  virtual llvm::APInt getBoundaryVal() const {
    // Default to Empty Set (All Zeros)
    return llvm::APInt(getBitWidth(), 0);
  }
};

} // namespace npa

#endif // NPA_BIT_VECTOR_PROBLEM_H
