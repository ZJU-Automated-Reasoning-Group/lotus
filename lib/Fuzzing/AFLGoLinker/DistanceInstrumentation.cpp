#include "Fuzzing/AFLGoLinker/DistanceInstrumentation.h"
#include "Fuzzing/Analysis/BasicBlockDistance.h"

#include <llvm/IR/IRBuilder.h>

using namespace llvm;

// XXX: this should be kept in sync with libaflgo_targets/src/distance.rs
const auto DistanceResolution = 1e3;
const char *AFLGoTraceBBDistanceName = "__aflgo_trace_bb_distance";

PreservedAnalyses
AFLGoDistanceInstrumentationPass::run(Module &M, ModuleAnalysisManager &AM) {
  auto &C = M.getContext();
  auto *VoidTy = Type::getVoidTy(C);
  auto *Int64Ty = Type::getInt64Ty(C);
  FunctionType *FT = FunctionType::get(VoidTy, {Int64Ty}, false);
  auto AFLGoTraceBBDistance = M.getOrInsertFunction(AFLGoTraceBBDistanceName, FT);

  auto &BBDistanceResult = AM.getResult<AFLGoBasicBlockDistanceAnalysis>(M);

  for (auto &F : M) {
    if (F.isDeclaration()) {
      continue;
    }

    auto BBDistances = BBDistanceResult.computeBBDistances(F);
    for (auto &BB : F) {
      if (BBDistances.find(&BB) == BBDistances.end()) {
        continue;
      }

      auto Distance =
          static_cast<uint64_t>(BBDistances[&BB] * DistanceResolution);
      auto *DistanceValue = ConstantInt::get(Int64Ty, Distance);
      IRBuilder<> IRB(&*BB.getFirstInsertionPt());
      IRB.CreateCall(AFLGoTraceBBDistance, {DistanceValue});
    }
  }

  return PreservedAnalyses::none();
}