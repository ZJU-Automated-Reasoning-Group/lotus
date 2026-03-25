#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralTaint.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <gtest/gtest.h>

namespace {

using lotus::unittest::findBlock;
using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModule;

const llvm::BasicBlock *findBlockByName(const llvm::Function &function,
                                        llvm::StringRef name) {
  return findBlock(function, name);
}

std::string writeTempTaintConfig(llvm::StringRef content) {
  llvm::SmallString<128> tempPath;
  if (auto ec =
          llvm::sys::fs::createTemporaryFile("npa-taint", ".spec", tempPath)) {
    ADD_FAILURE() << "failed to create temp taint spec: " << ec.message();
    return "";
  }

  std::error_code ec;
  llvm::raw_fd_ostream os(tempPath, ec);
  if (ec) {
    ADD_FAILURE() << "failed to open temp taint spec: " << ec.message();
    return "";
  }
  os << content;
  os.close();
  return tempPath.str().str();
}

std::string sourceRoot() {
  llvm::SmallString<256> path(__FILE__);
  llvm::sys::path::remove_filename(path);
  for (int i = 0; i < 4; ++i)
    llvm::sys::path::remove_filename(path);
  return path.str().str();
}

std::string defaultTaintConfigPath() {
  llvm::SmallString<256> path(sourceRoot());
  llvm::sys::path::append(path, "config", "taint.spec");
  return path.str().str();
}

} // namespace

TEST(NPA, InterproceduralTaintDirectSourceSpecTaintsReturnValue) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %p = call i32 @getchar()
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *p = findInstructionByName(*mainFn, "p");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(result.isValueTainted(after, p));
}

TEST(NPA, InterproceduralTaintSameBlockQueriesUseReplayedState) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()
    declare i8* @fgets(i8*, i64, i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %x = call i32 @getchar()
      %y = add i32 %x, 1
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      ret i32 %y
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n"
                           "SOURCE fgets Arg0 D T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *entry = &mainFn->getEntryBlock();
  const auto *y = findInstructionByName(*mainFn, "y");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  ASSERT_NE(y, nullptr);
  ASSERT_NE(p0, nullptr);
  EXPECT_TRUE(result.isValueTainted(entry, y));
  EXPECT_TRUE(result.isMemoryTainted(entry, p0));
}

TEST(NPA, InterproceduralTaintIndirectSourceSpecMatchesDirectCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %fp = bitcast i32 ()* @getchar to i32 ()*
      %p = call i32 %fp()
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *p = findInstructionByName(*mainFn, "p");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(result.isValueTainted(after, p));
}

TEST(NPA, InterproceduralTaintIgnoresUninitializedOnlySources) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @malloc(i64)

    define i32 @main() {
    entry:
      %p = call i8* @malloc(i64 4)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *p = findInstructionByName(*mainFn, "p");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p, nullptr);
  EXPECT_FALSE(result.isValueTainted(after, p));
}

TEST(NPA, InterproceduralTaintUnusedUnsupportedSpecsDoNotAbortDefaultRun) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @malloc(i64)

    define i32 @main() {
    entry:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  EXPECT_FALSE(result.status.unsupported_specs);
  EXPECT_TRUE(result.status.overall_converged);
}

TEST(NPA, InterproceduralTaintStoreDoesNotBackTaintStoredValue) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)

    define i32 @main() {
    entry:
      %buf = alloca [2 x i8], align 1
      %p0 = getelementptr inbounds [2 x i8], [2 x i8]* %buf, i64 0, i64 0
      %p1 = getelementptr inbounds [2 x i8], [2 x i8]* %buf, i64 0, i64 1
      store i8 0, i8* %p1, align 1
      %call = call i8* @fgets(i8* %p0, i64 8, i8* null)
      %clean = load i8, i8* %p1, align 1
      store i8 %clean, i8* %p0, align 1
      br label %after

    after:
      %use = add i8 %clean, 1
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE fgets Ret V T\n"
                           "SOURCE fgets Arg0 D T\n"
                           "PIPE fgets Ret V Arg0 V\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *clean = findInstructionByName(*mainFn, "clean");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(clean, nullptr);
  EXPECT_FALSE(result.isValueTainted(after, clean));
}

