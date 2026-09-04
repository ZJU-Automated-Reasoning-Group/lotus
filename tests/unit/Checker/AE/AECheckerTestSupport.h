#pragma once

/**
 * @file AECheckerTest.cpp
 * @brief Comprehensive unit tests for Abstract Execution (AE) checker
 *
 * Tests buffer overflow detection, null pointer dereference detection,
 * VLA handling, nested GEPs, and complex control flow scenarios.
 */

#include "Checker/AE/AEDetector.h"
#include "Checker/AE/AbstractInterpretation.h"
#include "Checker/AE/AbstractState.h"
#include "Checker/AE/IntervalValue.h"
#include "Checker/Framework/BugReportMgr.h"
#include "TestUtils/LLVMHelpers.h"

#ifndef GTEST_INTERNAL_CPLUSPLUS_LANG
#define GTEST_INTERNAL_CPLUSPLUS_LANG 201703L
#endif
#include <cstdlib>
#include <optional>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;

class AECheckerTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { setenv("LOTUS_AE_QUIET", "1", 1); }

  struct AEResult {
    size_t overflow_bugs{0};
    size_t null_bugs{0};
    size_t divzero_bugs{0};
    size_t int_overflow_bugs{0};
    size_t uaf_bugs{0};
    size_t invalid_free_bugs{0};
    size_t mem_leak_bugs{0};
    size_t pending_checkpoints{0};
  };

  LLVMContext context;
  std::unique_ptr<Module> parseModule(const char *source) {
    return lotus::unittest::parseModule(context, source, "AECheckerTest");
  }

  AEResult runAE(Module *module, bool analyzeAllFunctions = true,
                 bool enableMemLeak = false,
                 AbstractInterpretation::HandleRecur recursionMode =
                     AbstractInterpretation::WIDEN_NARROW,
                 std::optional<unsigned> widenDelay = 3u,
                 bool enableDivZero = false, bool enableIntOverflow = false) {
    if (!module->getFunction("main")) {
      FunctionType *MainTy =
          FunctionType::get(Type::getInt32Ty(context), false);
      Function *Main =
          Function::Create(MainTy, Function::ExternalLinkage, "main", module);
      BasicBlock *Entry = BasicBlock::Create(context, "entry", Main);
      ReturnInst::Create(context,
                         ConstantInt::get(Type::getInt32Ty(context), 0), Entry);
    }

    AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
    ae.reset();
    ae.setStrictCheckpoint(false);
    ae.setAnalyzeAllFunctions(analyzeAllFunctions);
    ae.setRecursionMode(recursionMode);
    if (widenDelay.has_value()) {
      ae.setWidenDelay(*widenDelay);
    }

    auto overflowDetector = std::make_unique<BufOverflowDetector>();
    auto *overflowDetectorPtr = overflowDetector.get();
    auto nullDetector = std::make_unique<NullptrDerefDetector>();
    auto *nullDetectorPtr = nullDetector.get();
    std::unique_ptr<DivZeroDetector> divZeroDetector;
    DivZeroDetector *divZeroDetectorPtr = nullptr;
    std::unique_ptr<OverflowDetector> intOverflowDetector;
    OverflowDetector *intOverflowDetectorPtr = nullptr;
    auto uafDetector = std::make_unique<UseAfterFreeDetector>();
    auto *uafDetectorPtr = uafDetector.get();
    auto invalidFreeDetector = std::make_unique<InvalidFreeDetector>();
    auto *invalidFreeDetectorPtr = invalidFreeDetector.get();
    std::unique_ptr<MemLeakDetector> memLeakDetector;
    MemLeakDetector *memLeakDetectorPtr = nullptr;
    ae.addDetector(std::move(overflowDetector));
    ae.addDetector(std::move(nullDetector));
    ae.setEnableDivZeroCheck(enableDivZero);
    if (enableDivZero) {
      divZeroDetector = std::make_unique<DivZeroDetector>();
      divZeroDetectorPtr = divZeroDetector.get();
      ae.addDetector(std::move(divZeroDetector));
    }
    ae.setEnableOverflowCheck(enableIntOverflow);
    if (enableIntOverflow) {
      intOverflowDetector = std::make_unique<OverflowDetector>();
      intOverflowDetectorPtr = intOverflowDetector.get();
      ae.addDetector(std::move(intOverflowDetector));
    }
    ae.addDetector(std::move(uafDetector));
    ae.addDetector(std::move(invalidFreeDetector));
    ae.setEnableMemLeakCheck(enableMemLeak);
    if (enableMemLeak) {
      memLeakDetector = std::make_unique<MemLeakDetector>();
      memLeakDetectorPtr = memLeakDetector.get();
      ae.addDetector(std::move(memLeakDetector));
    }

    ae.runOnModule(module);

    return {overflowDetectorPtr->getBugCount(),
            nullDetectorPtr->getBugCount(),
            divZeroDetectorPtr ? divZeroDetectorPtr->getBugCount() : 0u,
            intOverflowDetectorPtr ? intOverflowDetectorPtr->getBugCount() : 0u,
            uafDetectorPtr->getBugCount(),
            invalidFreeDetectorPtr->getBugCount(),
            memLeakDetectorPtr ? memLeakDetectorPtr->getBugCount() : 0u,
            ae.checkpoints.size()};
  }

  IntervalValue getFunctionReturnInterval(const Module *module,
                                          StringRef functionName) {
    const Function *func = module->getFunction(functionName);
    EXPECT_NE(func, nullptr);
    IntervalValue joined = IntervalValue::bottom();
    bool found = false;

    AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
    for (const BasicBlock &bb : *func) {
      const auto *ret = dyn_cast<ReturnInst>(bb.getTerminator());
      if (!ret || !ret->getReturnValue() || !ae.hasAbsStateFromTrace(ret)) {
        continue;
      }

      const AbstractState &state = ae.getAbsStateFromTrace(ret);
      uint32_t retId =
          AbstractInterpretation::getValueIdStatic(ret->getReturnValue());
      auto it = state._varToAbsVal.find(retId);
      if (it == state._varToAbsVal.end() || !it->second.isInterval()) {
        continue;
      }

      if (!found) {
        joined = it->second.getInterval();
        found = true;
      } else {
        joined.join_with(it->second.getInterval());
      }
    }

    EXPECT_TRUE(found);
    return joined;
  }

  const Instruction *findNamedInstruction(const Module *module,
                                          StringRef functionName,
                                          StringRef instName) {
    const Function *func = module->getFunction(functionName);
    EXPECT_NE(func, nullptr);
    if (!func) {
      return nullptr;
    }

    const auto *inst = lotus::unittest::findInstructionByName(*func, instName);
    if (!inst) {
      ADD_FAILURE() << "Instruction '" << instName.str() << "' not found in "
                    << functionName.str();
    }
    return inst;
  }

  AbstractValue getInstructionValue(const Instruction *inst) {
    EXPECT_NE(inst, nullptr);
    AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
    const AbstractState &state = ae.getAbsStateFromTrace(inst);
    uint32_t instId = AbstractInterpretation::getValueIdStatic(inst);
    auto it = state._varToAbsVal.find(instId);
    EXPECT_NE(it, state._varToAbsVal.end());
    if (it == state._varToAbsVal.end()) {
      return AbstractValue();
    }
    return it->second;
  }
};

// Test 1: Constant-sized array buffer overflow detection

// Test 2: Variable-length array (VLA) handling

// Test 3: Nested GEP offset tracking

// Test 4: Null pointer dereference detection

// Test 5: Null pointer dereference in GEP

// Test 6: External API - memcpy buffer overflow



// Test 7: External API - strcpy buffer overflow

// Test 8: Safe buffer access (no overflow)

// Test 9: Complex control flow with buffer access


// Test 10: VLA with tracked size from abstract state

// Test 11: Heap allocation buffer overflow

// Test 12: Struct field access

// Test 13: Multiple buffer accesses in loop

// Test 14: Inter-procedural buffer overflow


// Test 15: Stub function SAFE_BUFACCESS

// Test 16: Stub function UNSAFE_BUFACCESS

// Test 17: AbstractState VLA size computation

// Test 18: GEP offset accumulation for nested GEPs


// Test 19: Buffer overflow at boundary

// Test 20: Multiple detectors interaction
