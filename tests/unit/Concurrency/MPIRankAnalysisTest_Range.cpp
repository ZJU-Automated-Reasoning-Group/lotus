#include "MPIRankAnalysisTestCommon.h"

TEST_F(MPIRankAnalysisTest, CommSizeDerivedBoundRefinesRankRange) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Comm_size(i8*, i32*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      %size = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      call i32 @MPI_Comm_size(i8* %comm, i32* %size)
      %loaded_rank = load i32, i32* %rank, align 4
      %loaded_size = load i32, i32* %size, align 4
      %limit = sub i32 %loaded_size, 1
      %in_range = icmp ult i32 %loaded_rank, %limit
      br i1 %in_range, label %then, label %else

    then:
      %before_last = add i32 1, 2
      ret i32 %before_last

    else:
      %last_or_after = add i32 3, 4
      ret i32 %last_or_after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIRankAnalysis analysis(*module);
  analysis.analyze();

  const Instruction *before_last =
      findInstructionByName(*module->getFunction("main"), "before_last");
  ASSERT_NE(before_last, nullptr);
  RankExpr expr = analysis.getRankAtInstruction(before_last);
  EXPECT_EQ(expr.kind, RankExpr::Range);
  EXPECT_EQ(expr.range_min, 0);
  EXPECT_EQ(expr.range_max, MPIRankAnalysis::defaultCommSizeUpperBound() - 2);
}

TEST_F(MPIRankAnalysisTest, WrapperPropagatesRankInformationToCaller) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)

    define void @read_rank(i8* %comm, i32* %out) {
    entry:
      call i32 @MPI_Comm_rank(i8* %comm, i32* %out)
      ret void
    }

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call void @read_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %is_zero = icmp eq i32 %loaded, 0
      br i1 %is_zero, label %then, label %else

    then:
      %on_root = add i32 1, 2
      ret i32 %on_root

    else:
      %other = add i32 3, 4
      ret i32 %other
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIRankAnalysis analysis(*module);
  analysis.analyze();

  const Instruction *on_root =
      findInstructionByName(*module->getFunction("main"), "on_root");
  ASSERT_NE(on_root, nullptr);
  EXPECT_EQ(analysis.getRankAtInstruction(on_root).kind, RankExpr::Concrete);
  EXPECT_EQ(analysis.getRankAtInstruction(on_root).concrete_value, 0);
}