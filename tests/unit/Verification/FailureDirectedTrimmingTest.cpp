#include <gtest/gtest.h>

#include "TestUtils/LLVMHelpers.h"
#include "Verification/FailureDirectedTrimming/FailureDirectedTrimming.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

using namespace llvm;

namespace {

static unsigned countCallsTo(const Function &F, StringRef CalleeName) {
  unsigned Count = 0;
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      const Function *Callee = CI->getCalledFunction();
      if (Callee && Callee->getName() == CalleeName)
        ++Count;
    }
  }
  return Count;
}

static bool moduleHasFunctionWithPrefix(const Module &M, StringRef Prefix) {
  for (const Function &F : M) {
    if (F.getName().startswith(Prefix))
      return true;
  }
  return false;
}

// Check if a call instruction exists in a function
static const CallInst *findCallTo(const Function &F, StringRef CalleeName) {
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      const Function *Callee = CI->getCalledFunction();
      if (Callee && Callee->getName() == CalleeName)
        return CI;
    }
  }
  return nullptr;
}

// Verify that a call is wrapped with a nondet split:
//   if (nondet) call f.safe(...) else { call f(...); assume(false); unreachable }
static bool verifyCallWrapping(const Function &F, StringRef OriginalCallee,
                               StringRef SafeCallee) {
  const CallInst *SafeCall = findCallTo(F, SafeCallee);
  if (!SafeCall)
    return false;

  // The safe call should be in a basic block that's a successor of a branch
  const BasicBlock *SafeBB = SafeCall->getParent();
  if (!SafeBB)
    return false;

  // Find the predecessor that branches to this safe BB
  const BasicBlock *BranchBB = nullptr;
  const BranchInst *BranchBI = nullptr;
  for (const BasicBlock *Pred : predecessors(SafeBB)) {
    BranchBI = dyn_cast<BranchInst>(Pred->getTerminator());
    if (BranchBI && BranchBI->isConditional()) {
      // Check if SafeBB is either successor
      if (BranchBI->getSuccessor(0) == SafeBB || BranchBI->getSuccessor(1) == SafeBB) {
        BranchBB = Pred;
        break;
      }
    }
  }
  if (!BranchBB || !BranchBI)
    return false;

  // Find the failure branch (the one that's not SafeBB)
  const BasicBlock *FailBB = nullptr;
  if (BranchBI->getSuccessor(0) == SafeBB) {
    FailBB = BranchBI->getSuccessor(1);
  } else {
    FailBB = BranchBI->getSuccessor(0);
  }
  if (!FailBB)
    return false;

  // Check that the failure branch contains the original call
  const CallInst *OrigCall = nullptr;
  for (const Instruction &I : *FailBB) {
    auto *CI = dyn_cast<CallInst>(&I);
    if (CI) {
      const Function *Callee = CI->getCalledFunction();
      if (Callee && Callee->getName() == OriginalCallee) {
        OrigCall = CI;
        break;
      }
    }
  }
  if (!OrigCall)
    return false;

  // The failure branch should end with assume(false); unreachable
  const Instruction *Term = FailBB->getTerminator();
  if (!isa<UnreachableInst>(Term))
    return false;

  // Check for assume(false) before the unreachable
  bool FoundAssumeFalse = false;
  for (const Instruction &I : *FailBB) {
    auto *CI = dyn_cast<CallInst>(&I);
    if (CI && CI != OrigCall) {
      const Function *Callee = CI->getCalledFunction();
      if (Callee && Callee->getName() == "verifier.assume") {
        if (auto *C = dyn_cast<ConstantInt>(CI->getArgOperand(0))) {
          if (C->isZero()) {
            FoundAssumeFalse = true;
            break;
          }
        }
      }
    }
  }
  return FoundAssumeFalse;
}

// Verify that a safe clone has no error() calls (they should be converted to assume(false))
static bool safeCloneHasNoErrors(const Function &SafeF) {
  for (const BasicBlock &BB : SafeF) {
    for (const Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      const Function *Callee = CI->getCalledFunction();
      if (Callee && (Callee->getName() == "__VERIFIER_error" ||
                     Callee->getName() == "__CRAB_assert"))
        return false;
    }
  }
  return true;
}

