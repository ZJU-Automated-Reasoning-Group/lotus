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
#include "Checker/Report/BugReportMgr.h"
#include "TestUtils/LLVMHelpers.h"

#ifndef GTEST_INTERNAL_CPLUSPLUS_LANG
#define GTEST_INTERNAL_CPLUSPLUS_LANG 201703L
#endif
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>

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

  AEResult runAE(
      Module *module, bool analyzeAllFunctions = true,
      bool enableMemLeak = false,
      AbstractInterpretation::HandleRecur recursionMode =
          AbstractInterpretation::WIDEN_NARROW,
      std::optional<unsigned> widenDelay = 3u, bool enableDivZero = false,
      bool enableIntOverflow = false) {
    if (!module->getFunction("main")) {
      FunctionType *MainTy =
          FunctionType::get(Type::getInt32Ty(context), false);
      Function *Main =
          Function::Create(MainTy, Function::ExternalLinkage, "main", module);
      BasicBlock *Entry = BasicBlock::Create(context, "entry", Main);
      ReturnInst::Create(context, ConstantInt::get(Type::getInt32Ty(context), 0),
                         Entry);
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

    return {overflowDetectorPtr->getBugCount(), nullDetectorPtr->getBugCount(),
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

    for (const BasicBlock &bb : *func) {
      for (const Instruction &inst : bb) {
        if (inst.getName() == instName) {
          return &inst;
        }
      }
    }

    ADD_FAILURE() << "Instruction '" << instName.str() << "' not found in "
                  << functionName.str();
    return nullptr;
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
TEST_F(AECheckerTest, ConstantArrayBufferOverflow) {
  const char *source = R"(
    define void @test_overflow() {
      %arr = alloca [10 x i32], align 4
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 15
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}

// Test 2: Variable-length array (VLA) handling
TEST_F(AECheckerTest, VariableLengthArray) {
  const char *source = R"(
    define void @test_vla(i32 %n) {
      %arr = alloca i32, i32 %n
      %gep = getelementptr inbounds i32, i32* %arr, i64 0
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.null_bugs, 0u);
}

// Test 3: Nested GEP offset tracking
TEST_F(AECheckerTest, NestedGEP) {
  const char *source = R"(
    define void @test_nested_gep() {
      %arr = alloca [10 x [20 x i32]], align 4
      %gep1 = getelementptr inbounds [10 x [20 x i32]], [10 x [20 x i32]]* %arr, i64 0, i64 5
      %gep2 = getelementptr inbounds [20 x i32], [20 x i32]* %gep1, i64 0, i64 15
      store i32 42, i32* %gep2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
  // Nested GEP should correctly track offset from base
  // Offset = 5 * (20 * 4) + 15 * 4 = 400 + 60 = 460 bytes
  // Array size = 10 * 20 * 4 = 800 bytes, so this is safe
}

// Test 4: Null pointer dereference detection
TEST_F(AECheckerTest, NullPointerDeref) {
  const char *source = R"(
    define void @test_null_deref() {
      %ptr = alloca i32*
      store i32* null, i32** %ptr
      %p = load i32*, i32** %ptr
      %val = load i32, i32* %p
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.null_bugs, 0u);
}

// Test 5: Null pointer dereference in GEP
TEST_F(AECheckerTest, NullPointerDerefGEP) {
  const char *source = R"(
    define void @test_null_gep() {
      %ptr = alloca i32*
      store i32* null, i32** %ptr
      %p = load i32*, i32** %ptr
      %gep = getelementptr inbounds i32, i32* %p, i64 5
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.null_bugs, 0u);
}

// Test 6: External API - memcpy buffer overflow
TEST_F(AECheckerTest, MemcpyBufferOverflow) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
    
    define void @test_memcpy_overflow() {
      %dst = alloca [10 x i8], align 1
      %src = alloca [20 x i8], align 1
      %dst_ptr = bitcast [10 x i8]* %dst to i8*
      %src_ptr = bitcast [20 x i8]* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst_ptr, i8* %src_ptr, i64 15, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}

TEST_F(AECheckerTest, MemcpyInteriorPointerOverflow) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define void @test_memcpy_tail_overflow() {
      %dst = alloca [10 x i8], align 1
      %src = alloca [4 x i8], align 1
      %dst_base = bitcast [10 x i8]* %dst to i8*
      %dst_tail = getelementptr inbounds i8, i8* %dst_base, i64 8
      %src_ptr = bitcast [4 x i8]* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst_tail, i8* %src_ptr, i64 4, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AECheckerTest, MemcpyInteriorPointerWithinRemainingCapacity) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define void @test_memcpy_tail_safe() {
      %dst = alloca [10 x i8], align 1
      %src = alloca [2 x i8], align 1
      %dst_base = bitcast [10 x i8]* %dst to i8*
      %dst_tail = getelementptr inbounds i8, i8* %dst_base, i64 8
      %src_ptr = bitcast [2 x i8]* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst_tail, i8* %src_ptr, i64 2, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 7: External API - strcpy buffer overflow
TEST_F(AECheckerTest, StrcpyBufferOverflow) {
  const char *source = R"(
    declare i8* @strcpy(i8*, i8*)
    
    define void @test_strcpy_overflow() {
      %dst = alloca [10 x i8], align 1
      %src = alloca [20 x i8], align 1
      %dst_ptr = bitcast [10 x i8]* %dst to i8*
      %src_ptr = bitcast [20 x i8]* %src to i8*
      call i8* @strcpy(i8* %dst_ptr, i8* %src_ptr)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
}

// Test 8: Safe buffer access (no overflow)
TEST_F(AECheckerTest, SafeBufferAccess) {
  const char *source = R"(
    define void @test_safe_access() {
      %arr = alloca [10 x i32], align 4
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 5
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}

// Test 9: Complex control flow with buffer access
TEST_F(AECheckerTest, ComplexControlFlow) {
  const char *source = R"(
    define void @test_control_flow(i32 %n) {
      %arr = alloca [10 x i32], align 4
      %cmp = icmp slt i32 %n, 10
      br i1 %cmp, label %if_true, label %if_false
    
    if_true:
      %n64 = sext i32 %n to i64
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 %n64
      store i32 42, i32* %gep
      br label %end
    
    if_false:
      %gep2 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 15
      store i32 42, i32* %gep2
      br label %end
    
    end:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AECheckerTest, SignedGreaterEqualFalseBranchRefinement) {
  const char *source = R"(
    define void @test_sge_false_branch(i32 %n) {
    entry:
      %cmp = icmp sge i32 %n, 2
      br i1 %cmp, label %safe, label %checked

    safe:
      ret void

    checked:
      %arr = alloca [2 x i32], align 4
      %n64 = sext i32 %n to i64
      %gep = getelementptr inbounds [2 x i32], [2 x i32]* %arr, i64 0, i64 %n64
      store i32 1, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 10: VLA with tracked size from abstract state
TEST_F(AECheckerTest, VLATrackedSize) {
  const char *source = R"(
    define void @test_vla_tracked(i32 %n) {
      %n_clamped = call i32 @llvm.smin.i32(i32 %n, i32 100)
      %arr = alloca i32, i32 %n_clamped
      %gep = getelementptr inbounds i32, i32* %arr, i64 50
      store i32 42, i32* %gep
      ret void
    }
    
    declare i32 @llvm.smin.i32(i32, i32)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 11: Heap allocation buffer overflow
TEST_F(AECheckerTest, HeapAllocationOverflow) {
  const char *source = R"(
    declare i8* @malloc(i64)
    
    define void @test_heap_overflow() {
      %ptr = call i8* @malloc(i64 10)
      %gep = getelementptr inbounds i8, i8* %ptr, i64 15
      store i8 42, i8* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
}

// Test 12: Struct field access
TEST_F(AECheckerTest, StructFieldAccess) {
  const char *source = R"(
    %struct.Test = type { i32, i32, i32 }
    
    define void @test_struct() {
      %s = alloca %struct.Test, align 4
      %gep = getelementptr inbounds %struct.Test, %struct.Test* %s, i64 0, i32 2
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 13: Multiple buffer accesses in loop
TEST_F(AECheckerTest, LoopBufferAccess) {
  const char *source = R"(
    define void @test_loop() {
      %arr = alloca [10 x i32], align 4
      br label %loop
    
    loop:
      %i = phi i32 [ 0, %0 ], [ %i_next, %loop ]
      %i64 = sext i32 %i to i64
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 %i64
      store i32 42, i32* %gep
      %i_next = add i32 %i, 1
      %cmp = icmp slt i32 %i_next, 10
      br i1 %cmp, label %loop, label %exit
    
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 14: Inter-procedural buffer overflow
TEST_F(AECheckerTest, InterproceduralOverflow) {
  const char *source = R"(
    define void @helper(i32* %arr, i32 %idx) {
      %idx64 = sext i32 %idx to i64
      %gep = getelementptr inbounds i32, i32* %arr, i64 %idx64
      store i32 42, i32* %gep
      ret void
    }
    
    define i32 @main() {
      %arr = alloca [10 x i32], align 4
      %arr_ptr = bitcast [10 x i32]* %arr to i32*
      call void @helper(i32* %arr_ptr, i32 15)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), false);
  EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AECheckerTest, InterproceduralStoreSideEffectsReachCaller) {
  const char *source = R"(
    define void @set_null(i32** %slot) {
    entry:
      store i32* null, i32** %slot
      ret void
    }

    define void @test_side_effect() {
    entry:
      %slot = alloca i32*
      %x = alloca i32
      store i32* %x, i32** %slot
      call void @set_null(i32** %slot)
      %p = load i32*, i32** %slot
      %v = load i32, i32* %p
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.null_bugs, 0u);
}

// Test 15: Stub function SAFE_BUFACCESS
TEST_F(AECheckerTest, StubSafeBufAccess) {
  const char *source = R"(
    declare void @SAFE_BUFACCESS(i8*, i32)
    
    define void @test_safe_stub() {
      %arr = alloca [10 x i8], align 1
      %arr_ptr = bitcast [10 x i8]* %arr to i8*
      call void @SAFE_BUFACCESS(i8* %arr_ptr, i32 5)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.pending_checkpoints, 0u);
}

// Test 16: Stub function UNSAFE_BUFACCESS
TEST_F(AECheckerTest, StubUnsafeBufAccess) {
  const char *source = R"(
    declare void @UNSAFE_BUFACCESS(i8*, i32)
    
    define void @test_unsafe_stub() {
      %arr = alloca [10 x i8], align 1
      %arr_ptr = bitcast [10 x i8]* %arr to i8*
      call void @UNSAFE_BUFACCESS(i8* %arr_ptr, i32 15)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.pending_checkpoints, 0u);
}

// Test 17: AbstractState VLA size computation
TEST_F(AECheckerTest, AbstractStateVLASize) {
  AbstractState as;

  // Create a mock alloca instruction context
  LLVMContext ctx;
  Module M("test", ctx);
  FunctionType *FTy = FunctionType::get(Type::getVoidTy(ctx), false);
  Function *F = Function::Create(FTy, Function::ExternalLinkage, "test", M);
  BasicBlock *BB = BasicBlock::Create(ctx, "entry", F);

  // Create VLA alloca with non-constant size
  Type *Int32Ty = Type::getInt32Ty(ctx);
  // Use a parameter as size (simulating VLA)
  Argument *SizeArg = new Argument(Int32Ty, "size");
  AllocaInst *VLA = new AllocaInst(Int32Ty, 0, SizeArg, "vla", BB);

  // Set up abstract state with size tracking
  uint32_t sizeId = AbstractInterpretation::getValueIdStatic(SizeArg);
  as[sizeId] = AbstractValue(IntervalValue(50, 100));

  // Test improved VLA size computation
  uint32_t computedSize = as.getAllocaInstByteSize(VLA, as);

  // Should use upper bound from abstract state (100) * sizeof(i32) = 400
  // But clamped to MaxFieldLimit if needed
  EXPECT_GT(computedSize, 0);
  EXPECT_LE(computedSize, MaxFieldLimit);
}

// Test 18: GEP offset accumulation for nested GEPs
TEST_F(AECheckerTest, NestedGEPOffsetAccumulation) {
  const char *source = R"(
    define void @test_nested_gep_offset() {
      %arr = alloca [100 x i32], align 4
      %gep1 = getelementptr inbounds [100 x i32], [100 x i32]* %arr, i64 0, i64 10
      %gep2 = getelementptr inbounds i32, i32* %gep1, i64 5
      store i32 42, i32* %gep2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AECheckerTest, NestedGEPOverflowAfterOffsetBase) {
  const char *source = R"(
    define void @test_nested_gep_overflow() {
      %arr = alloca [100 x i32], align 4
      %gep1 = getelementptr inbounds [100 x i32], [100 x i32]* %arr, i64 0, i64 90
      %gep2 = getelementptr inbounds i32, i32* %gep1, i64 20
      store i32 42, i32* %gep2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
}

// Test 19: Buffer overflow at boundary
TEST_F(AECheckerTest, BoundaryOverflow) {
  const char *source = R"(
    define void @test_boundary() {
      %arr = alloca [10 x i32], align 4
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 10
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
}

// Test 20: Multiple detectors interaction
TEST_F(AECheckerTest, MultipleDetectors) {
  const char *source = R"(
    define void @test_multiple_issues() {
      %arr = alloca [10 x i32], align 4
      %ptr = alloca i32*
      store i32* null, i32** %ptr
      %p = load i32*, i32** %ptr
      %gep1 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 15
      %gep2 = getelementptr inbounds i32, i32* %p, i64 5
      store i32 42, i32* %gep1
      store i32 42, i32* %gep2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AECheckerTest, UseAfterFreeDetection) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @test_uaf() {
      %p = call i8* @malloc(i64 16)
      call void @free(i8* %p)
      %q = getelementptr inbounds i8, i8* %p, i64 1
      store i8 1, i8* %q
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.uaf_bugs, 0u);
}

TEST_F(AECheckerTest, FreedPointerAlsoCountsAsUnsafeDeref) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @test_freed_deref() {
      %p = call i8* @malloc(i64 16)
      call void @free(i8* %p)
      %q = getelementptr inbounds i8, i8* %p, i64 1
      store i8 1, i8* %q
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.null_bugs, 0u);
  EXPECT_GT(result.uaf_bugs, 0u);
}

TEST_F(AECheckerTest, InvalidFreeDetection) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @test_double_free() {
      %p = call i8* @malloc(i64 32)
      call void @free(i8* %p)
      call void @free(i8* %p)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.invalid_free_bugs, 0u);
}

TEST_F(AECheckerTest, InvalidFreeOfStackObject) {
  const char *source = R"(
    declare void @free(i8*)

    define void @test_free_stack() {
      %buf = alloca [8 x i8], align 1
      %ptr = bitcast [8 x i8]* %buf to i8*
      call void @free(i8* %ptr)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.invalid_free_bugs, 0u);
}

TEST_F(AECheckerTest, InvalidFreeOfGlobalObject) {
  const char *source = R"(
    @g = global i8 0
    declare void @free(i8*)

    define void @test_free_global() {
      call void @free(i8* @g)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.invalid_free_bugs, 0u);
}

TEST_F(AECheckerTest, FirstFreeIsNotInvalid) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @test_free_once() {
      %p = call i8* @malloc(i64 32)
      call void @free(i8* %p)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.invalid_free_bugs, 0u);
}

TEST_F(AECheckerTest, FreeNullIsNotInvalid) {
  const char *source = R"(
    declare void @free(i8*)

    define void @test_free_null() {
      call void @free(i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.invalid_free_bugs, 0u);
}

TEST_F(AECheckerTest, GlobalPointerInitializer) {
  const char *source = R"(
    @g = global i32 0
    @p = global i32* @g

    define i32 @main() {
    entry:
      %q = load i32*, i32** @p
      store i32 1, i32* %q
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), false);
  EXPECT_EQ(result.null_bugs, 0u);
}

TEST_F(AECheckerTest, GlobalAggregateInitializerPreservesNullField) {
  const char *source = R"(
    @x = global i32 42
    @ptrs = global [2 x i32*] [i32* null, i32* @x]

    define i32 @main() {
    entry:
      %slot = getelementptr inbounds [2 x i32*], [2 x i32*]* @ptrs, i64 0, i64 0
      %p = load i32*, i32** %slot
      %v = load i32, i32* %p
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), false);
  EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AECheckerTest, GlobalConstantExprInitializerTracksSubobjectOffset) {
  const char *source = R"(
    @arr = global [4 x i32] zeroinitializer
    @p = global i32* getelementptr inbounds ([4 x i32], [4 x i32]* @arr, i64 0, i64 3)

    define i32 @main() {
    entry:
      %q = load i32*, i32** @p
      %bad = getelementptr inbounds i32, i32* %q, i64 1
      store i32 1, i32* %bad
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), false);
  EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AECheckerTest, IndirectExternalMemcpyOverflow) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define void @test_indirect_memcpy() {
      %dst = alloca [4 x i8], align 1
      %src = alloca [8 x i8], align 1
      %dst_ptr = bitcast [4 x i8]* %dst to i8*
      %src_ptr = bitcast [8 x i8]* %src to i8*
      %fp = alloca void (i8*, i8*, i64, i1)*, align 8
      store void (i8*, i8*, i64, i1)* @llvm.memcpy.p0i8.p0i8.i64,
            void (i8*, i8*, i64, i1)** %fp
      %f = load void (i8*, i8*, i64, i1)*, void (i8*, i8*, i64, i1)** %fp
      call void %f(i8* %dst_ptr, i8* %src_ptr, i64 8, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}

TEST_F(AECheckerTest, IndirectExternalMemcpyAndMemmoveJoinNoNullFalsePositive) {
  const char *source = R"(
    declare i1 @unknown()
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
    declare void @llvm.memmove.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define void @test_indirect_copy_join() {
      %dst = alloca [4 x i8], align 1
      %src = alloca [8 x i8], align 1
      %dst_ptr = bitcast [4 x i8]* %dst to i8*
      %src_ptr = bitcast [8 x i8]* %src to i8*
      %cond = call i1 @unknown()
      %fp = select i1 %cond,
                   void (i8*, i8*, i64, i1)* @llvm.memcpy.p0i8.p0i8.i64,
                   void (i8*, i8*, i64, i1)* @llvm.memmove.p0i8.p0i8.i64
      call void %fp(i8* %dst_ptr, i8* %src_ptr, i64 8, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}

TEST_F(AECheckerTest, RecursiveSelfCallTopModeReturnsTop) {
  const char *source = R"(
    define i32 @demo(i32 %a) {
    entry:
      %cmp = icmp sge i32 %a, 10000
      br i1 %cmp, label %base, label %recur

    recur:
      %inc = add i32 %a, 1
      %res = call i32 @demo(i32 %inc)
      ret i32 %res

    base:
      ret i32 %a
    }

    define i32 @main() {
    entry:
      %res = call i32 @demo(i32 0)
      ret i32 %res
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result =
      runAE(module.get(), true, false, AbstractInterpretation::TOP, 3u);
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);

  IntervalValue ret = getFunctionReturnInterval(module.get(), "main");
  EXPECT_TRUE(ret.isTop()) << ret.toString();
}

TEST_F(AECheckerTest, RecursiveSelfCallWidenOnlyKeepsLowerBound) {
  const char *source = R"(
    define i32 @demo(i32 %a) {
    entry:
      %cmp = icmp sge i32 %a, 10000
      br i1 %cmp, label %base, label %recur

    recur:
      %inc = add i32 %a, 1
      %res = call i32 @demo(i32 %inc)
      ret i32 %res

    base:
      ret i32 %a
    }

    define i32 @main() {
    entry:
      %res = call i32 @demo(i32 0)
      ret i32 %res
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), true, false,
                          AbstractInterpretation::WIDEN_ONLY, 3u);
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);

  IntervalValue ret = getFunctionReturnInterval(module.get(), "main");
  EXPECT_FALSE(ret.isBottom());
  EXPECT_EQ(ret.lb().getIntNumeral(), 10000) << ret.toString();
  EXPECT_TRUE(ret.ub().is_infinity()) << ret.toString();
}

TEST_F(AECheckerTest, RecursiveSelfCallWidenNarrowPreservesRecursiveSummary) {
  const char *source = R"(
    define i32 @demo(i32 %a) {
    entry:
      %cmp = icmp sge i32 %a, 10000
      br i1 %cmp, label %base, label %recur

    recur:
      %inc = add i32 %a, 1
      %res = call i32 @demo(i32 %inc)
      ret i32 %res

    base:
      ret i32 %a
    }

    define i32 @main() {
    entry:
      %res = call i32 @demo(i32 0)
      ret i32 %res
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), true, false,
                          AbstractInterpretation::WIDEN_NARROW, 3u);
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);

  IntervalValue ret = getFunctionReturnInterval(module.get(), "main");
  EXPECT_EQ(ret.lb().getIntNumeral(), 10000) << ret.toString();
  EXPECT_TRUE(ret.ub().is_infinity()) << ret.toString();
}

TEST_F(AECheckerTest, MutualRecursionPropagatesEntryCallStateAcrossSCC) {
  const char *source = R"(
    define i32 @odd(i32 %n) {
    entry:
      %cmp = icmp sge i32 %n, 5
      br i1 %cmp, label %base, label %step

    step:
      %inc = add i32 %n, 1
      %res = call i32 @even(i32 %inc)
      ret i32 %res

    base:
      ret i32 %n
    }

    define i32 @even(i32 %n) {
    entry:
      %cmp = icmp sge i32 %n, 5
      br i1 %cmp, label %base, label %step

    step:
      %inc = add i32 %n, 1
      %res = call i32 @odd(i32 %inc)
      ret i32 %res

    base:
      ret i32 %n
    }

    define i32 @main() {
    entry:
      %res = call i32 @odd(i32 0)
      ret i32 %res
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), true, false,
                          AbstractInterpretation::WIDEN_NARROW, 3u);
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);

  IntervalValue ret = getFunctionReturnInterval(module.get(), "main");
  EXPECT_EQ(ret.lb().getIntNumeral(), 5) << ret.toString();
  EXPECT_TRUE(ret.ub().is_infinity()) << ret.toString();
}

TEST_F(AECheckerTest,
       InternalIndirectCallWithRecursiveTargetPreservesRecursiveSummary) {
  const char *source = R"(
    declare i1 @unknown()

    define i32 @helper(i32 %n) {
    entry:
      ret i32 0
    }

    define i32 @demo(i32 %a) {
    entry:
      %cmp = icmp sge i32 %a, 5
      br i1 %cmp, label %base, label %recur

    recur:
      %inc = add i32 %a, 1
      %res = call i32 @demo(i32 %inc)
      ret i32 %res

    base:
      ret i32 %a
    }

    define i32 @dispatch(i32 %n) {
    entry:
      %cond = call i1 @unknown()
      %fp = select i1 %cond, i32 (i32)* @helper, i32 (i32)* @demo
      %res = call i32 %fp(i32 %n)
      ret i32 %res
    }

    define i32 @main() {
    entry:
      %res = call i32 @dispatch(i32 0)
      ret i32 %res
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), false, false,
                          AbstractInterpretation::WIDEN_NARROW, 3u);
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);

  IntervalValue ret = getFunctionReturnInterval(module.get(), "main");
  EXPECT_EQ(ret.lb().getIntNumeral(), 5) << ret.toString();
  EXPECT_TRUE(ret.ub().is_infinity()) << ret.toString();
}

TEST_F(AECheckerTest, DefaultWidenDelayMatchesExplicitThree) {
  const char *source = R"(
    define i32 @count_to_three() {
    entry:
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %inc, %body ]
      %cmp = icmp slt i32 %i, 3
      br i1 %cmp, label %body, label %exit

    body:
      %inc = add i32 %i, 1
      br label %loop

    exit:
      ret i32 %i
    }

    define i32 @main() {
    entry:
      %res = call i32 @count_to_three()
      ret i32 %res
    }
  )";

  auto defaultModule = parseModule(source);
  ASSERT_NE(defaultModule, nullptr);
  AEResult defaultResult = runAE(defaultModule.get(), true, false,
                                 AbstractInterpretation::WIDEN_NARROW,
                                 std::nullopt);
  IntervalValue defaultRet =
      getFunctionReturnInterval(defaultModule.get(), "main");

  auto explicitModule = parseModule(source);
  ASSERT_NE(explicitModule, nullptr);
  AEResult explicitResult = runAE(explicitModule.get(), true, false,
                                  AbstractInterpretation::WIDEN_NARROW, 3u);
  IntervalValue explicitRet =
      getFunctionReturnInterval(explicitModule.get(), "main");

  EXPECT_EQ(defaultResult.overflow_bugs, explicitResult.overflow_bugs);
  EXPECT_EQ(defaultResult.null_bugs, explicitResult.null_bugs);
  EXPECT_TRUE(defaultRet.equals(explicitRet));
}

TEST_F(AECheckerTest, ParitySensitiveRegressionHarness) {
  struct ParityCase {
    const char *name;
    const char *source;
    size_t expectedOverflow;
    size_t expectedNull;
    std::optional<IntervalValue> expectedMainReturn;
    AbstractInterpretation::HandleRecur recursionMode;
    std::optional<unsigned> widenDelay;
  };

  const std::vector<ParityCase> cases = {
      {"recursive_top_summary",
       R"(
         define i32 @demo(i32 %a) {
         entry:
           %cmp = icmp sge i32 %a, 6
           br i1 %cmp, label %base, label %recur

         recur:
           %inc = add i32 %a, 1
           %res = call i32 @demo(i32 %inc)
           ret i32 %res

         base:
           ret i32 %a
         }

         define i32 @main() {
         entry:
           %res = call i32 @demo(i32 0)
           ret i32 %res
         }
       )",
       0u,
       0u,
       IntervalValue::top(),
       AbstractInterpretation::TOP,
       3u},
      {"indirect_external_memcpy_no_null_false_positive",
       R"(
         declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

         define void @test_copy() {
           %dst = alloca [4 x i8], align 1
           %src = alloca [8 x i8], align 1
           %dst_ptr = bitcast [4 x i8]* %dst to i8*
           %src_ptr = bitcast [8 x i8]* %src to i8*
           %fp = alloca void (i8*, i8*, i64, i1)*, align 8
           store void (i8*, i8*, i64, i1)* @llvm.memcpy.p0i8.p0i8.i64,
                 void (i8*, i8*, i64, i1)** %fp
           %f = load void (i8*, i8*, i64, i1)*, void (i8*, i8*, i64, i1)** %fp
           call void %f(i8* %dst_ptr, i8* %src_ptr, i64 8, i1 false)
           ret void
         }
       )",
       1u,
       0u,
       std::nullopt,
       AbstractInterpretation::WIDEN_NARROW,
       3u},
  };

  for (const ParityCase &testCase : cases) {
    SCOPED_TRACE(testCase.name);
    auto module = parseModule(testCase.source);
    ASSERT_NE(module, nullptr);

    AEResult result =
        runAE(module.get(), true, false, testCase.recursionMode,
              testCase.widenDelay);
    EXPECT_EQ(result.overflow_bugs, testCase.expectedOverflow);
    EXPECT_EQ(result.null_bugs, testCase.expectedNull);
    if (testCase.expectedMainReturn.has_value()) {
      IntervalValue ret = getFunctionReturnInterval(module.get(), "main");
      EXPECT_TRUE(ret.equals(*testCase.expectedMainReturn));
    }
  }
}

TEST_F(AECheckerTest, MemoryLeakDetection) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define void @test_leak() {
      %p = call i8* @malloc(i64 32)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), true, true);
  EXPECT_GT(result.mem_leak_bugs, 0u);
}

TEST_F(AECheckerTest, MemoryLeakEscapeViaOutParameterIsNotReported) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define void @publish(i8** %out) {
    entry:
      %p = call i8* @malloc(i64 32)
      store i8* %p, i8** %out
      ret void
    }

    define i32 @main() {
    entry:
      %slot = alloca i8*, align 8
      call void @publish(i8** %slot)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), true, true);
  EXPECT_EQ(result.mem_leak_bugs, 0u);
}

TEST_F(AECheckerTest, MemoryLeakOnLastReferenceOverwrite) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define void @overwrite_last_ref() {
    entry:
      %slot = alloca i8*, align 8
      %p = call i8* @malloc(i64 8)
      store i8* %p, i8** %slot
      store i8* null, i8** %slot
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), true, true);
  EXPECT_GT(result.mem_leak_bugs, 0u);
}

