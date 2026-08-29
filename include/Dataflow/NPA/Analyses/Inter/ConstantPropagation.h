#pragma once

#include "Dataflow/NPA/Domains/ConstantPropagationDomain.h"
#include "Dataflow/NPA/LLVM/AnalysisSupport.h"

#include <map>

namespace llvm {
class Module;
} // namespace llvm

namespace npa {

class InterConstantPropagation {
public:
  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, ConstantPropagationSummary::value_type> summaries;
    std::map<BlockKey, ConstantPropagationState> blockFacts;
  };

  static Result run(llvm::Module &M, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible);
};

} // namespace npa
