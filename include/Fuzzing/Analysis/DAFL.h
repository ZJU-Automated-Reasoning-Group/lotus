#pragma once

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/MemoryBuffer.h>

#include <memory>

namespace llvm {

class DAFLAnalysis : public AnalysisInfoMixin<DAFLAnalysis> {
public:
  static AnalysisKey Key;

  using WeightTy = uint64_t;
  // optional because we might not have target instructions
  using Result = Optional<DenseMap<const BasicBlock *, WeightTy>>;

  DAFLAnalysis(std::string InputFile, bool NoTargetsNoError, bool DebugFiles,
               bool Verbose)
      : InputFile(InputFile), NoTargetsNoError(NoTargetsNoError),
        DebugFiles(DebugFiles), Verbose(Verbose) {}

  Result run(Module &M, ModuleAnalysisManager &);

private:
  Result readFromFile(Module &, std::unique_ptr<MemoryBuffer> &);

  std::string InputFile;
  bool NoTargetsNoError;
  bool DebugFiles;
  bool Verbose;
};

} // namespace llvm