TEST_F(AECheckerTest, DivZeroDetection) {
  const char *source = R"(
    define i32 @main(i32 %d) {
    entry:
      %q = sdiv i32 42, %d
      ret i32 %q
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), true, false,
                          AbstractInterpretation::WIDEN_NARROW, 3u, true,
                          false);
  EXPECT_GT(result.divzero_bugs, 0u);
}

TEST_F(AECheckerTest, IntegerOverflowDetection) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %q = add i32 2147483647, 1
      ret i32 %q
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), true, false,
                          AbstractInterpretation::WIDEN_NARROW, 3u, false,
                          true);
  EXPECT_GT(result.int_overflow_bugs, 0u);
}

TEST_F(AECheckerTest, MainRootedAnalysisSkipsUnreachableHelpers) {
  const char *source = R"(
    define void @helper() {
    entry:
      %ptr = alloca i32*
      store i32* null, i32** %ptr
      %p = load i32*, i32** %ptr
      %val = load i32, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), false);
  EXPECT_EQ(result.null_bugs, 0u);
}

TEST_F(AECheckerTest, AEBugReportsAreClearedBetweenRuns) {
  const char *leakSource = R"(
    declare i8* @malloc(i64)

    define void @test_leak() {
    entry:
      %p = call i8* @malloc(i64 32)
      ret void
    }
  )";

  auto leakModule = parseModule(leakSource);
  ASSERT_NE(leakModule, nullptr);

  AEResult leakResult = runAE(leakModule.get(), true, true);
  EXPECT_GT(leakResult.mem_leak_bugs, 0u);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  int leakType = mgr.find_bug_type("AE Memory Leak");
  ASSERT_GE(leakType, 0);
  const auto *leakReports = mgr.get_reports_for_type(leakType);
  ASSERT_NE(leakReports, nullptr);
  EXPECT_EQ(leakReports->size(), 1u);

  const char *divSource = R"(
    define i32 @test_divzero(i32 %d) {
    entry:
      %q = sdiv i32 42, %d
      ret i32 %q
    }
  )";

  auto divModule = parseModule(divSource);
  ASSERT_NE(divModule, nullptr);

  AEResult divResult =
      runAE(divModule.get(), true, false,
            AbstractInterpretation::WIDEN_NARROW, 3u, true, false);
  EXPECT_GT(divResult.divzero_bugs, 0u);

  leakReports = mgr.get_reports_for_type(leakType);
  EXPECT_TRUE(leakReports == nullptr || leakReports->empty());
}

