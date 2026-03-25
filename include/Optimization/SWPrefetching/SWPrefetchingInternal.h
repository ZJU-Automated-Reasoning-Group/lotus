#pragma once

#include <string>

#include "Optimization/SWPrefetching/SWPrefetching.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/ProfileData/SampleProf.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"

namespace llvm {

using Hints = sampleprof::SampleRecord::CallTargetMap;

enum class PrefetchDistanceProvider {
  Profile,
  LBR,
  LLM,
  StaticAnalysis,
};

extern bool AutoFDOMapping;
extern cl::opt<PrefetchDistanceProvider> PrefetchDistanceProviderMode;
extern cl::opt<std::string> PrefetchFile;
extern cl::list<std::string> LBR_dist;
extern cl::list<std::string> LLM_dist;

ErrorOr<Hints> getHints(const Instruction &Inst,
                        const sampleprof::FunctionSamples *TopSamples);

} // namespace llvm
