#include "Alias/TypeQualifier/Config.h"
#include "Alias/TypeQualifier/FunctionSummary.h"
#include "Alias/TypeQualifier/QualifierAnalysis.h"
#include "Alias/TypeQualifier/QualifierTypes.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

#include <limits>

using namespace lotus::unittest;

TEST(TypeQualifier, DomainJoinPrefersUninitializedOverUnknown) {
  EXPECT_EQ(QualifierDomain::join(QualifierState::Initialized,
                                  QualifierState::Unknown),
            QualifierState::Unknown);
  EXPECT_EQ(QualifierDomain::join(QualifierState::Unknown,
                                  QualifierState::Uninitialized),
            QualifierState::Uninitialized);
  EXPECT_EQ(QualifierDomain::legacyMin(QualifierState::Initialized,
                                       QualifierState::Uninitialized),
            QualifierState::Uninitialized);
}

TEST(TypeQualifier, RegistryClassifiesKnownModels) {
  EXPECT_EQ(FunctionModelRegistry::lookup("malloc").kind,
            FunctionModelKind::Allocator);
  EXPECT_EQ(FunctionModelRegistry::lookup("kzalloc").kind,
            FunctionModelKind::ZeroAllocator);
  EXPECT_EQ(FunctionModelRegistry::lookup("llvm.memcpy.p0i8.p0i8.i64").kind,
            FunctionModelKind::Copy);
  EXPECT_EQ(FunctionModelRegistry::lookup("llvm.dbg.value").kind,
            FunctionModelKind::Ignore);
  EXPECT_EQ(FunctionModelRegistry::lookup("printf").kind,
            FunctionModelKind::Passthrough);
}

TEST(TypeQualifier, RegistryInitializesLegacySetsOnce) {
  GlobalContext ctx;
  initializeFunctionModelSets(ctx);

  EXPECT_TRUE(ctx.functionModelsInitialized);
  EXPECT_TRUE(ctx.HeapAllocFuncs.count("malloc"));
  EXPECT_TRUE(ctx.ZeroMallocFuncs.count("kzalloc"));
  EXPECT_TRUE(ctx.CopyFuncs.count("memcpy"));
  EXPECT_TRUE(ctx.TransferFuncs.count("copy_to_user"));
  EXPECT_TRUE(ctx.InitFuncs.count("memset"));
  EXPECT_TRUE(ctx.OtherFuncs.count("llvm.dbg.value"));
  EXPECT_TRUE(ctx.ObjSizeFuncs.count("llvm.objectsize.i64.p0i8"));
}

TEST(TypeQualifier, SummaryAccessorsExposeTypedStates) {
  Summary summary;
  summary.noNodes = 2;
  summary.reqVec.resize(2);
  summary.updateVec.resize(2);

  summary.setRequiredState(1, QualifierState::Initialized);
  summary.setUpdatedState(0, QualifierState::Uninitialized);

  EXPECT_EQ(summary.requiredState(1), QualifierState::Initialized);
  EXPECT_EQ(summary.returnState(), QualifierState::Uninitialized);
}

TEST(TypeQualifier, FuncAnalysisRunsOnDirectModeledAllocator) {
  const char *ir = R"IR(
    declare i8* @malloc(i64)

    define i32 @main() {
      %raw = call i8* @malloc(i64 16)
      %ptr = bitcast i8* %raw to i32*
      ret i32 0
    }
  )IR";

  llvm::LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  GlobalContext gc;
  FuncAnalysis analysis(module->getFunction("main"), &gc, false);
  EXPECT_FALSE(analysis.run());
}

