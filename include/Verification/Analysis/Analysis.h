#pragma once

#include "llvm/Pass.h"

namespace lotus {
namespace verification {
namespace analysis {

llvm::Pass *createClassifyInstructionsPass();
llvm::Pass *createClassifyLoopsPass();
llvm::Pass *createCountInstrPass();
llvm::Pass *createGetTestTargetsPass();
llvm::Pass *createCheckModulePass();

} // namespace analysis
} // namespace verification
} // namespace lotus
