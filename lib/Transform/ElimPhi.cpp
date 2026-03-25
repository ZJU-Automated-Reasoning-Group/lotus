//===- ElimPhi.cpp - Convert SSA PHIs to stores/loads ----------*- C++ -*-===//
// This file transforms PHI nodes into explicit memory operations,
// yielding IR that is no longer in SSA form (useful for visualisation
// or certain back-ends; never run this late in the real LLVM pipeline).
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
// #include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "elim-phi"

using namespace llvm;

namespace {

class EliminatePHIPass : public PassInfoMixin<EliminatePHIPass> {
  DenseMap<PHINode *, AllocaInst *> SlotForPhi;

  /* Create (or reuse) the stack slot that will represent the PHI and
     replace all uses of the PHI with a load in the PHI block. */
  AllocaInst *materialiseSlot(PHINode *PN, IRBuilder<> &Builder) {
    auto It = SlotForPhi.find(PN);
    if (It != SlotForPhi.end())
      return It->second;

    Function *F = PN->getFunction();
    BasicBlock &Entry = F->getEntryBlock();

    /* Insert the alloca as the very first non-alloca instruction so that
       optimisers can easily promote it again if desired. */
    IRBuilder<> EntryBuilder(&*Entry.getFirstInsertionPt());
    AllocaInst *AI = EntryBuilder.CreateAlloca(
        PN->getType(), nullptr,
        PN->hasName() ? PN->getName() + ".slot" : "phi.slot");

    /* Insert a load right after the last PHI in the PHI block. */
    Builder.SetInsertPoint(PN->getParent()->getFirstNonPHI());
    LoadInst *Ld = Builder.CreateLoad(
        PN->getType(), AI, PN->hasName() ? PN->getName() + ".val" : "phi.val");
    PN->replaceAllUsesWith(Ld);

    SlotForPhi[PN] = AI;
    return AI;
  }

  /* Ensure the edge PredBB -> SuccBB is split so we can safely insert a store.
     Returns the block we should insert into (either SuccBB or the new one). */
  BasicBlock *ensureEdgeForStore(BasicBlock *PredBB, BasicBlock *SuccBB,
                                 unsigned SuccIdx) {
    auto *TI = PredBB->getTerminator();

    /* An edge is critical if the predecessor has >1 successor and the successor
       has >1 predecessor. */
    bool Critical =
        (TI->getNumSuccessors() > 1) && !SuccBB->getSinglePredecessor();

    if (!Critical)
      return PredBB; // we can insert the store *before* the terminator

    LLVM_DEBUG(dbgs() << "  splitting critical edge " << PredBB->getName()
                      << " -> " << SuccBB->getName() << '\n');

    Function *F = PredBB->getParent();
    BasicBlock *EdgeBB =
        BasicBlock::Create(F->getContext(), "phi.store", F, SuccBB);
    IRBuilder<> B(EdgeBB);
    B.CreateBr(SuccBB);

    TI->setSuccessor(SuccIdx, EdgeBB);
    return EdgeBB;
  }

public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    bool Changed = false;
    SlotForPhi.clear();

    // Conservative safety check: this transform currently handles only
    // predecessor edges from BranchInst terminators.
    SmallPtrSet<BasicBlock *, 8> PhiBlocks;
    for (auto &BB : F)
      if (isa<PHINode>(BB.begin()))
        PhiBlocks.insert(&BB);

    for (BasicBlock *SuccBB : PhiBlocks) {
      for (BasicBlock *PredBB : predecessors(SuccBB)) {
        if (!isa<BranchInst>(PredBB->getTerminator())) {
          LLVM_DEBUG(dbgs()
                     << "  skipping elim-phi for function " << F.getName()
                     << ": unsupported non-branch predecessor edge\n");
          return PreservedAnalyses::all();
        }
      }
    }

    /* Collect all branch terminators first (iterator invalidation safety). */
    SmallVector<BranchInst *, 8> WorkList;
    for (auto &BB : F)
      if (auto *BI = dyn_cast<BranchInst>(BB.getTerminator()))
        WorkList.push_back(BI);

    for (BranchInst *BI : WorkList) {
      BasicBlock *PredBB = BI->getParent();
      for (unsigned i = 0, e = BI->getNumSuccessors(); i != e; ++i) {
        BasicBlock *SuccBB = BI->getSuccessor(i);
        if (!isa<PHINode>(SuccBB->begin()))
          continue; // nothing to do

        /* Choose the block in which we will emit the store(s). */
        BasicBlock *StoreBB = ensureEdgeForStore(PredBB, SuccBB, i);
        IRBuilder<> Builder(StoreBB == PredBB
                                ? (Instruction *)BI
                                : &*StoreBB->getFirstInsertionPt());

        /* Rewrite every PHI in the successor. */
        SmallVector<PHINode *, 8> Phis;
        for (auto &I : SuccBB->phis())
          Phis.push_back(&I);

        for (PHINode *PN : Phis) {
          AllocaInst *Slot = materialiseSlot(PN, Builder);
          Value *Incoming = PN->getIncomingValueForBlock(PredBB);
          Builder.CreateStore(Incoming, Slot);
          PN->removeIncomingValue(PredBB, /*DeletePHIIfEmpty=*/false);
          Changed = true;
        }
      }
    }

    /* Any PHI that has lost all incoming edges can now be deleted. */
    SmallVector<PHINode *, 8> DeadPhis;
    for (auto &BB : F)
      for (auto &PN : BB.phis())
        if (PN.getNumIncomingValues() == 0)
          DeadPhis.push_back(&PN);
    for (PHINode *PN : DeadPhis)
      PN->eraseFromParent();

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                     Pass registration glue
//===----------------------------------------------------------------------===//

/* --- Legacy Pass Manager (opt -elim-phi) --------------------------------- */
namespace {
struct LegacyEliminatePHIPass : public FunctionPass {
  static char ID;
  LegacyEliminatePHIPass() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) override {
    EliminatePHIPass Impl;
    FunctionAnalysisManager DummyFAM;
    PreservedAnalyses PA = Impl.run(F, DummyFAM);
    return !PA.areAllPreserved();
  }
};
} // end anonymous namespace

char LegacyEliminatePHIPass::ID = 0;
static RegisterPass<LegacyEliminatePHIPass>
    X("elim-phi", "Eliminate PHI nodes (non-SSA transform)");