TEST(TypeQualifier, FuncAnalysisRunsOnIndirectModeledCopyTargets) {
  const char *ir = R"IR(
    declare i8* @memcpy(i8*, i8*, i64)
    declare i8* @memmove(i8*, i8*, i64)

    define i32 @main(i1 %cond) {
    entry:
      %fp = select i1 %cond,
                   i8* (i8*, i8*, i64)* @memcpy,
                   i8* (i8*, i8*, i64)* @memmove
      %dst = alloca [8 x i8]
      %src = alloca [8 x i8]
      %dst0 = getelementptr inbounds [8 x i8], [8 x i8]* %dst, i32 0, i32 0
      %src0 = getelementptr inbounds [8 x i8], [8 x i8]* %src, i32 0, i32 0
      %call = call i8* %fp(i8* %dst0, i8* %src0, i64 8)
      ret i32 0
    }
  )IR";

  llvm::LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  auto *call = llvm::dyn_cast<llvm::CallInst>(findIndirectCall(*mainFn));
  ASSERT_NE(call, nullptr);

  GlobalContext gc;
  gc.Callees[const_cast<llvm::CallInst *>(call)].insert(
      module->getFunction("memcpy"));
  gc.Callees[const_cast<llvm::CallInst *>(call)].insert(
      module->getFunction("memmove"));

  FuncAnalysis analysis(mainFn, &gc, false);
  EXPECT_FALSE(analysis.run());
}

TEST(TypeQualifier, BackwardRequirednessMarksLoadedArgumentRequiredAtEntry) {
  const char *ir = R"IR(
    define i32 @read_arg(i32* %p) {
    entry:
      %x = load i32, i32* %p
      ret i32 %x
    }
  )IR";

  llvm::LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  auto *func = module->getFunction("read_arg");
  ASSERT_NE(func, nullptr);

  GlobalContext gc;
  FuncAnalysis analysis(func, &gc, false);
  EXPECT_FALSE(analysis.run());

  llvm::Argument *arg = func->getArg(0);
  const Summary &summary = analysis.getSummary();
  NodeIndex sumArgNode = summary.sumNodeFactory.getValueNodeFor(arg);
  ASSERT_NE(sumArgNode, std::numeric_limits<unsigned int>::max());
  EXPECT_EQ(summary.requiredInputState(sumArgNode), QualifierState::Initialized);

  NodeIndex sumArgObj = summary.sumNodeFactory.getObjectNodeFor(arg);
  ASSERT_NE(sumArgObj, std::numeric_limits<unsigned int>::max());
  EXPECT_EQ(summary.requiredInputState(sumArgObj), QualifierState::Initialized);
}

TEST(TypeQualifier, BackwardRequirednessMarksBranchConditionRequired) {
  const char *ir = R"IR(
    define i32 @branch_on_arg(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret i32 1
    else:
      ret i32 0
    }
  )IR";

  llvm::LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  auto *func = module->getFunction("branch_on_arg");
  ASSERT_NE(func, nullptr);

  GlobalContext gc;
  FuncAnalysis analysis(func, &gc, false);
  EXPECT_FALSE(analysis.run());

  llvm::Argument *arg = func->getArg(0);
  NodeIndex entryArgNode = analysis.getSummary().sumNodeFactory.getValueNodeFor(arg);
  ASSERT_NE(entryArgNode, std::numeric_limits<unsigned int>::max());
  EXPECT_EQ(analysis.getSummary().requiredInputState(entryArgNode),
            QualifierState::Initialized);
}

TEST(TypeQualifier, InterproceduralRequirednessPropagatesThroughSummary) {
  const char *ir = R"IR(
    define void @sink(i32* %p) {
    entry:
      %x = load i32, i32* %p
      ret void
    }

    define void @caller(i32* %p) {
    entry:
      call void @sink(i32* %p)
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  GlobalContext gc;
  auto *sink = module->getFunction("sink");
  auto *caller = module->getFunction("caller");
  ASSERT_NE(sink, nullptr);
  ASSERT_NE(caller, nullptr);

  FuncAnalysis sinkAnalysis(sink, &gc, false);
  EXPECT_FALSE(sinkAnalysis.run());

  FuncAnalysis callerAnalysis(caller, &gc, false);
  EXPECT_FALSE(callerAnalysis.run());

  llvm::Argument *arg = caller->getArg(0);
  EXPECT_TRUE(callerAnalysis.isArgumentRequiredAtEntry(arg));
  NodeIndex sumArgNode =
      callerAnalysis.getSummary().sumNodeFactory.getValueNodeFor(arg);
  ASSERT_NE(sumArgNode, std::numeric_limits<unsigned int>::max());
  EXPECT_EQ(callerAnalysis.getSummary().requiredInputState(sumArgNode),
            QualifierState::Initialized);
}
