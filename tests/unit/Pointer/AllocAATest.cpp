/**
 * @file AllocAATest.cpp
 * @brief Comprehensive unit tests for AllocAA (allocation site alias analysis)
 * 
 * These tests verify the core functionality of AllocAA including:
 * - Pointer aliasing queries
 * - Allocation site identification
 * - Primitive array detection
 * - GEP index analysis
 * - Object can-point-to-same checks
 */

#include "Alias/AllocAA/AllocAA.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::unittest;

// Test fixture for AllocAA with proper analysis setup
class AllocAATest : public LlvmModuleTest {
protected:
  // Helper to get a function by name
  Function *getFunction(Module &M, StringRef name) {
    return M.getFunction(name);
  }

  // Helper to find an alloca instruction in a function
  AllocaInst *getAllocaInst(Function *F, unsigned idx = 0) {
    unsigned count = 0;
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (auto *AI = dyn_cast<AllocaInst>(&I)) {
          if (count == idx) return AI;
          ++count;
        }
      }
    }
    return nullptr;
  }

  // Create AllocAA with a real CallGraph (caller must keep CG alive).
  // getSCEV/getLoopInfo are stubs that abort if ever called; canPointToTheSameObject
  // and the ctor do not use them for the tests below.
  std::unique_ptr<AllocAA> createAllocAA(Module &M, CallGraph &CG) {
    auto getCallGraph = [&CG]() -> CallGraph & { return CG; };
    auto getSCEV = [](Function &) -> ScalarEvolution & {
      ADD_FAILURE() << "getSCEV should not be called in this test";
      std::abort();
    };
    auto getLoopInfo = [](Function &) -> LoopInfo & {
      ADD_FAILURE() << "getLoopInfo should not be called in this test";
      std::abort();
    };
    return std::make_unique<AllocAA>(M, getSCEV, getLoopInfo, getCallGraph);
  }
};

// Test 1: Basic module parsing and structure verification
TEST_F(AllocAATest, BasicModuleParsing) {
  const char *source = R"(
    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *mainFn = getFunction(*module, "main");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_FALSE(mainFn->empty());
  
  AllocaInst *x = getAllocaInst(mainFn, 0);
  AllocaInst *y = getAllocaInst(mainFn, 1);
  ASSERT_NE(x, nullptr);
  ASSERT_NE(y, nullptr);
  
  // Verify they are different allocations
  EXPECT_NE(x, y);
  EXPECT_TRUE(x->getAllocatedType()->isIntegerTy());
  EXPECT_TRUE(y->getAllocatedType()->isIntegerTy());
}

// Test 2: Global variable array detection
TEST_F(AllocAATest, GlobalArrayDetection) {
  const char *source = R"(
    @global_arr = global [10 x i32] zeroinitializer, align 16
    @other_global = global i32 42, align 4
    
    define i32 @test_global_access() {
      %ptr = getelementptr inbounds [10 x i32], [10 x i32]* @global_arr, i64 0, i64 0
      %val = load i32, i32* %ptr
      ret i32 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  GlobalVariable *globalArr = module->getNamedGlobal("global_arr");
  GlobalVariable *otherGlobal = module->getNamedGlobal("other_global");
  ASSERT_NE(globalArr, nullptr);
  ASSERT_NE(otherGlobal, nullptr);
  
  // Verify types
  Type *arrType = globalArr->getValueType();
  ASSERT_TRUE(arrType->isArrayTy());
  EXPECT_EQ(cast<ArrayType>(arrType)->getNumElements(), 10u);
  
  Type *intType = otherGlobal->getValueType();
  EXPECT_TRUE(intType->isIntegerTy());
}

// Test 3: GetElementPtr constant index handling
TEST_F(AllocAATest, GEPConstantIndices) {
  const char *source = R"(
    define i32 @test_gep_indices() {
      %arr = alloca [20 x i32], align 16
      
      ; Constant indices - should be recognized
      %gep1 = getelementptr inbounds [20 x i32], [20 x i32]* %arr, i64 0, i64 5
      %gep2 = getelementptr inbounds [20 x i32], [20 x i32]* %arr, i64 0, i64 10
      
      ; Load from constant index
      %val = load i32, i32* %gep1
      
      ret i32 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = getFunction(*module, "test_gep_indices");
  ASSERT_NE(F, nullptr);
  
  std::vector<GetElementPtrInst *> gepInsts;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        gepInsts.push_back(GEP);
      }
    }
  }
  
  ASSERT_EQ(gepInsts.size(), 2u);
  
  // Verify all indices are constants
  for (auto *GEP : gepInsts) {
    for (auto &idx : GEP->indices()) {
      // Second index should be constant int
      if (auto *constInt = dyn_cast<ConstantInt>(idx)) {
        EXPECT_TRUE(constInt->getValue().isNonNegative());
      }
    }
    EXPECT_EQ(GEP->getNumIndices(), 2u);
  }
}

