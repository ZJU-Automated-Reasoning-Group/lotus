#include "MPIRankAnalysisTestCommon.h"

TEST_F(MPIRankAnalysisTest,
       InequalityBranchProducesExcludedParticipantPredicate) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %not_root = icmp ne i32 %loaded, 0
      br i1 %not_root, label %then, label %else

    then:
      %non_root = add i32 1, 2
      ret i32 %non_root

    else:
      %root = add i32 3, 4
      ret i32 %root
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIRankAnalysis analysis(*module);
  analysis.analyze();

  const Instruction *non_root =
      findInstructionByName(*module->getFunction("main"), "non_root");
  ASSERT_NE(non_root, nullptr);

  MPIRankPredicate predicate = analysis.getRankPredicateAtInstruction(non_root);
  EXPECT_FALSE(predicate.unknown);
  EXPECT_TRUE(predicate.universal);
  EXPECT_EQ(predicate.excluded_ranks.count(0), 1u);
  EXPECT_NE(analysis.getPredicateClassAtInstruction(non_root), 0u);
  EXPECT_NE(analysis.getParticipantClassAtInstruction(non_root), 0u);
}

TEST_F(MPIRankAnalysisTest, RankPredicatesDoNotMergeAcrossCommunicators) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)

    define i32 @main(i8* %comm0, i8* %comm1, i1 %pick_left) {
    entry:
      %rank0 = alloca i32, align 4
      %rank1 = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm0, i32* %rank0)
      call i32 @MPI_Comm_rank(i8* %comm1, i32* %rank1)
      %lhs = load i32, i32* %rank0, align 4
      %rhs = load i32, i32* %rank1, align 4
      br i1 %pick_left, label %left, label %right

    left:
      br label %join

    right:
      br label %join

    join:
      %merged = phi i32 [ %lhs, %left ], [ %rhs, %right ]
      ret i32 %merged
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIRankAnalysis analysis(*module);
  analysis.analyze();

  const Instruction *merged =
      findInstructionByName(*module->getFunction("main"), "merged");
  ASSERT_NE(merged, nullptr);
  MPIRankPredicate predicate = analysis.getRankPredicate(merged);
  EXPECT_TRUE(predicate.unknown);
}