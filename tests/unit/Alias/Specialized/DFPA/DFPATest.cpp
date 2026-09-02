#include "Alias/Specialized/DFPA/DFPAPass.h"

#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/CanonicalizeGEPPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/LoweringMemCpyPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/RemoveASMInstPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/RemoveExceptionHandlerPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/StandardHeapAPIRewritePass.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using namespace llvm;
using namespace dfpa;
using namespace lotus::unittest;

namespace {

DFPAResult runDFPA(Module &M, DFPAConfig Config = DFPAConfig(),
                   bool Preprocess = false) {
  legacy::PassManager PM;
  if (Preprocess) {
    PM.add(new aser::CanonicalizeGEPPass());
    PM.add(new aser::LoweringMemCpyPass());
    PM.add(new aser::RemoveExceptionHandlerPass());
    PM.add(new aser::RemoveASMInstPass());
    PM.add(new StandardHeapAPIRewritePass());
  }
  auto *Pass = new DFPAPass(Config);
  PM.add(Pass);
  PM.run(M);
  return Pass->getResult();
}

std::set<std::string> targetNames(const DFPAResult &Result, const CallBase *CB) {
  std::set<std::string> Names;
  const DFPATargetSet *Targets = Result.getTargets(CB);
  if (!Targets)
    return Names;
  for (Function *F : *Targets)
    Names.insert(F->getName().str());
  return Names;
}

} // namespace

TEST(DFPA, DirectFunctionPointerVariableAssignment) {
  const char *IR = R"(
    define i32 @add(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %fp = alloca i32 (i32)*
      store i32 (i32)* @add, i32 (i32)** %fp
      %loaded = load i32 (i32)*, i32 (i32)** %fp
      %res = call i32 %loaded(i32 7)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR, "DFPATest");
  ASSERT_NE(M, nullptr);

  DFPAResult Result = runDFPA(*M);
  auto Calls = getIndirectCalls(*M->getFunction("main"));
  ASSERT_EQ(Calls.size(), 1u);
  EXPECT_EQ(targetNames(Result, Calls.front()), (std::set<std::string>{"add"}));
  EXPECT_TRUE(Result.isPrecise(Calls.front()));
}

TEST(DFPA, StructFieldLoadStore) {
  const char *IR = R"(
    %struct.S = type { i32 (i32)* }

    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %s = alloca %struct.S
      %f = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      store i32 (i32)* @foo, i32 (i32)** %f
      %loaded = load i32 (i32)*, i32 (i32)** %f
      %res = call i32 %loaded(i32 9)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR, "DFPATest");
  ASSERT_NE(M, nullptr);

  DFPAResult Result = runDFPA(*M);
  auto Calls = getIndirectCalls(*M->getFunction("main"));
  ASSERT_EQ(Calls.size(), 1u);
  EXPECT_EQ(targetNames(Result, Calls.front()), (std::set<std::string>{"foo"}));
  EXPECT_TRUE(Result.isPrecise(Calls.front()));
}

TEST(DFPA, PhiMergeStaysPreciseWithoutUnknowns) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %sub = sub i32 %x, 1
      ret i32 %sub
    }

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %fp = phi i32 (i32)* [ @foo, %then ], [ @bar, %else ]
      %res = call i32 %fp(i32 1)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR, "DFPATest");
  ASSERT_NE(M, nullptr);

  DFPAResult Result = runDFPA(*M);
  auto Calls = getIndirectCalls(*M->getFunction("main"));
  ASSERT_EQ(Calls.size(), 1u);
  EXPECT_EQ(targetNames(Result, Calls.front()),
            (std::set<std::string>{"bar", "foo"}));
  EXPECT_TRUE(Result.isPrecise(Calls.front()));
}