TEST(NPA, InterproceduralTaintStrongUpdateClearsDirectMemoryTaint) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      store i8 0, i8* %p0, align 1
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath = writeTempTaintConfig("SOURCE fgets Arg0 D T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p0, nullptr);
  EXPECT_FALSE(result.isMemoryTainted(after, p0));
}

TEST(NPA, InterproceduralTaintWritesBackPointerMemoryFromCallee) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define void @write_taint(i32* %p) {
    entry:
      %t = call i32 @getchar()
      store i32 %t, i32* %p, align 4
      ret void
    }

    define i32 @main() {
    entry:
      %slot = alloca i32, align 4
      call void @write_taint(i32* %slot)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *slot = findInstructionByName(*mainFn, "slot");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(slot, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, slot));
}

TEST(NPA, InterproceduralTaintWritesBackOffsetSensitivePointerMemory) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define void @write_taint_offset(i8* %p) {
    entry:
      %t = call i32 @getchar()
      %t8 = trunc i32 %t to i8
      %q = getelementptr inbounds i8, i8* %p, i64 1
      store i8 %t8, i8* %q, align 1
      ret void
    }

    define i32 @main() {
    entry:
      %buf = alloca [2 x i8], align 1
      %p0 = getelementptr inbounds [2 x i8], [2 x i8]* %buf, i64 0, i64 0
      %p1 = getelementptr inbounds [2 x i8], [2 x i8]* %buf, i64 0, i64 1
      call void @write_taint_offset(i8* %p0)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  const auto *p1 = findInstructionByName(*mainFn, "p1");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p0, nullptr);
  ASSERT_NE(p1, nullptr);
  EXPECT_FALSE(result.isMemoryTainted(after, p0));
  EXPECT_TRUE(result.isMemoryTainted(after, p1));
}

TEST(NPA, InterproceduralTaintTracksGlobalStoreThenLoad) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @g = global i32 0
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %t = call i32 @getchar()
      store i32 %t, i32* @g
      %x = load i32, i32* @g
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *t = findInstructionByName(*mainFn, "t");
  const auto *x = findInstructionByName(*mainFn, "x");
  auto *g = module->getGlobalVariable("g");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(t, nullptr);
  ASSERT_NE(x, nullptr);
  ASSERT_NE(g, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, g));
  EXPECT_TRUE(result.isValueTainted(after, x));
}

TEST(NPA, InterproceduralTaintTracksGlobalPointerReachability) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @gp = global i8* null
    declare i8* @fgets(i8*, i64, i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      store i8* %p0, i8** @gp
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      %q = load i8*, i8** @gp
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE fgets Ret V T\n"
                           "SOURCE fgets Arg0 D T\n"
                           "PIPE fgets Ret V Arg0 V\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *q = findInstructionByName(*mainFn, "q");
  auto *gp = module->getGlobalVariable("gp");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(q, nullptr);
  ASSERT_NE(gp, nullptr);
  EXPECT_TRUE(result.isReachableMemoryTainted(after, gp));
  EXPECT_TRUE(result.isMemoryTainted(after, q));
}

TEST(NPA, InterproceduralTaintMainPointerArgsAreNotSeededByDefault) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @main(i32 %argc, i8** %argv) {
    entry:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *entry = &mainFn->getEntryBlock();
  const llvm::Argument *argv = nullptr;
  for (const auto &arg : mainFn->args()) {
    if (arg.getName() == "argv") {
      argv = &arg;
      break;
    }
  }
  ASSERT_NE(argv, nullptr);
  EXPECT_FALSE(result.isValueTainted(entry, argv));
}

TEST(NPA, InterproceduralTaintMainPointerArgsCanBeSeededExplicitly) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @main(i32 %argc, i8** %argv) {
    entry:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterproceduralTaint::Options options;
  options.seed_main_pointer_args = true;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *entry = &mainFn->getEntryBlock();
  const llvm::Argument *argv = nullptr;
  for (const auto &arg : mainFn->args()) {
    if (arg.getName() == "argv") {
      argv = &arg;
      break;
    }
  }
  ASSERT_NE(argv, nullptr);
  EXPECT_TRUE(result.isValueTainted(entry, argv));
}

