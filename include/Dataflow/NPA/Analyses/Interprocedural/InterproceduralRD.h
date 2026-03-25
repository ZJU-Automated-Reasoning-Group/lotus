#ifndef NPA_INTERPROC_RD_H
#define NPA_INTERPROC_RD_H

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"
#include "Dataflow/NPA/Domains/GenKillDomain.h"

#include <map>

#include <llvm/IR/Module.h>

namespace npa {

class InterproceduralRD {
public:
  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, GenKillDomain::value_type> summaries;
    std::map<BlockKey, llvm::APInt> blockFacts;
  };

  static Result run(llvm::Module &M, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible);
};

} // namespace npa
#endif
