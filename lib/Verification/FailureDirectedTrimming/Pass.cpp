// FailureDirectedTrimmingPass: failure-directed program trimming (FDTrim).
// Ferles et al., ESEC/FSE'17.
//
// Inserts verifier.assume(...) that prune paths provably irrelevant to
// assertion failures while preserving equi-safety (paper §3 Def. Equi-safety,
// Def. Trimmed program).
//
// Core: infer safety condition SC(π) (φ ⇒ wp(s, true)); insert assume(¬SC(π))
// as trimming condition (necessary for failure; paper §5, Theorem 5.1).
//
// Paper §5 Program Instrumentation (modular):
//   1) Safe clone f.fdtrim.safe: assert→assume, error→assume(false); internal
//   calls→safe clones. 2) Wrap call f(args): if(⋆) call f.safe(args) else {
//   call f(args); assume(false); unreachable }. 3) Compute safety conditions
//   (paper §4 Fig. 3), negate, bound/simplify (paper §6), insert assumes.

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "FailureDirectedTrimmingImpl.h"
#include "Verification/FailureDirectedTrimming/FailureDirectedTrimming.h"

#include <iterator>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Pass.h>

using namespace llvm;

bool runFailureDirectedTrimming(Module &M) {
  bool Changed = false;

  FunctionCallee AssumeFn = getVerifierAssume(M);
  NondetFactory Nondet(M);
  DerefUFFactory DerefUF(M);

  DenseMap<Function *, Function *> SafeOf = cloneSafeFunctions(M, AssumeFn);
  if (!SafeOf.empty())
    Changed = true;

  Changed |= wrapCallsInOriginalFunctions(M, AssumeFn, SafeOf, Nondet);

  auto isFailContextCall = [&](const CallBase &CB) -> bool {
    // A "failure-context" call is one created by wrapCallsInOriginalFunctions:
    // in the failure branch we emit:
    //   call f(...)
    //   assume(false)
    //   unreachable
    //
    // We use this predicate to detect functions that are ever called outside a
    // failure context; such functions are not instrumented because safe-clone
    // wrapping does not give us the same modular guarantee for their callees.
    const BasicBlock *BB = CB.getParent();
    if (!BB)
      return false;
    if (!isa<UnreachableInst>(BB->getTerminator()))
      return false;

    for (auto It = std::next(CB.getIterator()); It != BB->end(); ++It) {
      const Instruction *I = &*It;
      if (isa<DbgInfoIntrinsic>(I))
        continue;
      const auto *NextCB = dyn_cast<CallBase>(I);
      if (!NextCB)
        return false;
      Function *CF = getDirectCalledFunction(*NextCB);
      if (!CF || !isAssumeFunctionName(CF->getName()))
        return false;
      if (NextCB->arg_size() < 1)
        return false;
      if (auto *C = dyn_cast<ConstantInt>(NextCB->getArgOperand(0))) {
        return C->isZero();
      }
      return false;
    }
    return false;
  };

  DenseSet<const Function *> DoNotInstrument;
  bool HasIndirectCalls = false;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (F.getName().endswith(".fdtrim.safe"))
      continue;

    for (Instruction &I : instructions(F)) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      Function *CF = getDirectCalledFunction(*CB);
      if (!CF) {
        if (!CB->isInlineAsm())
          HasIndirectCalls = true;
        continue;
      }
      if (!SafeOf.count(CF))
        continue;
      if (!isFailContextCall(*CB))
        DoNotInstrument.insert(CF);
    }
  }

  if (HasIndirectCalls) {
    // If the module contains indirect calls, any function with its address
    // taken may be invoked without going through our call wrappers.
    // Conservatively skip instrumentation of such functions.
    for (auto &KV : SafeOf) {
      Function *F = KV.first;
      if (F && F->hasAddressTaken())
        DoNotInstrument.insert(F);
    }
  }

  lotus::AAConfig AAConfig =
      lotus::parseAAConfigFromString(FDTrimAA, lotus::AAConfig::SeaDsa());
  lotus::AliasAnalysisWrapper AA(M, AAConfig);

  HasAsrtsEnv Has = computeHasAsrts(M);

  ExprFactory EF(M.getContext());
  BoundVarManager BVM;
  SummaryEnv Env;

  if (FDTrimSummaryIterations > 0) {
    // Bounded refinement of summaries. The analysis is conservative, so it is
    // sound to stop early; more iterations may improve precision by propagating
    // stronger summaries along the call graph.
    for (unsigned It = 0; It < FDTrimSummaryIterations; ++It) {
      for (Function &F : M) {
        if (F.isDeclaration())
          continue;
        if (F.getName().endswith(".fdtrim.safe"))
          continue;

        FunctionSCResult R = computeSafetyConditions(F, EF, BVM, AA, Env, Has);
        Env.Summaries[&F] = R.Summary;
      }
    }
  }

  std::set<const Instruction *> Instrumented;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (F.getName().endswith(".fdtrim.safe"))
      continue;
    if (!SafeOf.count(&F))
      continue;
    if (DoNotInstrument.count(&F))
      continue;

    FunctionSCResult R = computeSafetyConditions(F, EF, BVM, AA, Env, Has);

    std::vector<Instruction *> Points;
    Points.reserve(64);

    if (FDTrimInstrumentCalls) {
      for (Instruction &I : instructions(F)) {
        if (isa<DbgInfoIntrinsic>(&I))
          continue;
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;
        if (Function *CF = getDirectCalledFunction(*CB)) {
          if (isAssumeFunctionName(CF->getName()) ||
              isAssumeNotFunctionName(CF->getName()) ||
              isAssertFunctionName(CF->getName()) ||
              isErrorFunctionName(CF->getName()) ||
              isNondetFunctionName(CF->getName()))
            continue;
        }
        Points.push_back(&I);
      }
    }

    if (FDTrimInstrumentConditionals) {
      for (BasicBlock &BB : F) {
        Instruction *T = BB.getTerminator();
        if (isa<BranchInst>(T) || isa<SwitchInst>(T))
          Points.push_back(T);
      }
    }

    if (FDTrimInstrumentLoops) {
      DominatorTree DT;
      DT.recalculate(F);
      LoopInfo LI;
      LI.analyze(DT);
      std::function<void(Loop *)> Visit = [&](Loop *L) {
        if (!L)
          return;
        if (BasicBlock *Header = L->getHeader()) {
          Instruction *InsPt = firstNonPhiNonDbg(*Header);
          Points.push_back(InsPt);
        }
        for (Loop *Sub : L->getSubLoops())
          Visit(Sub);
      };
      for (Loop *L : LI)
        Visit(L);
    }

    std::sort(Points.begin(), Points.end());
    Points.erase(std::unique(Points.begin(), Points.end()), Points.end());

    for (Instruction *P : Points) {
      if (!P || !P->getParent())
        continue;
      if (Instrumented.count(P))
        continue;
      auto It = R.BeforeInst.find(P);
      if (It == R.BeforeInst.end())
        continue;

      // Paper §5: TC(π) = ¬SC(π); paper §6: bound conjuncts (weaker = sound).
      ExprRef Safety = It->second;
      ExprRef TrimCond = negateForTrimming(EF, Safety);
      TrimCond = boundConjuncts(EF, TrimCond, FDTrimMaxConjuncts);

      if (FDTrimQuantElim == "z3") {
        // Optional Z3 quantifier elimination on the trimming condition.
        // This can reduce the amount of nondeterminism introduced later.
        if (ExprRef QE = tryEliminateExistsByZ3QE(EF, M, TrimCond))
          TrimCond = QE;
      }

      if (TrimCond && TrimCond->Kind == ExprKind::BoolConst &&
          TrimCond->BoolVal) {
        Instrumented.insert(P);
        continue;
      }

      IRBuilder<> B(P);
      DenseMap<uint32_t, Value *> BoundVals;
      ExprRef NoExists =
          eliminateExistsByNondet(EF, TrimCond, B, M, Nondet, BoundVals);
      Value *CondV =
          codegenValue(EF, NoExists, B, BoundVals, M, Nondet, DerefUF);
      if (!CondV)
        continue;
      if (auto *C = dyn_cast<ConstantInt>(CondV)) {
        if (C->isOne()) {
          Instrumented.insert(P);
          continue;
        }
      }

      B.CreateCall(AssumeFn, CondV);
      Instrumented.insert(P);
      Changed = true;
    }
  }

  return Changed;
}

PreservedAnalyses FailureDirectedTrimmingPass::run(Module &M,
                                                   ModuleAnalysisManager &) {
  bool Changed = runFailureDirectedTrimming(M);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

namespace {

class FailureDirectedTrimmingLegacyPass : public ModulePass {
public:
  static char ID;
  FailureDirectedTrimmingLegacyPass() : ModulePass(ID) {}

  bool runOnModule(Module &M) override { return runFailureDirectedTrimming(M); }
};

char FailureDirectedTrimmingLegacyPass::ID = 0;
static RegisterPass<FailureDirectedTrimmingLegacyPass>
    X("fdtrim", "Failure-directed program trimming", false, false);

} // namespace