TEST_F(AECheckerTest, EnabledChecksAutoInstallLibraryDetectors) {
  const char *source = R"(
    define void @test_auto_install(i32 %idx) {
    entry:
      %arr = alloca [4 x i32], align 4
      %slot = alloca i32*, align 8
      store i32* null, i32** %slot
      %p = load i32*, i32** %slot
      %idx64 = sext i32 %idx to i64
      %bad = getelementptr inbounds [4 x i32], [4 x i32]* %arr, i64 0, i64 %idx64
      store i32 1, i32* %bad
      %v = load i32, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      call void @test_auto_install(i32 6)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  ae.reset();
  ae.setStrictCheckpoint(false);
  ae.setAnalyzeAllFunctions(true);
  ae.setEnableBufOverflowCheck(true);
  ae.setEnableNullDerefCheck(true);
  ae.runOnModule(module.get());

  BugReportMgr &mgr = BugReportMgr::get_instance();
  int overflowType = mgr.find_bug_type("AE Buffer Overflow");
  ASSERT_GE(overflowType, 0);
  const auto *overflowReports = mgr.get_reports_for_type(overflowType);
  ASSERT_NE(overflowReports, nullptr);
  EXPECT_FALSE(overflowReports->empty());

  int nullType = mgr.find_bug_type("AE Null Dereference");
  ASSERT_GE(nullType, 0);
  const auto *nullReports = mgr.get_reports_for_type(nullType);
  ASSERT_NE(nullReports, nullptr);
  EXPECT_FALSE(nullReports->empty());
}

TEST_F(AECheckerTest, PosixMemalignStoresAllocatedPointerThroughOutParam) {
  const char *source = R"(
    declare i32 @posix_memalign(i8**, i64, i64)

    define void @test_posix_memalign() {
    entry:
      %slot = alloca i8*, align 8
      store i8* null, i8** %slot
      %res = call i32 @posix_memalign(i8** %slot, i64 16, i64 32)
      %p = load i8*, i8** %slot
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  runAE(module.get());

  const auto *load =
      dyn_cast<LoadInst>(findNamedInstruction(module.get(),
                                             "test_posix_memalign", "p"));
  ASSERT_NE(load, nullptr);
  AbstractValue value = getInstructionValue(load);
  EXPECT_TRUE(value.isAddr());
  EXPECT_FALSE(value.getAddrs().empty());
  EXPECT_FALSE(value.getAddrs().contains(NullMemAddr));
}

TEST_F(AECheckerTest, AsprintfStoresAllocatedPointerThroughOutParam) {
  const char *source = R"(
    @.fmt = private unnamed_addr constant [3 x i8] c"%d\00"
    declare i32 @asprintf(i8**, i8*, ...)

    define void @test_asprintf() {
    entry:
      %slot = alloca i8*, align 8
      store i8* null, i8** %slot
      %fmt = getelementptr inbounds [3 x i8], [3 x i8]* @.fmt, i64 0, i64 0
      %res = call i32 (i8**, i8*, ...) @asprintf(i8** %slot, i8* %fmt, i32 7)
      %p = load i8*, i8** %slot
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  runAE(module.get());

  const auto *load =
      dyn_cast<LoadInst>(findNamedInstruction(module.get(), "test_asprintf", "p"));
  ASSERT_NE(load, nullptr);
  AbstractValue value = getInstructionValue(load);
  EXPECT_TRUE(value.isAddr());
  EXPECT_FALSE(value.getAddrs().empty());
  EXPECT_FALSE(value.getAddrs().contains(NullMemAddr));
}

TEST_F(AECheckerTest, ScanfUnsignedFormatProducesNonNegativeRange) {
  const char *source = R"(
    @.fmtu = private unnamed_addr constant [3 x i8] c"%u\00"
    declare i32 @scanf(i8*, ...)

    define i32 @read_unsigned() {
    entry:
      %x = alloca i32, align 4
      %fmt = getelementptr inbounds [3 x i8], [3 x i8]* @.fmtu, i64 0, i64 0
      %rv = call i32 (i8*, ...) @scanf(i8* %fmt, i32* %x)
      %v = load i32, i32* %x
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  runAE(module.get());

  IntervalValue ret = getFunctionReturnInterval(module.get(), "read_unsigned");
  EXPECT_GE(ret.lb().getIntNumeralOrZero(), 0);
}

TEST_F(AECheckerTest, StrtoullProducesNonNegativeRange) {
  const char *source = R"(
    @.num = private unnamed_addr constant [2 x i8] c"0\00"
    declare i64 @strtoull(i8*, i8**, i32)

    define i64 @read_num() {
    entry:
      %num = getelementptr inbounds [2 x i8], [2 x i8]* @.num, i64 0, i64 0
      %val = call i64 @strtoull(i8* %num, i8** null, i32 10)
      ret i64 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  runAE(module.get());

  IntervalValue ret = getFunctionReturnInterval(module.get(), "read_num");
  EXPECT_GE(ret.lb().getIntNumeralOrZero(), 0);
}

TEST_F(AECheckerTest, NegativeGEPProducesConcreteAddressSet) {
  const char *source = R"(
    define void @test_negative_gep_addr() {
    entry:
      %arr = alloca [8 x i8], align 1
      %p = getelementptr inbounds [8 x i8], [8 x i8]* %arr, i64 0, i64 5
      %q = getelementptr inbounds i8, i8* %p, i64 -1
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  runAE(module.get());

  const auto *q =
      findNamedInstruction(module.get(), "test_negative_gep_addr", "q");
  ASSERT_NE(q, nullptr);

  AbstractValue qVal = getInstructionValue(q);
  EXPECT_TRUE(qVal.isAddr());
  EXPECT_FALSE(qVal.getAddrs().empty());
}

TEST_F(AECheckerTest, NegativeGEPMemcpyRespectsBacktrackedCapacity) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define void @test_negative_gep_memcpy_safe() {
    entry:
      %dst = alloca [8 x i8], align 1
      %src = alloca [4 x i8], align 1
      %dst_tail = getelementptr inbounds [8 x i8], [8 x i8]* %dst, i64 0, i64 5
      %dst_back = getelementptr inbounds i8, i8* %dst_tail, i64 -1
      %src_ptr = getelementptr inbounds [4 x i8], [4 x i8]* %src, i64 0, i64 0
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst_back, i8* %src_ptr, i64 4, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}