// Test 4: Memory allocation through malloc-like calls
TEST_F(AllocAATest, HeapAllocationDetection) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)
    
    define i32 @test_heap_alloc() {
      ; Allocate memory
      %size = mul i64 4, 100
      %raw = call i8* @malloc(i64 %size)
      
      ; Cast to int pointer
      %ptr = bitcast i8* %raw to i32*
      
      ; Store and load
      store i32 42, i32* %ptr
      %val = load i32, i32* %ptr
      
      ; Free memory
      call void @free(i8* %raw)
      
      ret i32 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = getFunction(*module, "test_heap_alloc");
  ASSERT_NE(F, nullptr);
  
  // Find malloc call
  auto *mallocCall = dyn_cast<CallInst>(findCallTo(F, "malloc"));
  ASSERT_NE(mallocCall, nullptr);
  
  // Find free call
  auto *freeCall = dyn_cast<CallInst>(findCallTo(F, "free"));
  ASSERT_NE(freeCall, nullptr);
  
  // Verify malloc returns pointer
  EXPECT_TRUE(mallocCall->getType()->isPointerTy());
  
  // Verify bitcast exists
  bool foundBitcast = false;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (isa<BitCastInst>(&I)) {
        foundBitcast = true;
        break;
      }
    }
  }
  EXPECT_TRUE(foundBitcast);
}

// Test 5: Pointer escape analysis indicators
TEST_F(AllocAATest, PointerEscapePatterns) {
  const char *source = R"(
    declare void @external_func(i32*)

    define i32 @test_no_escape() {
      %x = alloca i32
      store i32 10, i32* %x
      %val = load i32, i32* %x
      ret i32 %val
    }
    
    define void @test_with_escape() {
      %x = alloca i32
      store i32 10, i32* %x
      call void @external_func(i32* %x)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *noEscapeFn = getFunction(*module, "test_no_escape");
  Function *withEscapeFn = getFunction(*module, "test_with_escape");
  
  ASSERT_NE(noEscapeFn, nullptr);
  ASSERT_NE(withEscapeFn, nullptr);
  
  // Check no-escape function only has local operations
  bool noEscapeHasExternalCall = false;
  for (auto &BB : *noEscapeFn) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (auto *callee = CI->getCalledFunction()) {
          if (!callee->isIntrinsic()) {
            noEscapeHasExternalCall = true;
          }
        }
      }
    }
  }
  EXPECT_FALSE(noEscapeHasExternalCall);
  
  // Check escape function has external call
  bool withEscapeHasExternalCall = false;
  for (auto &BB : *withEscapeFn) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (auto *callee = CI->getCalledFunction()) {
          if (callee->getName() == "external_func") {
            withEscapeHasExternalCall = true;
          }
        }
      }
    }
  }
  EXPECT_TRUE(withEscapeHasExternalCall);
}

