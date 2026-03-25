/**
 * @file UnderApproxAATest.cpp
 * @brief Unit tests for UnderApproxAA/EquivDB must-alias inference.
 */

#include "Alias/UnderApproxAA/EquivDB.h"
#include "Alias/UnderApproxAA/UnderApproxAA.h"
#include "TestUtils/LLVMHelpers.h"

#include <memory>

#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace UnderApprox;
using namespace lotus::unittest;

namespace {

struct MemorySSAContext {
  TargetLibraryInfoImpl TLII;
  TargetLibraryInfo TLI;
  AssumptionCache AC;
  AAResults AAR;
  BasicAAResult BAA;
  std::unique_ptr<MemorySSA> MSSA;

  MemorySSAContext(Function &F, DominatorTree &DT)
      : TLI(TLII), AC(F), AAR(TLI),
        BAA(F.getParent()->getDataLayout(), F, TLI, AC, &DT) {
    AAR.addAAResult(BAA);
    MSSA = std::make_unique<MemorySSA>(F, &AAR, &DT);
  }
};

class UnderApproxAATest : public LlvmModuleTest {};

TEST_F(UnderApproxAATest, ConstantSelectMustAliasChosenArm) {
  const char *Source = R"(
    define void @test(i8* %p, i8* %q) {
    entry:
      %s = select i1 true, i8* %p, i8* %q
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  Instruction *S = findInstructionByName(*F, "s");
  Argument *P = F->getArg(0);
  ASSERT_NE(S, nullptr);
  ASSERT_NE(P, nullptr);

  EquivDB DB(*F);
  EXPECT_TRUE(DB.mustAlias(S, P));
}

TEST_F(UnderApproxAATest, MemoryPhiForwardingUsesMustAliasValueClass) {
  const char *Source = R"(
    define i8* @test(i1 %c1, i1 %c2) {
    entry:
      %x = alloca i8
      %slot = alloca i8*
      %x_cast = bitcast i8* %x to i8*
      br i1 %c1, label %then, label %else

    then:
      store i8* %x_cast, i8** %slot
      br label %merge

    else:
      %v_alt = select i1 %c2, i8* %x, i8* %x_cast
      store i8* %v_alt, i8** %slot
      br label %merge

    merge:
      %q = load i8*, i8** %slot
      ret i8* %q
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *X = findInstructionByName(*F, "x");
  auto *Q = findInstructionByName(*F, "q");
  ASSERT_NE(X, nullptr);
  ASSERT_NE(Q, nullptr);

  DominatorTree DT(*F);
  MemorySSAContext MemCtx(*F, DT);

  EquivDB DB(*F, MemCtx.MSSA.get(), &DT);
  EXPECT_TRUE(DB.mustAlias(Q, X));
}

TEST_F(UnderApproxAATest, SingleStoreAllocaIsSlotSensitive) {
  const char *Source = R"(
    define void @test(i8* %a, i8* %b) {
    entry:
      %slot = alloca [2 x i8*]
      %p0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %slot, i64 0, i64 0
      %p1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %slot, i64 0, i64 1
      store i8* %a, i8** %p0
      store i8* %b, i8** %p1
      %l0 = load i8*, i8** %p0
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *A = F->getArg(0);
  auto *L0 = findInstructionByName(*F, "l0");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(L0, nullptr);

  DominatorTree DT(*F);
  EquivDB DB(*F, nullptr, &DT);
  EXPECT_TRUE(DB.mustAlias(L0, A));
}

TEST_F(UnderApproxAATest, SingleStoreGlobalSlotIsForwarded) {
  const char *Source = R"(
    @G = global [2 x i8*] zeroinitializer

    define void @test(i8* %a, i8* %b) {
    entry:
      %g0 = getelementptr inbounds [2 x i8*], [2 x i8*]* @G, i64 0, i64 0
      %g1 = getelementptr inbounds [2 x i8*], [2 x i8*]* @G, i64 0, i64 1
      %g0_alias = bitcast i8** %g0 to i8**
      store i8* %a, i8** %g0
      store i8* %b, i8** %g1
      %l0 = load i8*, i8** %g0_alias
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *A = F->getArg(0);
  auto *L0 = findInstructionByName(*F, "l0");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(L0, nullptr);

  DominatorTree DT(*F);
  EquivDB DB(*F, nullptr, &DT);
  EXPECT_TRUE(DB.mustAlias(L0, A));
}

TEST_F(UnderApproxAATest, SingleStoreHeapSlotIsForwarded) {
  const char *Source = R"(
    declare noalias i8* @malloc(i64)

    define void @test(i8* %a, i8* %b) {
    entry:
      %buf = call i8* @malloc(i64 16)
      %slot0 = bitcast i8* %buf to i8**
      %slot1 = getelementptr inbounds i8*, i8** %slot0, i64 1
      %slot0_alias = bitcast i8** %slot0 to i8**
      store i8* %a, i8** %slot0
      store i8* %b, i8** %slot1
      %l0 = load i8*, i8** %slot0_alias
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *A = F->getArg(0);
  auto *L0 = findInst(F, "l0");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(L0, nullptr);

  DominatorTree DT(*F);
  EquivDB DB(*F, nullptr, &DT);
  EXPECT_TRUE(DB.mustAlias(L0, A));
}

TEST_F(UnderApproxAATest, UnderApproxAAHandlesGlobalVsLocalFunctionQuery) {
  const char *Source = R"(
    @G = global i8 0

    define i8* @test() {
    entry:
      %p = getelementptr inbounds i8, i8* @G, i64 0
      ret i8* %p
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  auto *G = M->getNamedGlobal("G");
  auto *F = M->getFunction("test");
  ASSERT_NE(G, nullptr);
  ASSERT_NE(F, nullptr);

  auto *P = findInst(F, "p");
  ASSERT_NE(P, nullptr);

  UnderApproxAA AA(*M);
  EXPECT_TRUE(AA.mustAlias(P, G));
}

TEST_F(UnderApproxAATest, ClosedGEPSupportsEquivalentIntegerIndexExprs) {
  const char *Source = R"(
    define void @test(i8* %base, i64 %i) {
    entry:
      %base1 = bitcast i8* %base to i8*
      %base2 = select i1 true, i8* %base, i8* %base
      %idx1 = add i64 %i, 1
      %idx2 = add i64 1, %i
      %p = getelementptr inbounds i8, i8* %base1, i64 %idx1
      %q = getelementptr inbounds i8, i8* %base2, i64 %idx2
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *P = findInst(F, "p");
  auto *Q = findInst(F, "q");
  ASSERT_NE(P, nullptr);
  ASSERT_NE(Q, nullptr);

  EquivDB DB(*F);
  EXPECT_TRUE(DB.mustAlias(P, Q));
}

TEST_F(UnderApproxAATest, UnderApproxAACacheRefreshesAfterIRMutation) {
  const char *Source = R"(
    define void @test(i8* %p, i8* %q) {
    entry:
      %s = select i1 true, i8* %p, i8* %q
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *P = F->getArg(0);
  auto *Q = F->getArg(1);
  auto *S = dyn_cast<SelectInst>(findInst(F, "s"));
  ASSERT_NE(P, nullptr);
  ASSERT_NE(Q, nullptr);
  ASSERT_NE(S, nullptr);

  UnderApproxAA AA(*M);
  EXPECT_FALSE(AA.mustAlias(S, Q));

  S->setCondition(ConstantInt::getTrue(context));
  S->setTrueValue(P);
  S->setFalseValue(P);

  EXPECT_TRUE(AA.mustAlias(S, P));
}

TEST_F(UnderApproxAATest, QueryReturnsMayAliasForUnknown) {
  const char *Source = R"(
    define void @test(i8* %p, i8* %q) {
    entry:
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  UnderApproxAA AA(*M);
  EXPECT_EQ(AA.query(F->getArg(0), F->getArg(1)), AliasResult::MayAlias);
}

TEST_F(UnderApproxAATest, DirectCallSummaryReturnsArgumentPath) {
  const char *Source = R"(
    define i8** @ret_field([2 x i8*]* %arr) {
    entry:
      %elt = getelementptr inbounds [2 x i8*], [2 x i8*]* %arr, i64 0, i64 1
      ret i8** %elt
    }

    define void @test() {
    entry:
      %arr = alloca [2 x i8*]
      %elt = getelementptr inbounds [2 x i8*], [2 x i8*]* %arr, i64 0, i64 1
      %ret = call i8** @ret_field([2 x i8*]* %arr)
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *Elt = findInst(F, "elt");
  auto *Ret = findInst(F, "ret");
  ASSERT_NE(Elt, nullptr);
  ASSERT_NE(Ret, nullptr);

  EquivDB DB(*F);
  EXPECT_TRUE(DB.mustAlias(Elt, Ret));
}

TEST_F(UnderApproxAATest, DirectCallSummaryAppliesStrongStoreEffect) {
  const char *Source = R"(
    define void @write_arg(i8** %slot, i8* %v) {
    entry:
      store i8* %v, i8** %slot
      ret void
    }

    define i8* @test(i8* %a) {
    entry:
      %slot = alloca i8*
      call void @write_arg(i8** %slot, i8* %a)
      %l = load i8*, i8** %slot
      ret i8* %l
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *L = findInst(F, "l");
  auto *A = F->getArg(0);
  ASSERT_NE(L, nullptr);
  ASSERT_NE(A, nullptr);

  EquivDB DB(*F);
  EXPECT_TRUE(DB.mustAlias(L, A));
}

TEST_F(UnderApproxAATest, UnknownCallKillsSingletonStoreFact) {
  const char *Source = R"(
    declare void @opaque(i8**)

    define i8* @test(i8* %a) {
    entry:
      %slot = alloca i8*
      store i8* %a, i8** %slot
      call void @opaque(i8** %slot)
      %l = load i8*, i8** %slot
      ret i8* %l
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *L = findInst(F, "l");
  auto *A = F->getArg(0);
  ASSERT_NE(L, nullptr);
  ASSERT_NE(A, nullptr);

  EquivDB DB(*F);
  EXPECT_FALSE(DB.mustAlias(L, A));
}

TEST_F(UnderApproxAATest, ReadonlyCallPreservesSingletonStoreFact) {
  const char *Source = R"(
    declare void @reader(i8**) readonly

    define i8* @test(i8* %a) {
    entry:
      %slot = alloca i8*
      store i8* %a, i8** %slot
      call void @reader(i8** %slot)
      %l = load i8*, i8** %slot
      ret i8* %l
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *L = findInst(F, "l");
  auto *A = F->getArg(0);
  ASSERT_NE(L, nullptr);
  ASSERT_NE(A, nullptr);

  EquivDB DB(*F);
  EXPECT_TRUE(DB.mustAlias(L, A));
}

TEST_F(UnderApproxAATest, UnknownCallPreservesUnrelatedLocalSlot) {
  const char *Source = R"(
    declare void @opaque(i8**)

    define i8* @test(i8* %a, i8* %b) {
    entry:
      %slot0 = alloca i8*
      %slot1 = alloca i8*
      store i8* %a, i8** %slot0
      store i8* %b, i8** %slot1
      call void @opaque(i8** %slot1)
      %l0 = load i8*, i8** %slot0
      ret i8* %l0
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *L0 = findInst(F, "l0");
  auto *A = F->getArg(0);
  ASSERT_NE(L0, nullptr);
  ASSERT_NE(A, nullptr);

  EquivDB DB(*F);
  EXPECT_TRUE(DB.mustAlias(L0, A));
}

TEST_F(UnderApproxAATest, UnknownCallDoesNotPreserveGlobalSlot) {
  const char *Source = R"(
    @G = global i8* null
    declare void @opaque()

    define i8* @test(i8* %a) {
    entry:
      store i8* %a, i8** @G
      call void @opaque()
      %l = load i8*, i8** @G
      ret i8* %l
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *L = findInst(F, "l");
  auto *A = F->getArg(0);
  ASSERT_NE(L, nullptr);
  ASSERT_NE(A, nullptr);

  EquivDB DB(*F);
  EXPECT_FALSE(DB.mustAlias(L, A));
}

TEST_F(UnderApproxAATest, ArgMemOnlyCallKillsOnlyReachableSlots) {
  const char *Source = R"(
    declare void @argonly(i8**) argmemonly

    define void @test(i8* %a, i8* %b) {
    entry:
      %slot0 = alloca i8*
      %slot1 = alloca i8*
      store i8* %a, i8** %slot0
      store i8* %b, i8** %slot1
      call void @argonly(i8** %slot1)
      %l0 = load i8*, i8** %slot0
      %l1 = load i8*, i8** %slot1
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *L0 = findInst(F, "l0");
  auto *L1 = findInst(F, "l1");
  auto *A = F->getArg(0);
  auto *B = F->getArg(1);
  ASSERT_NE(L0, nullptr);
  ASSERT_NE(L1, nullptr);
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);

  EquivDB DB(*F);
  EXPECT_TRUE(DB.mustAlias(L0, A));
  EXPECT_FALSE(DB.mustAlias(L1, B));
}

TEST_F(UnderApproxAATest, DirectCallSummaryIntersectsReturnAcrossExits) {
  const char *Source = R"(
    define i8* @ret_arg(i1 %c, i8* %p) {
    entry:
      br i1 %c, label %then, label %else
    then:
      ret i8* %p
    else:
      %q = bitcast i8* %p to i8*
      ret i8* %q
    }

    define i8* @test(i1 %c, i8* %p) {
    entry:
      %r = call i8* @ret_arg(i1 %c, i8* %p)
      ret i8* %r
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *R = findInst(F, "r");
  auto *P = F->getArg(1);
  ASSERT_NE(R, nullptr);
  ASSERT_NE(P, nullptr);

  EquivDB DB(*F);
  EXPECT_TRUE(DB.mustAlias(R, P));
}

TEST_F(UnderApproxAATest, DirectCallSummaryDropsConflictingStoreEffectsButKeepsReturn) {
  const char *Source = R"(
    define i8* @writer(i1 %c, i8** %slot, i8* %v) {
    entry:
      br i1 %c, label %then, label %else
    then:
      store i8* %v, i8** %slot
      br label %merge
    else:
      store i8* null, i8** %slot
      br label %merge
    merge:
      ret i8* %v
    }

    define i8* @test(i1 %c, i8* %v) {
    entry:
      %slot = alloca i8*
      %ret = call i8* @writer(i1 %c, i8** %slot, i8* %v)
      %l = load i8*, i8** %slot
      ret i8* %ret
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *Ret = findInst(F, "ret");
  auto *L = findInst(F, "l");
  auto *V = F->getArg(1);
  ASSERT_NE(Ret, nullptr);
  ASSERT_NE(L, nullptr);
  ASSERT_NE(V, nullptr);

  EquivDB DB(*F);
  EXPECT_TRUE(DB.mustAlias(Ret, V));
  EXPECT_FALSE(DB.mustAlias(L, V));
}

TEST_F(UnderApproxAATest, SummaryCacheIsSafeAcrossMultipleModules) {
  const char *First = R"(
    define i8* @id(i8* %p) {
    entry:
      ret i8* %p
    }

    define i8* @test(i8* %p) {
    entry:
      %r = call i8* @id(i8* %p)
      ret i8* %r
    }
  )";

  const char *Second = R"(
    define i8* @id(i8* %p) {
    entry:
      ret i8* null
    }

    define i8* @test(i8* %p) {
    entry:
      %r = call i8* @id(i8* %p)
      ret i8* %r
    }
  )";

  auto M1 = parseModule(First);
  auto M2 = parseModule(Second);
  ASSERT_NE(M1, nullptr);
  ASSERT_NE(M2, nullptr);

  Function *F1 = M1->getFunction("test");
  Function *F2 = M2->getFunction("test");
  ASSERT_NE(F1, nullptr);
  ASSERT_NE(F2, nullptr);

  auto *R1 = findInst(F1, "r");
  auto *R2 = findInst(F2, "r");
  auto *P1 = F1->getArg(0);
  auto *P2 = F2->getArg(0);
  ASSERT_NE(R1, nullptr);
  ASSERT_NE(R2, nullptr);
  ASSERT_NE(P1, nullptr);
  ASSERT_NE(P2, nullptr);

  EquivDB DB1(*F1);
  EquivDB DB2(*F2);
  EXPECT_TRUE(DB1.mustAlias(R1, P1));
  EXPECT_FALSE(DB2.mustAlias(R2, P2));
}

} // namespace
