#ifndef NPA_INTERPROC_NULLABILITY_H
#define NPA_INTERPROC_NULLABILITY_H

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"
#include "Dataflow/NPA/Domains/TaintTransferDomain.h"

#include <map>
#include <unordered_map>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/Module.h>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace npa {

class InterproceduralNullability {
public:
  struct Options {
    bool seed_entry_pointer_args = true;
    bool treat_unknown_calls_as_maybe_null = true;
    IndirectCallResolutionMode call_resolution_mode =
        IndirectCallResolutionMode::ClosedWorldTypeCompatible;
  };

  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, TaintTransferDomain::value_type> summaries;
    std::map<BlockKey, llvm::APInt> blockFacts;
    std::map<BlockKey, llvm::APInt> blockExitFacts;
    std::unordered_map<const llvm::Value *, unsigned> valueBits;
    std::unordered_map<const llvm::Value *, unsigned> memoryBits;
    std::unordered_map<const llvm::Value *, std::vector<unsigned>>
        pointerMemoryBits;

    bool isMaybeNull(const llvm::BasicBlock *block,
                     const llvm::Value *value) const;
    bool isMaybeNullMemory(const llvm::BasicBlock *block,
                           const llvm::Value *pointer) const;
  };

  static Result run(llvm::Module &M, lotus::AliasAnalysisWrapper &aliasAnalysis,
                    const Options &options, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC);
  static Result run(llvm::Module &M, lotus::AliasAnalysisWrapper &aliasAnalysis,
                    bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible);
  static Result run(llvm::Module &M, const Options &options,
                    bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC);
  static Result run(llvm::Module &M, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible);
};

} // namespace npa

#endif // NPA_INTERPROC_NULLABILITY_H