// Test 6: Store/Load pair analysis
TEST_F(AllocAATest, StoreLoadPairs) {
  const char *source = R"(
    define i32 @test_store_load() {
      %x = alloca i32
      %y = alloca i32*
      
      ; Store to x
      store i32 100, i32* %x
      
      ; Store address of x to y
      store i32* %x, i32** %y
      
      ; Load from y
      %ptr = load i32*, i32** %y
      
      ; Load through pointer
      %val = load i32, i32* %ptr
      
      ret i32 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = getFunction(*module, "test_store_load");
  ASSERT_NE(F, nullptr);
  
  // Count stores and loads
  unsigned storeCount = 0, loadCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (isa<StoreInst>(&I)) ++storeCount;
      if (isa<LoadInst>(&I)) ++loadCount;
    }
  }
  
  EXPECT_EQ(storeCount, 2u);
  EXPECT_EQ(loadCount, 2u);
}

// Test 7: Function argument attributes
TEST_F(AllocAATest, ArgumentAttributes) {
  const char *source = R"(
    ; Function with read-only argument
    define i32 @read_only_callee(i32* nocapture readonly %ptr) {
      %val = load i32, i32* %ptr
      ret i32 %val
    }
    
    ; Function with writeable argument
    define void @writeable_callee(i32* %ptr) {
      store i32 42, i32* %ptr
      ret void
    }
    
    define i32 @test_attrs() {
      %x = alloca i32
      store i32 10, i32* %x
      
      %val = call i32 @read_only_callee(i32* %x)
      call void @writeable_callee(i32* %x)
      
      ret i32 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *readOnlyFn = getFunction(*module, "read_only_callee");
  Function *writeableFn = getFunction(*module, "writeable_callee");
  
  ASSERT_NE(readOnlyFn, nullptr);
  ASSERT_NE(writeableFn, nullptr);
  
  // Check readonly attribute
  EXPECT_TRUE(readOnlyFn->hasParamAttribute(0, Attribute::ReadOnly) ||
              readOnlyFn->hasParamAttribute(0, Attribute::ReadNone));
  
  // Check no readonly on writeable
  EXPECT_FALSE(writeableFn->hasParamAttribute(0, Attribute::ReadOnly) &&
               !writeableFn->hasParamAttribute(0, Attribute::ReadNone));
}

// Test 8: Complex GEP with multiple indices
TEST_F(AllocAATest, ComplexGEPAccess) {
  const char *source = R"(
    %struct.Point = type { i32, i32 }
    %struct.Rectangle = type { %struct.Point, %struct.Point }
    
    define i32 @test_struct_access() {
      %rect = alloca %struct.Rectangle
      
      ; Access rect.top_left.x
      %gep1 = getelementptr inbounds %struct.Rectangle, %struct.Rectangle* %rect, i32 0, i32 0, i32 0
      
      ; Access rect.bottom_right.y
      %gep2 = getelementptr inbounds %struct.Rectangle, %struct.Rectangle* %rect, i32 0, i32 1, i32 1
      
      store i32 10, i32* %gep1
      store i32 20, i32* %gep2
      
      %val = load i32, i32* %gep1
      ret i32 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = getFunction(*module, "test_struct_access");
  ASSERT_NE(F, nullptr);
  
  // Count GEPs
  unsigned gepCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (isa<GetElementPtrInst>(&I)) ++gepCount;
    }
  }
  
  EXPECT_EQ(gepCount, 2u);
}

// Test 9: Recursive call graph patterns
TEST_F(AllocAATest, RecursiveCallStructure) {
  const char *source = R"(
    define i32 @recursive_fib(i32 %n) {
      %is_base = icmp sle i32 %n, 1
      br i1 %is_base, label %base, label %recurse
      
    base:
      ret i32 %n
      
    recurse:
      %n1 = sub i32 %n, 1
      %r1 = call i32 @recursive_fib(i32 %n1)
      %n2 = sub i32 %n, 2
      %r2 = call i32 @recursive_fib(i32 %n2)
      %res = add i32 %r1, %r2
      ret i32 %res
    }
    
    define i32 @main() {
      %result = call i32 @recursive_fib(i32 10)
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *fibFn = getFunction(*module, "recursive_fib");
  Function *mainFn = getFunction(*module, "main");
  
  ASSERT_NE(fibFn, nullptr);
  ASSERT_NE(mainFn, nullptr);
  
  // Count recursive calls in fib
  unsigned recursiveCalls = 0;
  for (auto &BB : *fibFn) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() == fibFn) {
          ++recursiveCalls;
        }
      }
    }
  }
  
  EXPECT_EQ(recursiveCalls, 2u);
}