TEST(NPA, InterproceduralTaintPropagatesThroughIcmpAndSelect) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %t = call i32 @getchar()
      %cmp = icmp eq i32 %t, 0
      %sel = select i1 %cmp, i32 %t, i32 1
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *cmp = findInstructionByName(*mainFn, "cmp");
  const auto *sel = findInstructionByName(*mainFn, "sel");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(cmp, nullptr);
  ASSERT_NE(sel, nullptr);
  EXPECT_TRUE(result.isValueTainted(after, cmp));
  EXPECT_TRUE(result.isValueTainted(after, sel));
}

TEST(NPA, InterproceduralTaintPropagatesThroughPhiIntoCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main(i1 %cond) {
    entry:
      %t = call i32 @getchar()
      br i1 %cond, label %left, label %right

    left:
      br label %merge

    right:
      br label %merge

    merge:
      %v = phi i32 [ %t, %left ], [ 0, %right ]
      %r = call i32 @id(i32 %v)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *r = findInstructionByName(*mainFn, "r");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_TRUE(result.isValueTainted(after, r));
}

TEST(NPA, InterproceduralTaintPropagatesThroughAggregateAndVectorOps) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %t = call i32 @getchar()
      %agg = insertvalue {i32, i32} undef, i32 %t, 0
      %x = extractvalue {i32, i32} %agg, 0
      %vec0 = insertelement <2 x i32> undef, i32 %t, i32 0
      %vec1 = shufflevector <2 x i32> %vec0, <2 x i32> undef,
                             <2 x i32> <i32 0, i32 1>
      %y = extractelement <2 x i32> %vec1, i32 0
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *agg = findInstructionByName(*mainFn, "agg");
  const auto *x = findInstructionByName(*mainFn, "x");
  const auto *vec1 = findInstructionByName(*mainFn, "vec1");
  const auto *y = findInstructionByName(*mainFn, "y");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(agg, nullptr);
  ASSERT_NE(x, nullptr);
  ASSERT_NE(vec1, nullptr);
  ASSERT_NE(y, nullptr);
  EXPECT_TRUE(result.isValueTainted(after, agg));
  EXPECT_TRUE(result.isValueTainted(after, x));
  EXPECT_TRUE(result.isValueTainted(after, vec1));
  EXPECT_TRUE(result.isValueTainted(after, y));
}

TEST(NPA, InterproceduralTaintDifferentConstantOffsetsDoNotAlias) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %p1 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 1
      %call = call i8* @fgets(i8* %p0, i64 8, i8* null)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  const auto *p1 = findInstructionByName(*mainFn, "p1");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p0, nullptr);
  ASSERT_NE(p1, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, p0));
  EXPECT_FALSE(result.isMemoryTainted(after, p1));
}

TEST(NPA, InterproceduralTaintUnknownOffsetsAliasConservatively) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)

    define i32 @main(i32 %argc, i8** %argv) {
    entry:
      %buf = alloca [8 x i8], align 1
      %idx = zext i32 %argc to i64
      %p_unk = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 %idx
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %call = call i8* @fgets(i8* %p_unk, i64 8, i8* null)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p0, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, p0));
}

TEST(NPA, InterproceduralTaintKeepsConstantOffsetThroughBitcastedGEP) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %p1 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 1
      %p1.cast = bitcast i8* %p1 to i8*
      %src = call i8* @fgets(i8* %p1.cast, i64 8, i8* null)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE fgets Ret V T\n"
                           "SOURCE fgets Arg0 D T\n"
                           "PIPE fgets Ret V Arg0 V\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  const auto *p1 = findInstructionByName(*mainFn, "p1");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p0, nullptr);
  ASSERT_NE(p1, nullptr);
  EXPECT_FALSE(result.isMemoryTainted(after, p0));
  EXPECT_TRUE(result.isMemoryTainted(after, p1));
}

