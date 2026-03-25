#include "Analysis/Concurrency/OpenMP/DataSharingAnalysis.h"

#include "TestUtils/LLVMHelpers.h"

using namespace llvm;
using namespace OpenMP;

class DataSharingAnalysisTest : public lotus::unittest::LlvmModuleTest {};

TEST_F(DataSharingAnalysisTest, OutlinedCapturesInferSharedAndFirstprivate) {
  const char *source = R"(
    define internal void @.omp_outlined.(i32* %.omp.shared_ptr, i32 %.omp.val) {
    entry:
      %v = load i32, i32* %.omp.shared_ptr, align 4
      store i32 %v, i32* %.omp.shared_ptr, align 4
      %x = add i32 %.omp.val, 1
      ret void
    }

    define i32 @main() {
    entry:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DataSharingAnalysis analysis(*module);
  analysis.analyze();

  const Function *outlined = module->getFunction(".omp_outlined.");
  ASSERT_NE(outlined, nullptr);
  const Argument *arg0 = outlined->arg_begin();
  const Argument *arg1 = arg0 + 1;

  EXPECT_TRUE(analysis.isShared(arg0));
  EXPECT_TRUE(analysis.isFirstprivate(arg1));
  auto entries = analysis.getEntriesForRegion(outlined);
  EXPECT_EQ(entries.size(), 2u);
}

TEST_F(DataSharingAnalysisTest, ReadOnlyPointerCaptureBecomesSharedNoModify) {
  const char *source = R"(
    define internal void @.omp_outlined.(i32* %.omp.read_ptr) {
    entry:
      %v = load i32, i32* %.omp.read_ptr, align 4
      %x = add i32 %v, 1
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DataSharingAnalysis analysis(*module);
  analysis.analyze();

  const Function *outlined = module->getFunction(".omp_outlined.");
  ASSERT_NE(outlined, nullptr);
  const Argument *arg0 = outlined->arg_begin();
  EXPECT_EQ(analysis.getAttribute(arg0), DataSharingAttribute::SharedNoModify);
}

TEST_F(DataSharingAnalysisTest, EscapingPointerCaptureStaysConservativeShared) {
  const char *source = R"(
    declare void @sink(i32*)

    define internal void @.omp_outlined.(i32* %.omp.ptr) {
    entry:
      call void @sink(i32* %.omp.ptr)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DataSharingAnalysis analysis(*module);
  analysis.analyze();

  const Function *outlined = module->getFunction(".omp_outlined.");
  ASSERT_NE(outlined, nullptr);
  const Argument *arg0 = outlined->arg_begin();
  EXPECT_EQ(analysis.getAttribute(arg0), DataSharingAttribute::Shared);
}

TEST_F(DataSharingAnalysisTest, EntriesCarryCanonicalRegionKeys) {
  const char *source = R"(
    define internal void @.omp_outlined.(i32* %.omp.shared_ptr, i32 %.omp.val) {
    entry:
      %tmp = alloca i32*, align 8
      store i32* %.omp.shared_ptr, i32** %tmp, align 8
      %loaded = load i32*, i32** %tmp, align 8
      %elt = getelementptr inbounds i32, i32* %loaded, i64 1
      store i32 7, i32* %elt, align 4
      %x = add i32 %.omp.val, 1
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DataSharingAnalysis analysis(*module);
  analysis.analyze();

  const Function *outlined = module->getFunction(".omp_outlined.");
  ASSERT_NE(outlined, nullptr);
  auto entries = analysis.getEntriesForRegion(outlined);
  ASSERT_EQ(entries.size(), 2u);

  const Argument *arg0 = outlined->arg_begin();
  bool found_region_key = false;
  for (const auto &entry : entries) {
    if (entry.variable == arg0) {
      found_region_key = true;
      EXPECT_EQ(entry.canonical_base, arg0);
    }
  }
  EXPECT_TRUE(found_region_key);
}

TEST_F(DataSharingAnalysisTest,
       GlobalAnnotationParsesFirstprivateBeforePrivate) {
  const char *source = R"(
    @g = global i32 0, align 4
    @.str = private unnamed_addr constant [20 x i8] c"omp firstprivate(g)\00", align 1
    @.str.1 = private unnamed_addr constant [1 x i8] c"\00", align 1

    @llvm.global.annotations = appending global [1 x { i8*, i8*, i8*, i32, i8* }] [
      { i8*, i8*, i8*, i32, i8* } {
        i8* bitcast (i32* @g to i8*),
        i8* getelementptr inbounds ([20 x i8], [20 x i8]* @.str, i32 0, i32 0),
        i8* getelementptr inbounds ([1 x i8], [1 x i8]* @.str.1, i32 0, i32 0),
        i32 1,
        i8* null
      }
    ], section "llvm.metadata"
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DataSharingAnalysis analysis(*module);
  analysis.analyze();

  const GlobalVariable *g = module->getGlobalVariable("g");
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(analysis.getAttribute(g), DataSharingAttribute::Firstprivate);
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
