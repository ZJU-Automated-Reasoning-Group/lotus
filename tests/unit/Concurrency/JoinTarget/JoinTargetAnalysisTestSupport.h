#pragma once

#include "Concurrency/JoinTarget/JoinTargetAnalysis.h"

#include "TestUtils/LLVMHelpers.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/InstIterator.h>
#include <algorithm>

using namespace llvm;
using namespace mhp;

class JoinTargetAnalysisTest : public lotus::unittest::LlvmModuleTest {};

inline SmallVector<const Instruction *, 4>
findCallsByName(const Module &module, StringRef functionName,
                StringRef calleeName) {
  SmallVector<const Instruction *, 4> calls;
  const Function *function = module.getFunction(functionName);
  if (!function) {
    return calls;
  }
  for (const Instruction &inst : instructions(function)) {
    const auto *call = dyn_cast<CallBase>(&inst);
    if (call && call->getCalledFunction() &&
        call->getCalledFunction()->getName() == calleeName) {
      calls.push_back(&inst);
    }
  }
  return calls;
}