TEST(NPA, InterproceduralTaintReachabilityIgnoresNullPointerSeeds) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %slot = alloca i8*, align 8
      store i8* null, i8** %slot, align 8
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE fgets Ret V T\n"
                           "SOURCE fgets Arg0 D T\n"
                           "PIPE fgets Ret V Arg0 V\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  const auto *slot = findInstructionByName(*mainFn, "slot");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p0, nullptr);
  ASSERT_NE(slot, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, p0));
  EXPECT_FALSE(result.isReachableMemoryTainted(after, slot));
}

TEST(NPA, InterproceduralTaintPointerSlotsTrackReachableButNotDirectMemory) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      %slot = alloca i8*, align 8
      store i8* %p0, i8** %slot, align 8
      %q = load i8*, i8** %slot, align 8
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath = writeTempTaintConfig("SOURCE fgets Arg0 D T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  const auto *slot = findInstructionByName(*mainFn, "slot");
  const auto *q = findInstructionByName(*mainFn, "q");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p0, nullptr);
  ASSERT_NE(slot, nullptr);
  ASSERT_NE(q, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, p0));
  EXPECT_FALSE(result.isMemoryTainted(after, slot));
  EXPECT_TRUE(result.isReachableMemoryTainted(after, slot));
  EXPECT_TRUE(result.isMemoryTainted(after, q));
}

TEST(NPA, InterproceduralTaintReachabilityTracksCurrentPointerSlotContents) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %slot = alloca i8*, align 8
      store i8* %p0, i8** %slot, align 8
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      store i8* null, i8** %slot, align 8
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath = writeTempTaintConfig("SOURCE fgets Arg0 D T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *slot = findInstructionByName(*mainFn, "slot");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(result.isReachableMemoryTainted(after, slot));
}

TEST(NPA, InterproceduralTaintReachabilityTracksCalleePointerStores) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)

    define void @store_ptr(i8** %slot, i8* %p) {
    entry:
      store i8* %p, i8** %slot, align 8
      ret void
    }

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %slot = alloca i8*, align 8
      store i8* null, i8** %slot, align 8
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      call void @store_ptr(i8** %slot, i8* %p0)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath = writeTempTaintConfig("SOURCE fgets Arg0 D T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *slot = findInstructionByName(*mainFn, "slot");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(slot, nullptr);
  EXPECT_TRUE(result.isReachableMemoryTainted(after, slot));
}

TEST(NPA, InterproceduralTaintReachabilityTracksCalleeGlobalPointerStores) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @gp = global i8* null
    declare i8* @fgets(i8*, i64, i8*)

    define void @set_global_ptr(i8* %p) {
    entry:
      store i8* %p, i8** @gp, align 8
      ret void
    }

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      call void @set_global_ptr(i8* %p0)
      br label %after

    after:
      %q = load i8*, i8** @gp, align 8
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath = writeTempTaintConfig("SOURCE fgets Arg0 D T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *gp = module->getGlobalVariable("gp");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(gp, nullptr);
  EXPECT_TRUE(result.isReachableMemoryTainted(after, gp));
}

TEST(NPA, InterproceduralTaintReachabilityTracksCalleePointerStoreClears) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)

    define void @clear_slot(i8** %slot) {
    entry:
      store i8* null, i8** %slot, align 8
      ret void
    }

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %slot = alloca i8*, align 8
      store i8* %p0, i8** %slot, align 8
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      call void @clear_slot(i8** %slot)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath = writeTempTaintConfig("SOURCE fgets Arg0 D T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *slot = findInstructionByName(*mainFn, "slot");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(result.isReachableMemoryTainted(after, slot));
}