// Count trimming assumptions (verifier.assume calls) in a function
static unsigned countTrimmingAssumptions(const Function &F) {
  unsigned Count = 0;
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      const Function *Callee = CI->getCalledFunction();
      if (Callee && Callee->getName() == "verifier.assume") {
        // Check it's not assume(false) from wrapping (those are in failure branches)
        if (auto *C = dyn_cast<ConstantInt>(CI->getArgOperand(0))) {
          if (!C->isZero()) {
            Count++;
          }
        } else {
          // Non-constant assumption - likely a trimming assumption
          Count++;
        }
      }
    }
  }
  return Count;
}

} // namespace

TEST(FailureDirectedTrimmingPassTest, CreatesSafeClonesAndWrapsCalls) {
  LLVMContext Ctx;
  auto M = lotus::unittest::parseAssembly(Ctx, R"IR(
; ModuleID = 'fdtrim-test'
target datalayout = "e-m:o-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx14.0.0"

declare void @__VERIFIER_error()

define i32 @foo(i32 %x) {
entry:
  %cmp = icmp slt i32 %x, 0
  br i1 %cmp, label %err, label %ok
err:
  call void @__VERIFIER_error()
  unreachable
ok:
  ret i32 %x
}

define i32 @bar(i32 %x) {
entry:
  %y = call i32 @foo(i32 %x)
  %cmp2 = icmp eq i32 %y, 0
  br i1 %cmp2, label %err, label %ok
err:
  call void @__VERIFIER_error()
  unreachable
ok:
  ret i32 %y
}
)IR");
  ASSERT_TRUE(M);
  ASSERT_FALSE(verifyModule(*M, &errs()));

  ModuleAnalysisManager MAM;
  FailureDirectedTrimmingPass Pass;
  Pass.run(*M, MAM);

  Function *Foo = M->getFunction("foo");
  Function *FooSafe = M->getFunction("foo.fdtrim.safe");
  Function *BarSafe = M->getFunction("bar.fdtrim.safe");
  ASSERT_NE(Foo, nullptr);
  ASSERT_NE(FooSafe, nullptr);
  ASSERT_NE(BarSafe, nullptr);

  Function *Bar = M->getFunction("bar");
  ASSERT_NE(Bar, nullptr);

  // The original bar should contain at least one call to the safe foo version.
  EXPECT_GE(countCallsTo(*Bar, "foo.fdtrim.safe"), 1u);

  // Stronger oracle: verify the call is properly wrapped with nondet split
  EXPECT_TRUE(verifyCallWrapping(*Bar, "foo", "foo.fdtrim.safe"));

  // Stronger oracle: verify safe clones have no error() calls
  EXPECT_TRUE(safeCloneHasNoErrors(*FooSafe));
  EXPECT_TRUE(safeCloneHasNoErrors(*BarSafe));

  // Stronger oracle: verify calls inside safe clones are rewired to safe clones
  // (bar.fdtrim.safe should call foo.fdtrim.safe, not foo)
  EXPECT_EQ(countCallsTo(*BarSafe, "foo.fdtrim.safe"), 1u);
  EXPECT_EQ(countCallsTo(*BarSafe, "foo"), 0u);

  // The module should contain verifier.assume (used by the transformation and
  // by trimming instrumentation).
  EXPECT_NE(M->getFunction("verifier.assume"), nullptr);
}