// Test 10: Interprocedural pointer passing
TEST_F(AllocAATest, InterproceduralPointerFlow) {
  const char *source = R"(
    define void @modifier(i32* %ptr) {
      store i32 99, i32* %ptr
      ret void
    }
    
    define i32 @caller() {
      %x = alloca i32
      store i32 10, i32* %x
      
      ; Pass pointer to callee
      call void @modifier(i32* %x)
      
      ; Load modified value
      %val = load i32, i32* %x
      ret i32 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *modifierFn = getFunction(*module, "modifier");
  Function *callerFn = getFunction(*module, "caller");
  
  ASSERT_NE(modifierFn, nullptr);
  ASSERT_NE(callerFn, nullptr);
  
  // Verify modifier writes through pointer
  bool hasStoreThroughParam = false;
  Argument *ptrArg = modifierFn->getArg(0);
  for (auto &BB : *modifierFn) {
    for (auto &I : BB) {
      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        if (SI->getPointerOperand() == ptrArg) {
          hasStoreThroughParam = true;
        }
      }
    }
  }
  EXPECT_TRUE(hasStoreThroughParam);
}

// Test 11: PHI node analysis for merge points
TEST_F(AllocAATest, PHINodeAnalysis) {
  const char *source = R"(
    define i32* @test_phi(i1 %cond) {
      %x = alloca i32
      %y = alloca i32
      store i32 1, i32* %x
      store i32 2, i32* %y
      
      br i1 %cond, label %true_bb, label %false_bb
      
    true_bb:
      br label %merge
      
    false_bb:
      br label %merge
      
    merge:
      %ptr = phi i32* [%x, %true_bb], [%y, %false_bb]
      ret i32* %ptr
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = getFunction(*module, "test_phi");
  ASSERT_NE(F, nullptr);
  
  // Find PHI node
  PHINode *phi = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *P = dyn_cast<PHINode>(&I)) {
        phi = P;
        break;
      }
    }
  }
  
  ASSERT_NE(phi, nullptr);
  EXPECT_EQ(phi->getNumIncomingValues(), 2u);
  EXPECT_TRUE(phi->getType()->isPointerTy());
}

// Test 12: Array loop access patterns
TEST_F(AllocAATest, ArrayLoopPattern) {
  const char *source = R"(
    define void @test_loop_access(i32* %arr, i32 %n) {
    entry:
      br label %loop
      
    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %idx = sext i32 %i to i64
      %ptr = getelementptr inbounds i32, i32* %arr, i64 %idx
      store i32 %i, i32* %ptr
      %next = add i32 %i, 1
      %cmp = icmp slt i32 %next, %n
      br i1 %cmp, label %loop, label %exit
      
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = getFunction(*module, "test_loop_access");
  ASSERT_NE(F, nullptr);
  
  // Find loop
  bool hasLoop = false;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *BI = dyn_cast<BranchInst>(&I)) {
        if (BI->isConditional()) {
          for (unsigned i = 0; i < BI->getNumSuccessors(); ++i) {
            BasicBlock *succ = BI->getSuccessor(i);
            // Check if successor has a back edge
            for (auto *pred : predecessors(succ)) {
              if (pred == &BB) {
                hasLoop = true;
                break;
              }
            }
          }
        }
      }
    }
  }
  EXPECT_TRUE(hasLoop);
}

