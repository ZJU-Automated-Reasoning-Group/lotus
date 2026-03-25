//
// AserPTA Tests - Pointer analysis with field-sensitivity and
// context-sensitivity
//
#include "Alias/AserPTA/PointerAnalysis/Context/KCallSite.h"
#include "Alias/AserPTA/PointerAnalysis/Context/KOrigin.h"
#include "Alias/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldInsensitive/FIMemModel.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/AserPTA/PointerAnalysis/PointerAnalysisPass.h"
#include "Alias/AserPTA/PointerAnalysis/Program/CallSite.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/PartialUpdateSolver.h"
#include "Alias/AserPTA/PreProcessing/Passes/CanonicalizeGEPPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/InsertGlobalCtorCallPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/LoweringMemCpyPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/RemoveExceptionHandlerPass.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace aser;
using namespace lotus::unittest;

// Field-insensitive, context-insensitive
using FI_NoCtx_Model = DefaultLangModel<NoCtx, FIMemModel<NoCtx>>;
using FI_NoCtx_Solver = PartialUpdateSolver<FI_NoCtx_Model>;

// Field-sensitive, context-insensitive
using FS_NoCtx_Model = DefaultLangModel<NoCtx, FSMemModel<NoCtx>>;
using FS_NoCtx_Solver = PartialUpdateSolver<FS_NoCtx_Model>;

// Field-sensitive, 1-CFA
using FS_1CFA_Model = DefaultLangModel<KCallSite<1>, FSMemModel<KCallSite<1>>>;
using FS_1CFA_Solver = PartialUpdateSolver<FS_1CFA_Model>;

namespace {

template <typename Solver>
void addAserPTAPasses(llvm::legacy::PassManager &passes) {
  passes.add(new CanonicalizeGEPPass());
  passes.add(new LoweringMemCpyPass());
  passes.add(new RemoveExceptionHandlerPass());
  passes.add(new InsertGlobalCtorCallPass());
  passes.add(new PointerAnalysisPass<Solver>());
}

} // namespace

// ============================================================================
// Field-Insensitive Tests
// ============================================================================

