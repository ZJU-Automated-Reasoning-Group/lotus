#ifndef NPA_INTERPROC_RD_H
#define NPA_INTERPROC_RD_H

#include "Dataflow/NPA/Domains/GenKillDomain.h"
#include "Dataflow/NPA/LLVM/AnalysisSupport.h"

#include <map>

#include <llvm/IR/Module.h>

namespace npa {

class InterReachingDefinitions {
public:
  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, GenKillTransformer::value_type> summaries;
    std::map<BlockKey, llvm::APInt> blockFacts;
  };

  static Result run(llvm::Module &M, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible);
};

} // namespace npa
#endif
