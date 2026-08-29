#pragma once

#include "Dataflow/NPA/Domains/IntervalDomain.h"
#include "Dataflow/NPA/LLVM/AnalysisSupport.h"

#include <map>

namespace llvm {
class Module;
} // namespace llvm

namespace npa {

class InterIntervalAnalysis {
public:
  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, IntervalSummary::value_type> summaries;
    std::map<BlockKey, IntervalState> blockFacts;
  };

  static Result run(llvm::Module &M, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible);
};

} // namespace npa