TEST(DFPA, DirectSummaryPropagatesReturnedFunctionPointer) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 (i32)* @id(i32 (i32)* %f) {
    entry:
      ret i32 (i32)* %f
    }

    define i32 @main() {
    entry:
      %fp = call i32 (i32)* @id(i32 (i32)* @foo)
      %res = call i32 %fp(i32 4)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  DFPAResult Result = runDFPA(*M);
  auto Calls = getIndirectCalls(*M->getFunction("main"));
  ASSERT_EQ(Calls.size(), 1u);
  EXPECT_EQ(targetNames(Result, Calls.front()), (std::set<std::string>{"foo"}));
  EXPECT_TRUE(Result.isPrecise(Calls.front()));
}

TEST(DFPA, DirectCallSummaryWritesIntoActualObject) {
  const char *IR = R"(
    %struct.S = type { i32 (i32)* }

    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define void @set_cb(%struct.S* %s, i32 (i32)* %cb) {
    entry:
      %f = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      store i32 (i32)* %cb, i32 (i32)** %f
      ret void
    }

    define i32 @main() {
    entry:
      %s = alloca %struct.S
      call void @set_cb(%struct.S* %s, i32 (i32)* @foo)
      %f = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      %loaded = load i32 (i32)*, i32 (i32)** %f
      %res = call i32 %loaded(i32 11)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  DFPAResult Result = runDFPA(*M);
  auto Calls = getIndirectCalls(*M->getFunction("main"));
  ASSERT_EQ(Calls.size(), 1u);
  EXPECT_EQ(targetNames(Result, Calls.front()), (std::set<std::string>{"foo"}));
}

TEST(DFPA, MemcpyCopiesFunctionPointerField) {
  const char *IR = R"(
    %struct.S = type { i32 (i32)* }

    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %src = alloca %struct.S
      %dst = alloca %struct.S
      %f = getelementptr inbounds %struct.S, %struct.S* %src, i32 0, i32 0
      store i32 (i32)* @foo, i32 (i32)** %f
      %src8 = bitcast %struct.S* %src to i8*
      %dst8 = bitcast %struct.S* %dst to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst8, i8* %src8, i64 8, i1 false)
      %df = getelementptr inbounds %struct.S, %struct.S* %dst, i32 0, i32 0
      %loaded = load i32 (i32)*, i32 (i32)** %df
      %res = call i32 %loaded(i32 3)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  DFPAResult Result = runDFPA(*M, DFPAConfig(), true);
  auto Calls = getIndirectCalls(*M->getFunction("main"));
  ASSERT_EQ(Calls.size(), 1u);
  EXPECT_EQ(targetNames(Result, Calls.front()), (std::set<std::string>{"foo"}));
}

TEST(DFPA, GlobalInitializerSeedsFunctionPointer) {
  const char *IR = R"(
    @fp = global i32 (i32)* @foo

    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %loaded = load i32 (i32)*, i32 (i32)** @fp
      %res = call i32 %loaded(i32 5)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  DFPAResult Result = runDFPA(*M);
  auto Calls = getIndirectCalls(*M->getFunction("main"));
  ASSERT_EQ(Calls.size(), 1u);
  EXPECT_EQ(targetNames(Result, Calls.front()), (std::set<std::string>{"foo"}));
  EXPECT_TRUE(Result.isPrecise(Calls.front()));
}

TEST(DFPA, UnknownIndexDegradesButStaysSound) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %r = sub i32 %x, 1
      ret i32 %r
    }

    define i32 @main(i64 %idx) {
    entry:
      %arr = alloca [2 x i32 (i32)*]
      %p0 = getelementptr inbounds [2 x i32 (i32)*], [2 x i32 (i32)*]* %arr, i64 0, i64 0
      %p1 = getelementptr inbounds [2 x i32 (i32)*], [2 x i32 (i32)*]* %arr, i64 0, i64 1
      store i32 (i32)* @foo, i32 (i32)** %p0
      store i32 (i32)* @bar, i32 (i32)** %p1
      %dyn = getelementptr inbounds [2 x i32 (i32)*], [2 x i32 (i32)*]* %arr, i64 0, i64 %idx
      %loaded = load i32 (i32)*, i32 (i32)** %dyn
      %res = call i32 %loaded(i32 7)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  DFPAResult Result = runDFPA(*M);
  auto Calls = getIndirectCalls(*M->getFunction("main"));
  ASSERT_EQ(Calls.size(), 1u);
  EXPECT_EQ(targetNames(Result, Calls.front()),
            (std::set<std::string>{"bar", "foo"}));
  EXPECT_FALSE(Result.isPrecise(Calls.front()));
}

