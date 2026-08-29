#ifndef NPA_INTERPROC_TAINT_H
#define NPA_INTERPROC_TAINT_H

#include "Dataflow/NPA/Domains/TaintDomain.h"
#include "Dataflow/NPA/LLVM/AnalysisSupport.h"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace npa {

class InterTaint {
public:
  struct Options {
    bool seed_main_pointer_args = false;
    bool propagate_pointer_value_on_load = false;
    bool fail_on_unsupported_specs = false;
    std::string taint_config_path;
    IndirectCallResolutionMode call_resolution_mode =
        IndirectCallResolutionMode::ClosedWorldTypeCompatible;
  };

  struct Result {
    struct SinkHit {
      const llvm::CallBase *call = nullptr;
      std::vector<std::string> tainted_inputs;
    };

    AnalysisStatus status;
    std::map<FunctionKey, TaintTransformer::value_type> summaries;
    std::map<BlockKey, llvm::APInt> blockFacts;
    std::map<BlockKey, llvm::APInt> blockExitFacts;
    std::map<BlockKey,
             std::unordered_map<const llvm::Value *, std::vector<unsigned>>>
        blockReachablePointerMemoryBits;
    std::unordered_map<const llvm::Value *, unsigned> valueBits;
    std::unordered_map<const llvm::Value *, std::vector<unsigned>>
        pointerMemoryBits;
    std::unordered_map<const llvm::Value *, std::vector<unsigned>>
        reachablePointerMemoryBits;

    bool isValueTainted(const llvm::BasicBlock *block,
                        const llvm::Value *value) const;
    bool isMemoryTainted(const llvm::BasicBlock *block,
                         const llvm::Value *pointer) const;
    bool isReachableMemoryTainted(const llvm::BasicBlock *block,
                                  const llvm::Value *pointer) const;
    bool isSinkTriggered(const llvm::CallBase *call) const;
    void reportVulnerabilities(llvm::raw_ostream &os) const;

    std::vector<SinkHit> sinkHits;
  };

  static Result run(llvm::Module &M, lotus::AliasAnalysisWrapper &aliasAnalysis,
                    const Options &options, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC);
  static Result run(llvm::Module &M, lotus::AliasAnalysisWrapper &aliasAnalysis,
                    bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible);
};

} // namespace npa

#endif // NPA_INTERPROC_TAINT_H
