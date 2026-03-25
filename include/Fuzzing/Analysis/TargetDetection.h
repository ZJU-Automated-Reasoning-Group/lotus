#pragma once

#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/PassManager.h>

namespace llvm {

class AFLGoTargetDetectionAnalysis
    : public AnalysisInfoMixin<AFLGoTargetDetectionAnalysis> {
public:
  struct Result {
    SmallVector<std::pair<BasicBlock *, CallBase *>, 16> BBs;
    SmallVector<Instruction *, 16> Is;
  };

  constexpr static const char *const TargetFunctionName =
      "__aflgo_trace_bb_target";
  constexpr static const char *const TargetInstructionAnnotation =
      "libaflgo.target";

  static AnalysisKey Key;

  Result run(Function &F, FunctionAnalysisManager &FAM);
};

} // namespace llvm