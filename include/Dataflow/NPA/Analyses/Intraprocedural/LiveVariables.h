#ifndef NPA_LIVE_VARIABLES_H
#define NPA_LIVE_VARIABLES_H

#include "Dataflow/NPA/Analyses/BitVectorSolver.h"

#include <llvm/IR/Function.h>

namespace npa {

class LiveVariables {
public:
  static BitVectorSolver::Result
  run(llvm::Function &F, SolverStrategy strategy = SolverStrategy::Newton);
};

} // namespace npa

#endif // NPA_LIVE_VARIABLES_H