TEST(NPA, InterproceduralTaintReachableDerefPipePropagatesTransitively) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)
    declare i8* @memcpy(i8*, i8*, i64)

    define i32 @main() {
    entry:
      %srcbuf = alloca [8 x i8], align 1
      %src = getelementptr inbounds [8 x i8], [8 x i8]* %srcbuf, i64 0, i64 0
      %srcslot = alloca i8*, align 8
      store i8* %src, i8** %srcslot, align 8

      %dstbuf = alloca [8 x i8], align 1
      %dst = getelementptr inbounds [8 x i8], [8 x i8]* %dstbuf, i64 0, i64 0
      %dstslot = alloca i8*, align 8
      store i8* %dst, i8** %dstslot, align 8

      %call = call i8* @fgets(i8* %src, i64 8, i8* null)
      %srcslot.raw = bitcast i8** %srcslot to i8*
      %dstslot.raw = bitcast i8** %dstslot to i8*
      %copy = call i8* @memcpy(i8* %dstslot.raw, i8* %srcslot.raw, i64 8)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE fgets Ret V T\n"
                           "SOURCE fgets Arg0 D T\n"
                           "PIPE fgets Ret V Arg0 V\n"
                           "PIPE memcpy Arg0 R Arg1 R\n"
                           "PIPE memcpy Ret V Arg0 V\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *src = findInstructionByName(*mainFn, "src");
  const auto *srcslotRaw = findInstructionByName(*mainFn, "srcslot.raw");
  const auto *dst = findInstructionByName(*mainFn, "dst");
  const auto *dstslotRaw = findInstructionByName(*mainFn, "dstslot.raw");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(src, nullptr);
  ASSERT_NE(srcslotRaw, nullptr);
  ASSERT_NE(dst, nullptr);
  ASSERT_NE(dstslotRaw, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, dst));
  EXPECT_TRUE(result.isReachableMemoryTainted(after, dstslotRaw));
}

TEST(NPA, InterproceduralTaintAfterArgPipeUsesShippedSpecOrder) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @fmt = private unnamed_addr constant [3 x i8] c"%s\00", align 1
    declare i8* @fgets(i8*, i64, i8*)
    declare i32 @snprintf(i8*, i64, i8*, ...)

    define i32 @main() {
    entry:
      %srcbuf = alloca [8 x i8], align 1
      %src = getelementptr inbounds [8 x i8], [8 x i8]* %srcbuf, i64 0, i64 0
      %dstbuf = alloca [8 x i8], align 1
      %dst = getelementptr inbounds [8 x i8], [8 x i8]* %dstbuf, i64 0, i64 0
      %fmtptr = getelementptr inbounds [3 x i8], [3 x i8]* @fmt, i64 0, i64 0
      %in = call i8* @fgets(i8* %src, i64 8, i8* null)
      %n = call i32 (i8*, i64, i8*, ...) @snprintf(i8* %dst, i64 8,
                                                   i8* %fmtptr, i8* %src)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE fgets Arg0 D T\n"
                           "PIPE snprintf Arg0 D AfterArg2 D\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *src = findInstructionByName(*mainFn, "src");
  const auto *dst = findInstructionByName(*mainFn, "dst");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(src, nullptr);
  ASSERT_NE(dst, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, src));
  EXPECT_TRUE(result.isMemoryTainted(after, dst));
}

TEST(NPA, InterproceduralTaintResolvesIndirectExternalSourceViaAliasWrapper) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @srcfp = global i32 ()* @getchar
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %fp = load i32 ()*, i32 ()** @srcfp
      %x = call i32 %fp()
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *x = findInstructionByName(*mainFn, "x");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(x, nullptr);
  EXPECT_TRUE(result.isValueTainted(after, x));
}

TEST(NPA, InterproceduralTaintDetectsSinkHits) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)
    declare i32 @printf(i8*, ...)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      %sink = call i32 (i8*, ...) @printf(i8* %p0)
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *sink =
      llvm::dyn_cast<llvm::CallBase>(findInstructionByName(*mainFn, "sink"));
  ASSERT_NE(sink, nullptr);
  EXPECT_TRUE(result.isSinkTriggered(sink));
  EXPECT_FALSE(result.sinkHits.empty());
}

TEST(NPA, InterproceduralTaintResolvesIndirectExternalSinkViaAliasWrapper) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @sinkfp = global i32 (i8*)* @system
    declare i8* @fgets(i8*, i64, i8*)
    declare i32 @system(i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      %fp = load i32 (i8*)*, i32 (i8*)** @sinkfp
      %sink = call i32 %fp(i8* %p0)
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  const auto *sink =
      llvm::dyn_cast<llvm::CallBase>(findInstructionByName(*mainFn, "sink"));
  ASSERT_NE(sink, nullptr);
  EXPECT_TRUE(result.isSinkTriggered(sink));
}

