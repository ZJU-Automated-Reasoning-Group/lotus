#pragma once

#include "Concurrency/MHP/HappensBeforeAnalysis.h"
#include "Concurrency/MHP/MHPAnalysis.h"
#include "Concurrency/Utils/CppAtomics.h"

#include "TestUtils/LLVMHelpers.h"

#include <llvm/Config/llvm-config.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;
using namespace lotus::unittest;

class AtomicHappensBeforeTest : public lotus::unittest::LlvmModuleTest {
protected:
  const Instruction *findStoreToGlobal(const Function &func,
                                       StringRef global_name) {
    for (const auto &bb : func) {
      for (const auto &inst : bb) {
        const auto *store = dyn_cast<StoreInst>(&inst);
        if (!store) {
          continue;
        }
        const Value *ptr = store->getPointerOperand()->stripPointerCasts();
        if (const auto *gv = dyn_cast<GlobalVariable>(ptr)) {
          if (gv->getName() == global_name) {
            return &inst;
          }
        }
      }
    }
    return nullptr;
  }
};