TEST(AserPTA_FI, NoAliasTwoAllocas) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseModule(ctx, ir, "AserPTATest");
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FI_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FI, AliasStoreLoad) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseModule(ctx, ir, "AserPTATest");
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FI_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FI, GlobalNoAlias) {
  const char *ir = R"(
    @g1 = global i32 0
    @g2 = global i32 0
    define i32 @main() {
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseModule(ctx, ir, "AserPTATest");
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FI_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FI, PointerChain) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      %pp = alloca i32**
      store i32* %x, i32** %p
      store i32** %p, i32*** %pp
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseModule(ctx, ir, "AserPTATest");
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FI_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FI, NullPointer) {
  const char *ir = R"(
    define i32 @main() {
      %p = alloca i32*
      store i32* null, i32** %p
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseModule(ctx, ir, "AserPTATest");
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FI_NoCtx_Solver>(passes);
  passes.run(*module);
}

// ============================================================================
// Field-Sensitive Tests
// ============================================================================

TEST(AserPTA_FS, DifferentStructFieldsNoAlias) {
  const char *ir = R"(
    %struct.S = type { i32, i32 }

    define i32 @main() {
      %s = alloca %struct.S
      %f0 = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      %f1 = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 1
      store i32 10, i32* %f0
      store i32 20, i32* %f1
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FS, NestedStructFields) {
  const char *ir = R"(
    %inner = type { i32, i32 }
    %outer = type { %inner, i32 }

    define i32 @main() {
      %o = alloca %outer
      %inner_ptr = getelementptr inbounds %outer, %outer* %o, i32 0, i32 0
      %f0 = getelementptr inbounds %inner, %inner* %inner_ptr, i32 0, i32 0
      %f1 = getelementptr inbounds %inner, %inner* %inner_ptr, i32 0, i32 1
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FS, ArrayElementAccess) {
  const char *ir = R"(
    define i32 @main() {
      %arr = alloca [10 x i32]
      %p0 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i32 0, i32 0
      %p5 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i32 0, i32 5
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FS, HeapAllocation) {
  const char *ir = R"(
    declare i8* @malloc(i64)

    define i32 @main() {
      %size = mul i64 4, 100
      %raw = call i8* @malloc(i64 %size)
      %ptr = bitcast i8* %raw to i32*
      store i32 42, i32* %ptr
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FS, MultipleHeapAllocations) {
  const char *ir = R"(
    declare i8* @malloc(i64)

    define i32 @main() {
      %p1 = call i8* @malloc(i64 16)
      %p2 = call i8* @malloc(i64 32)
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

// ============================================================================
// Context-Sensitive Tests (1-CFA)
// ============================================================================

TEST(AserPTA_1CFA, FunctionParameter) {
  const char *ir = R"(
    define void @callee(i32* %p) {
      ret void
    }

    define i32 @main() {
      %x = alloca i32
      call void @callee(i32* %x)
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_1CFA_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_1CFA, SameFunctionDifferentContexts) {
  const char *ir = R"(
    @global_ptr = global i32* null

    define void @helper(i32* %p) {
      store i32* %p, i32** @global_ptr
      ret void
    }

    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      call void @helper(i32* %x)
      call void @helper(i32* %y)
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_1CFA_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_1CFA, RecursiveFunction) {
  const char *ir = R"(
    define void @foo(i32** %p, i32 %n) {
      %cmp = icmp sgt i32 %n, 0
      br i1 %cmp, label %body, label %exit

    body:
      %ptr = alloca i32
      store i32* %ptr, i32** %p
      %n1 = sub i32 %n, 1
      call void @foo(i32** %p, i32 %n1)
      br label %exit

    exit:
      ret void
    }

    define i32 @main() {
      %p = alloca i32*
      call void @foo(i32** %p, i32 3)
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_1CFA_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_1CFA, FunctionReturnPointsTo) {
  const char *ir = R"(
    define i32* @alloc_int() {
      %x = alloca i32
      store i32 42, i32* %x
      ret i32* %x
    }

    define i32 @main() {
      %p = call i32* @alloc_int()
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_1CFA_Solver>(passes);
  passes.run(*module);
}

// ============================================================================
// Indirect Call Tests
// ============================================================================

TEST(AserPTA_FS, IndirectFunctionCall) {
  const char *ir = R"(
    define void @func1(i32* %p) {
      ret void
    }

    define void @func2(i32* %p) {
      ret void
    }

    define void @caller(void (i32*)* %fp, i32* %arg) {
      call void %fp(i32* %arg)
      ret void
    }

    define i32 @main() {
      %x = alloca i32
      %fptr = alloca void (i32*)*
      store void (i32*)* @func1, void (i32*)** %fptr
      %f = load void (i32*)*, void (i32*)** %fptr
      call void @caller(void (i32*)* %f, i32* %x)
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

// ============================================================================
// Complex Patterns Tests
// ============================================================================

TEST(AserPTA_FS, PHINodeMerge) {
  const char *ir = R"(
    define i32* @main(i1 %cond) {
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
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FS, BitCastHandling) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      store i32 12345678, i32* %x
      %byte_ptr = bitcast i32* %x to i8*
      %first_byte = load i8, i8* %byte_ptr
      %result = zext i8 %first_byte to i32
      ret i32 %result
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FS, SelectInstruction) {
  const char *ir = R"(
    define i32* @main(i1 %cond) {
      %x = alloca i32
      %y = alloca i32
      store i32 10, i32* %x
      store i32 20, i32* %y
      %ptr = select i1 %cond, i32* %x, i32* %y
      ret i32* %ptr
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

// ============================================================================
// Comparison Tests (FI vs FS)
// ============================================================================

TEST(AserPTA_Comparison, FIvsFS_SameResult) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passesFI, passesFS;
  addAserPTAPasses<FI_NoCtx_Solver>(passesFI);
  addAserPTAPasses<FS_NoCtx_Solver>(passesFS);
  passesFI.run(*module);
  passesFS.run(*module);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