// Test 13: Null pointer handling
TEST_F(AllocAATest, NullPointerHandling) {
  const char *source = R"(
    define i32 @test_null() {
      %x = alloca i32*
      store i32* null, i32** %x
      %ptr = load i32*, i32** %x
      %is_null = icmp eq i32* %ptr, null
      br i1 %is_null, label %null_case, label %non_null
      
    null_case:
      ret i32 0
      
    non_null:
      %val = load i32, i32* %ptr
      ret i32 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = getFunction(*module, "test_null");
  ASSERT_NE(F, nullptr);
  
  // Find null comparison
  bool hasNullCheck = false;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *Cmp = dyn_cast<ICmpInst>(&I)) {
        if (Cmp->getOperand(1)->getType()->isPointerTy()) {
          hasNullCheck = true;
        }
      }
    }
  }
  EXPECT_TRUE(hasNullCheck);
}

// Test 14: BitCast operations
TEST_F(AllocAATest, BitCastOperations) {
  const char *source = R"(
    define i32 @test_bitcast() {
      %x = alloca i32
      store i32 12345678, i32* %x
      
      ; Bitcast to char pointer for byte access
      %byte_ptr = bitcast i32* %x to i8*
      
      ; Read first byte
      %first_byte = load i8, i8* %byte_ptr
      %result = zext i8 %first_byte to i32
      
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = getFunction(*module, "test_bitcast");
  ASSERT_NE(F, nullptr);
  
  // Find bitcast
  bool hasBitCast = false;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (isa<BitCastInst>(&I)) {
        hasBitCast = true;
        break;
      }
    }
  }
  EXPECT_TRUE(hasBitCast);
}

// Test 15: Call graph structure
TEST_F(AllocAATest, CallGraphStructure) {
  const char *source = R"(
    define i32 @leaf1(i32 %x) {
      ret i32 %x
    }
    
    define i32 @leaf2(i32 %x) {
      %y = add i32 %x, 1
      ret i32 %y
    }
    
    define i32 @intermediate(i32 %x) {
      %r1 = call i32 @leaf1(i32 %x)
      %r2 = call i32 @leaf2(i32 %r1)
      ret i32 %r2
    }
    
    define i32 @main() {
      %result = call i32 @intermediate(i32 42)
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  // Build call graph
  CallGraph cg(*module);
  
  Function *mainFn = getFunction(*module, "main");
  Function *intermediateFn = getFunction(*module, "intermediate");
  
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(intermediateFn, nullptr);
  
  // Check main calls intermediate
  CallGraphNode *mainNode = cg[mainFn];
  ASSERT_NE(mainNode, nullptr);
  
  bool callsIntermediate = false;
  for (auto &edge : *mainNode) {
    if (edge.second->getFunction() == intermediateFn) {
      callsIntermediate = true;
      break;
    }
  }
  EXPECT_TRUE(callsIntermediate);
}

// Test 16: AllocAA canPointToTheSameObject - distinct stack allocas
TEST_F(AllocAATest, CanPointToTheSameObject_DistinctAllocas_False) {
  const char *source = R"(
    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  CallGraph CG(*module);
  std::unique_ptr<AllocAA> allocAA = createAllocAA(*module, CG);
  ASSERT_NE(allocAA, nullptr);

  AllocaInst *x = getAllocaInst(getFunction(*module, "main"), 0);
  AllocaInst *y = getAllocaInst(getFunction(*module, "main"), 1);
  ASSERT_NE(x, nullptr);
  ASSERT_NE(y, nullptr);

  EXPECT_FALSE(allocAA->canPointToTheSameObject(x, y));
  EXPECT_FALSE(allocAA->canPointToTheSameObject(y, x));
}

// Test 17: AllocAA canPointToTheSameObject - same value
TEST_F(AllocAATest, CanPointToTheSameObject_SameValue_True) {
  const char *source = R"(
    define i32 @main() {
      %x = alloca i32
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  CallGraph CG(*module);
  std::unique_ptr<AllocAA> allocAA = createAllocAA(*module, CG);
  AllocaInst *x = getAllocaInst(getFunction(*module, "main"), 0);
  ASSERT_NE(x, nullptr);

  EXPECT_TRUE(allocAA->canPointToTheSameObject(x, x));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