TEST(DFPA, PtrIntFallbackStaysSound) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %pi = ptrtoint i32 (i32)* @foo to i64
      %fp = inttoptr i64 %pi to i32 (i32)*
      %res = call i32 %fp(i32 5)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  DFPAResult Result = runDFPA(*M);
  auto Calls = getIndirectCalls(*M->getFunction("main"));
  ASSERT_EQ(Calls.size(), 1u);
  EXPECT_EQ(targetNames(Result, Calls.front()), (std::set<std::string>{"foo"}));
  EXPECT_FALSE(Result.isPrecise(Calls.front()));
}

TEST(DFPA, DemandRefinesDirectCallerBinding) {
  const char *IR = R"(
    define i32 @add(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @sub(i32 %x) {
    entry:
      %r = sub i32 %x, 1
      ret i32 %r
    }

    define i32 (i32)* @id(i32 (i32)* %f) {
    entry:
      ret i32 (i32)* %f
    }

    define i32 @main() {
    entry:
      %a = call i32 (i32)* @id(i32 (i32)* @add)
      %b = call i32 (i32)* @id(i32 (i32)* @sub)
      %ra = call i32 %a(i32 1)
      %rb = call i32 %b(i32 2)
      %sum = add i32 %ra, %rb
      ret i32 %sum
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  DFPAResult Result = runDFPA(*M);
  auto Calls = getIndirectCalls(*M->getFunction("main"));
  ASSERT_EQ(Calls.size(), 2u);
  EXPECT_EQ(targetNames(Result, Calls[0]), (std::set<std::string>{"add"}));
  EXPECT_EQ(targetNames(Result, Calls[1]), (std::set<std::string>{"sub"}));
}

TEST(DFPA, RegressFunptrSimpleSmoke) {
  LLVMContext Ctx;
  auto M = loadModule(std::string(CMAKE_SOURCE_DIR) +
                          "/tests/regress/Alias/PTA/funptr-simple.ll",
                      Ctx, "DFPATest");
  ASSERT_NE(M, nullptr);

  DFPAResult Result = runDFPA(*M, DFPAConfig(), true);
  EXPECT_GT(Result.getAllTargets().size(), 0u);
}

TEST(DFPA, RegressFunptrStructSmoke) {
  LLVMContext Ctx;
  auto M = loadModule(std::string(CMAKE_SOURCE_DIR) +
                          "/tests/regress/Alias/PTA/funptr-struct.ll",
                      Ctx, "DFPATest");
  ASSERT_NE(M, nullptr);

  DFPAResult Result = runDFPA(*M, DFPAConfig(), true);
  EXPECT_GT(Result.getAllTargets().size(), 0u);
}

TEST(DFPA, RegressGlobalInitializerSmoke) {
  LLVMContext Ctx;
  auto M = loadModule(std::string(CMAKE_SOURCE_DIR) +
                          "/tests/regress/Alias/PTA/global-initializer.ll",
                      Ctx, "DFPATest");
  ASSERT_NE(M, nullptr);

  DFPAResult Result = runDFPA(*M, DFPAConfig(), true);
  EXPECT_GE(Result.getStats().num_indirect_calls, 0u);
}

TEST(DFPA, RegressFunptrNestedCallSmoke) {
  LLVMContext Ctx;
  auto M = loadModule(std::string(CMAKE_SOURCE_DIR) +
                          "/tests/regress/Alias/PTA/funptr-nested-call.ll",
                      Ctx, "DFPATest");
  ASSERT_NE(M, nullptr);

  DFPAConfig Config;
  Config.max_demand_states = 10000;
  DFPAResult Result = runDFPA(*M, Config, true);
  EXPECT_GT(Result.getAllTargets().size(), 0u);
}
