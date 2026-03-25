#include "Analysis/Crypto/ctllvm.h"

#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>

using namespace lotus::unittest;

namespace {

struct PassFixture {
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb;

  PassFixture() {
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
  }
};

std::string runPass(llvm::Module &module, CTPass pass) {
  PassFixture fixture;
  testing::internal::CaptureStderr();
  pass.run(module, fixture.mam);
  return testing::internal::GetCapturedStderr();
}

std::string runPipeline(llvm::Module &module, llvm::StringRef pipeline) {
  PassFixture fixture;
  auto plugin_info = getPassPluginInfo();
  plugin_info.RegisterPassBuilderCallbacks(fixture.pb);

  llvm::ModulePassManager mpm;
  llvm::Error err = fixture.pb.parsePassPipeline(mpm, pipeline);
  if (err) {
    std::string message = llvm::toString(std::move(err));
    ADD_FAILURE() << "Failed to parse pipeline: " << message;
    return "";
  }

  testing::internal::CaptureStderr();
  mpm.run(module, fixture.mam);
  return testing::internal::GetCapturedStderr();
}

TEST(CryptoAnalysisTest, DefaultOptionsMatchLegacyDefaults) {
  CTOptions options;
  EXPECT_TRUE(options.file_path.empty());
  EXPECT_TRUE(options.type_system);
  EXPECT_TRUE(options.test_all_parameters);
  EXPECT_TRUE(options.enable_may_leak);
  EXPECT_TRUE(options.try_hard_on_name);
  EXPECT_FALSE(options.user_specify);
  EXPECT_TRUE(options.soundness_mode);
  EXPECT_EQ(options.alias_threshold, 2000);
  EXPECT_TRUE(options.report_leakages);
  EXPECT_FALSE(options.time_analysis);
  EXPECT_TRUE(options.auto_continue);
  EXPECT_EQ(options.inline_threshold, 10);
  EXPECT_FALSE(options.debug);
  EXPECT_FALSE(options.print_function);
}

TEST(CryptoAnalysisTest, PipelineRegistrationRunsCtllvmPass) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @example(i32 %secret) {
    entry:
      %sum = add i32 %secret, 1
      ret i32 %sum
    }
  )");
  ASSERT_NE(module, nullptr);

  std::string output = runPipeline(*module, "ctllvm");
  EXPECT_NE(output.find("\"function\": \"example\""), std::string::npos);
  EXPECT_NE(output.find("proved-CT"), std::string::npos);
}

TEST(CryptoAnalysisTest, ReportsBranchLeakForSecretCondition) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @example(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else

    then:
      ret i32 1

    else:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  std::string output = runPass(*module, CTPass{});
  EXPECT_NE(output.find("\"branch\": 1"), std::string::npos);
  EXPECT_NE(output.find("proved-NCT"), std::string::npos);
}

TEST(CryptoAnalysisTest, AliasThresholdStillCountsAsAnalysisFailure) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define void @example(i32* %p, i32* %q) {
    entry:
      %a = load i32, i32* %p, align 4
      store i32 %a, i32* %q, align 4
      %b = load i32, i32* %q, align 4
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  CTOptions options;
  options.alias_threshold = 1;

  std::string output = runPass(*module, CTPass(options));
  EXPECT_NE(output.find("Number of too many alias: 1"), std::string::npos);
}

TEST(CryptoAnalysisTest, InlineFailureAccountingPreservesIndirectCallStats) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define void @example(void ()* %fp) {
    entry:
      call void %fp()
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  CTOptions options;
  options.auto_continue = false;

  std::string output = runPass(*module, CTPass(options));
  EXPECT_NE(output.find("Cannot analyze function: example"), std::string::npos);
  EXPECT_NE(output.find("Number of indirect call: 1"), std::string::npos);
}

} // namespace
