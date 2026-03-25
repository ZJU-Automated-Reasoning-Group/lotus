#ifndef NPA_INTERPROC_MAYBE_UNINITIALIZED_H
#define NPA_INTERPROC_MAYBE_UNINITIALIZED_H

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"
#include "Dataflow/NPA/Domains/TaintTransferDomain.h"

#include <map>

#include <llvm/IR/Module.h>

namespace npa {

class InterproceduralMaybeUninitialized {
public:
  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, TaintTransferDomain::value_type> summaries;
    std::map<BlockKey, llvm::APInt> blockFacts;
  };

  static Result run(llvm::Module &M, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible);
};

} // namespace npa

#endif // NPA_INTERPROC_MAYBE_UNINITIALIZED_H