TEST(NPA, InterproceduralTaintSinkReplayUsesAnalysisSpecificCalleeResolution) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @sinkfp = global i32 (i8*)* @system
    declare i8* @fgets(i8*, i64, i8*)
    declare i32 @system(i8*)

    define i32 @system_local(i8* %arg) {
    entry:
      ret i32 0
    }

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      %fp = load i32 (i8*)*, i32 (i8*)** @sinkfp
      %sink = call i32 %fp(i8* %p0)
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *sink =
      llvm::dyn_cast<llvm::CallBase>(findInstructionByName(*mainFn, "sink"));
  ASSERT_NE(sink, nullptr);
  ASSERT_EQ(result.sinkHits.size(), 1u);
  EXPECT_TRUE(result.isSinkTriggered(sink));
}

TEST(NPA, InterproceduralTaintIgnoresUninitializedOnlySinkPaths) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @malloc(i64)
    declare i32 @printf(i8*, ...)

    define i32 @main() {
    entry:
      %p = call i8* @malloc(i64 4)
      %sink = call i32 (i8*, ...) @printf(i8* %p)
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *sink =
      llvm::dyn_cast<llvm::CallBase>(findInstructionByName(*mainFn, "sink"));
  ASSERT_NE(sink, nullptr);
  EXPECT_FALSE(result.isSinkTriggered(sink));
}

TEST(NPA, InterproceduralTaintObserverLibraryCallDoesNotKillArgumentTaint) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)
    declare i64 @strlen(i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      %len = call i64 @strlen(i8* %p0)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p0, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, p0));
}

TEST(NPA, InterproceduralTaintFailsClosedWhenConfigIsMissing) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %x = call i32 @getchar()
      ret i32 %x
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterproceduralTaint::Options options;
  options.taint_config_path = "/definitely/missing/npa-taint.spec";
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);

  EXPECT_TRUE(result.status.configuration_error);
  EXPECT_FALSE(result.status.overall_converged);
  EXPECT_TRUE(result.sinkHits.empty());
}

TEST(NPA, InterproceduralTaintUnsupportedUninitializedSpecsDoNotAbortByDefault) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %x = call i32 @getchar()
      ret i32 %x
    }
  )");
  ASSERT_TRUE(module);

  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V U\n");
  ASSERT_FALSE(configPath.empty());

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  EXPECT_TRUE(result.status.unsupported_specs);
  EXPECT_TRUE(result.status.summary_solve.converged);
  EXPECT_FALSE(result.valueBits.empty());
  EXPECT_TRUE(result.status.approximated);
  EXPECT_FALSE(result.status.overall_converged);
}

TEST(NPA, InterproceduralTaintRejectsUnsupportedUninitializedSpecsInStrictMode) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %x = call i32 @getchar()
      ret i32 %x
    }
  )");
  ASSERT_TRUE(module);

  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V U\n");
  ASSERT_FALSE(configPath.empty());

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  options.fail_on_unsupported_specs = true;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  EXPECT_TRUE(result.status.unsupported_specs);
  EXPECT_FALSE(result.status.summary_solve.converged);
  EXPECT_TRUE(result.valueBits.empty());
  EXPECT_FALSE(result.status.overall_converged);
}

TEST(NPA, InterproceduralTaintSupportsReturnMemorySourceSpecs) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @mkbuf()

    define i32 @main() {
    entry:
      %p = call i8* @mkbuf()
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  const std::string configPath = writeTempTaintConfig("SOURCE mkbuf Ret D T\n");
  ASSERT_FALSE(configPath.empty());

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *p = findInstructionByName(*mainFn, "p");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, p));
}

