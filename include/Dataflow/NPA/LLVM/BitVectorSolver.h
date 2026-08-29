#ifndef NPA_BIT_VECTOR_SOLVER_H
#define NPA_BIT_VECTOR_SOLVER_H

#include "Dataflow/NPA/LLVM/BitVectorProblem.h"
#include "Dataflow/NPA/Domains/BitSetDomain.h"
#include "Dataflow/NPA/NPA.h"

#include <unordered_map>

namespace npa {

enum class SolverStrategy { Kleene, Newton };

/// Linear solver strategy for Newton iteration (TOPLAS 2016: LCFL / tensor
/// product).
using LinearStrategy = npa::LinearStrategy;

/**
 * @brief Generic solver for bit-vector dataflow problems using NPA
 */
class BitVectorSolver {
public:
  using ResultMap = std::unordered_map<const llvm::BasicBlock *, llvm::APInt>;

  struct Result {
    ResultMap IN;
    ResultMap OUT;
    Stat stats;
  };

  /**
   * @brief Run the analysis on a function
   */
  static Result run(llvm::Function &F, const BitVectorProblem &info,
                    SolverStrategy strategy = SolverStrategy::Newton,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    bool verbose = false);
};

} // namespace npa

#endif // NPA_BIT_VECTOR_SOLVER_H