TEST(FailureDirectedTrimmingPassTest, SafeCloneConvertsCrabAssertToAssume) {
  LLVMContext Ctx;
  auto M = lotus::unittest::parseAssembly(Ctx, R"IR(
; ModuleID = 'fdtrim-assert-test'
target datalayout = "e-m:o-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx14.0.0"

declare void @__CRAB_assert(i32)

define void @foo(i32 %x) {
entry:
  call void @__CRAB_assert(i32 %x)
  ret void
}
)IR");
  ASSERT_TRUE(M);
  ASSERT_FALSE(verifyModule(*M, &errs()));

  Function *Foo = M->getFunction("foo");
  ASSERT_NE(Foo, nullptr);
  // Verify original has the assert
  EXPECT_EQ(countCallsTo(*Foo, "__CRAB_assert"), 1u);

  ModuleAnalysisManager MAM;
  FailureDirectedTrimmingPass Pass;
  Pass.run(*M, MAM);

  Function *FooSafe = M->getFunction("foo.fdtrim.safe");
  ASSERT_NE(FooSafe, nullptr);

  // Stronger oracle: verify __CRAB_assert is completely removed
  EXPECT_EQ(countCallsTo(*FooSafe, "__CRAB_assert"), 0u);
  
  // Stronger oracle: verify it's replaced with verifier.assume
  EXPECT_GE(countCallsTo(*FooSafe, "verifier.assume"), 1u);
  
  // Stronger oracle: verify the condition is preserved (assume should use %x)
  // The __CRAB_assert(i32 %x) should become assume(%x != 0) or similar
  bool FoundAssumeWithX = false;
  for (const BasicBlock &BB : *FooSafe) {
    for (const Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      const Function *Callee = CI->getCalledFunction();
      if (Callee && Callee->getName() == "verifier.assume") {
        if (CI->arg_size() >= 1) {
          Value *Arg = CI->getArgOperand(0);
          // Check if it uses the function argument (directly or through a comparison)
          std::function<bool(Value *)> usesArg = [&](Value *V) -> bool {
            if (V == FooSafe->getArg(0))
              return true;
            if (auto *I = dyn_cast<Instruction>(V)) {
              for (Use &U : I->operands()) {
                if (usesArg(U.get()))
                  return true;
              }
            }
            return false;
          };
          if (usesArg(Arg)) {
            FoundAssumeWithX = true;
            break;
          }
        }
      }
    }
    if (FoundAssumeWithX)
      break;
  }
  EXPECT_TRUE(FoundAssumeWithX) << "Expected verifier.assume to use the function argument";
}

TEST(FailureDirectedTrimmingPassTest, DefaultDerefCodegenUsesUF) {
  LLVMContext Ctx;
  auto M = lotus::unittest::parseAssembly(Ctx, R"IR(
; ModuleID = 'fdtrim-deref-test'
target datalayout = "e-m:o-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx14.0.0"

declare void @__VERIFIER_error()

define void @foo(i32* %p) {
entry:
  br label %loop

loop:
  %x = load i32, i32* %p
  %cmp = icmp eq i32 %x, 0
  br i1 %cmp, label %err, label %loop

err:
  call void @__VERIFIER_error()
  unreachable
}
)IR");
  ASSERT_TRUE(M);
  ASSERT_FALSE(verifyModule(*M, &errs()));

  Function *Foo = M->getFunction("foo");
  ASSERT_NE(Foo, nullptr);

  ModuleAnalysisManager MAM;
  FailureDirectedTrimmingPass Pass;
  Pass.run(*M, MAM);

  // The default deref mode is "uf", so inserted assumptions should reference
  // verifier.drf.trim.* helper functions rather than emitting new loads.
  EXPECT_TRUE(moduleHasFunctionWithPrefix(*M, "verifier.drf.trim."));
  EXPECT_NE(M->getFunction("verifier.assume"), nullptr);

  // Stronger oracle: verify the UF helper functions are actually used
  bool FoundUFHelperCall = false;
  for (const Function &F : *M) {
    if (F.getName().startswith("verifier.drf.trim.")) {
      // Check if this function is called somewhere
      for (const Function &Caller : *M) {
        for (const BasicBlock &BB : Caller) {
          for (const Instruction &I : BB) {
            auto *CI = dyn_cast<CallInst>(&I);
            if (!CI)
              continue;
            const Function *Callee = CI->getCalledFunction();
            if (Callee && Callee == &F) {
              FoundUFHelperCall = true;
              break;
            }
          }
          if (FoundUFHelperCall)
            break;
        }
        if (FoundUFHelperCall)
          break;
      }
      if (FoundUFHelperCall)
        break;
    }
  }
  EXPECT_TRUE(FoundUFHelperCall) << "Expected verifier.drf.trim.* functions to be called";

  // Stronger oracle: verify trimming assumptions are actually inserted
  // (not just that verifier.assume exists)
  unsigned TrimmingAssumptions = countTrimmingAssumptions(*Foo);
  EXPECT_GT(TrimmingAssumptions, 0u) 
      << "Expected trimming assumptions to be inserted in the function";
}