TEST(
    NPA,
    InterproceduralTaintUsesAnalysisSpecificCalleeResolutionDuringPropagation) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @fp = global i32 (i8*)* @clean_ret
    declare i32 @getchar()

    define i32 @clean_ret(i8* %p) {
    entry:
      ret i32 0
    }

    define i32 @tainted_ret(i8* %p) {
    entry:
      %t = call i32 @getchar()
      ret i32 %t
    }

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %f = load i32 (i8*)*, i32 (i8*)** @fp
      %r = call i32 %f(i8* %p0)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterproceduralTaint::Options options;
  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *r = findInstructionByName(*mainFn, "r");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_FALSE(result.isValueTainted(after, r));
}

TEST(NPA, InterproceduralTaintSinkReplayAfterSelectTransformation) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)
    declare i32 @system(i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %other = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %p1 = getelementptr inbounds [8 x i8], [8 x i8]* %other, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      %choice = select i1 true, i8* %p0, i8* %p1
      %sink = call i32 @system(i8* %choice)
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  const auto *sink =
      llvm::dyn_cast<llvm::CallBase>(findInstructionByName(*mainFn, "sink"));
  ASSERT_NE(sink, nullptr);
  EXPECT_TRUE(result.isSinkTriggered(sink));
}

TEST(NPA, InterproceduralTaintLoadDoesNotUsePointerValueTaintByDefault) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %slot = alloca i32, align 4
      store i32 7, i32* %slot, align 4
      %t = call i32 @getchar()
      %cmp = icmp eq i32 %t, 0
      %p = select i1 %cmp, i32* %slot, i32* %slot
      %x = load i32, i32* %p, align 4
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *x = findInstructionByName(*mainFn, "x");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(x, nullptr);
  EXPECT_FALSE(result.isValueTainted(after, x));
}

TEST(NPA, InterproceduralTaintUnknownExternalCallIsNoop) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @mystery(i8*)
    declare i32 @system(i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %r = call i8* @mystery(i8* %p0)
      %sink = call i32 @system(i8* %p0)
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  const auto *sink =
      llvm::dyn_cast<llvm::CallBase>(findInstructionByName(*mainFn, "sink"));
  ASSERT_NE(sink, nullptr);
  EXPECT_FALSE(result.isSinkTriggered(sink));
}

TEST(NPA, InterproceduralTaintUnknownExternalPointerArgLeavesFactsUntainted) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @mystery(i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %r = call i8* @mystery(i8* %p0)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p0, nullptr);
  EXPECT_FALSE(result.isMemoryTainted(after, p0));
}

TEST(NPA, InterproceduralTaintUnresolvedIndirectCallIsNoop) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @driver(i8* (i8*)* %fp) {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %r = call i8* %fp(i8* %p0)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(*module, wrapper);

  const auto *driverFn = module->getFunction("driver");
  ASSERT_NE(driverFn, nullptr);
  const auto *after = findBlockByName(*driverFn, "after");
  const auto *p = findInstructionByName(*driverFn, "p0");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p, nullptr);
  EXPECT_FALSE(result.isMemoryTainted(after, p));
}

TEST(NPA, InterproceduralTaintHandlesExternalPointerGlobalsWithoutInitializers) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @ext = external global i8*

    define i32 @main() {
    entry:
      %p = load i8*, i8** @ext, align 8
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  EXPECT_FALSE(result.status.configuration_error);
  EXPECT_TRUE(result.status.summary_solve.converged);
}

TEST(NPA, InterproceduralTaintHandlesCyclicConstantBackedIndirectTargets) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @fp = global i8* bitcast (i8** @fp to i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %raw = load i8*, i8** @fp, align 8
      %callee = bitcast i8* %raw to i32 (i8*)*
      %r = call i32 %callee(i8* %p0)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterproceduralTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterproceduralTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlockByName(*mainFn, "after");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p0, nullptr);
  EXPECT_FALSE(result.status.configuration_error);
  EXPECT_TRUE(result.status.summary_solve.converged);
  EXPECT_FALSE(result.isMemoryTainted(after, p0));
}

TEST(NPA, InterproceduralTaintTensorStrategyFallsBackToScc) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %x = call i32 @getchar()
      ret i32 %x
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterproceduralTaint::run(
      *module, wrapper, false, npa::LinearStrategy::TensorProduct);

  EXPECT_TRUE(result.status.summary_solve.converged);
}
