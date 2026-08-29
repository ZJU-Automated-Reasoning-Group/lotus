#ifndef NPA_INTERPROC_LIVE_VARIABLES_H
#define NPA_INTERPROC_LIVE_VARIABLES_H

#include "Dataflow/NPA/Domains/TaintDomain.h"
#include "Dataflow/NPA/LLVM/AnalysisSupport.h"

#include <map>
#include <unordered_map>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/Module.h>

namespace npa {

class InterLiveVariables {
public:
  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, TaintTransformer::value_type> summaries;
    std::map<BlockKey, llvm::APInt> blockFacts;
    std::unordered_map<const llvm::Value *, unsigned> valueBits;
    unsigned bitWidth = 1;
  };

  static Result run(llvm::Module &M, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible);
};

} // namespace npa

#endif // NPA_INTERPROC_LIVE_VARIABLES_H
