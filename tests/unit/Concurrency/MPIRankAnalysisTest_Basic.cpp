#include "MPIRankAnalysisTestCommon.h"

TEST_F(MPIRankAnalysisTest, CommRankLoadsAreTrackedAsSymbolic) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      ret i32 %loaded
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIRankAnalysis analysis(*module);
  analysis.analyze();

  const Instruction *loaded =
      findInstructionByName(*module->getFunction("main"), "loaded");
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(analysis.getRankExpr(loaded).kind, RankExpr::Symbolic);
}

TEST_F(MPIRankAnalysisTest, EqualityBranchRefinesTrueSuccessor) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
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

TEST_F(MPIRankAnalysisTest, OrderedInequalityRefinesRange) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %lt_two = icmp ult i32 %loaded, 2
      br i1 %lt_two, label %then, label %else

    then:
      %low_rank = add i32 1, 2
      ret i32 %low_rank

    else:
      %high_rank = add i32 3, 4
      ret i32 %high_rank
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIRankAnalysis analysis(*module);
  analysis.analyze();

  const Instruction *low_rank =
      findInstructionByName(*module->getFunction("main"), "low_rank");
  ASSERT_NE(low_rank, nullptr);
  RankExpr expr = analysis.getRankAtInstruction(low_rank);
  EXPECT_EQ(expr.kind, RankExpr::Range);
  EXPECT_EQ(expr.range_min, 0);
  EXPECT_EQ(expr.range_max, 1);
}