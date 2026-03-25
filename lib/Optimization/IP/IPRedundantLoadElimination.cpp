#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include "IR/MemorySSA/MemorySSA.h"

//===----------------------------------------------------------------------===//
/// @file IPRedundantLoadElimination.cpp
/// @brief Inter-procedural Redundant Load Elimination pass implementation
///
/// This file implements an inter-procedural redundant load elimination pass
/// that removes repeated loads from the same memory location within a basic
/// block when it is safe to do so.
///
/// The pass uses MemorySSA instrumentation to track memory operations and
/// identify redundant loads. It is conservative and only performs local
/// (within-basic-block) redundancy elimination, relying on the MemorySSA
/// TLVars to encode interprocedural effects.
///
///===----------------------------------------------------------------------===//

namespace previrt {
namespace transforms {

using namespace llvm;
using namespace analysis;

static cl::opt<bool>
    OnlySingletonRLE("ip-rle-only-singleton",
                     cl::desc("IP RLE: consider only singleton memory regions"),
                     cl::Hidden, cl::init(true));

// Interprocedural redundant load elimination using MemorySSA instrumentation.
// Conservative: only removes repeated loads within a basic block when the
// MemorySSA version (TLVar) and pointer operand are identical and there are no
// intervening memory writes. This benefits interprocedural code because TLVars
// already encode effects across calls.
//
// Pseudocode:
//   for each basic block BB:
//     seen = {} // (TLVar, Ptr) -> dominating load
//     for inst I in BB:
//       if I is shadow.mem.load and next inst is Load L:
//         key = (TLVar, stripCasts(L.ptr))
//         if key in seen: replace L with seen[key], drop L and maybe load call
//         else: seen[key] = L
//       elif I mayWriteToMemory (but is NOT a shadow.mem.load): seen.clear()
//
// Fix Bug 10: the original code called resetSeen() on every
// mayReadOrWriteMemory() instruction, which includes shadow.mem.load calls
// themselves — clearing the cache just before checking for a redundant load.
// The fix is to only invalidate the cache on instructions that *write* to
// memory (and are not shadow.mem markers), since reads cannot change the
// memory state that the TLVar encodes.

/// @brief Inter-procedural Redundant Load Elimination pass
///
/// This pass identifies and removes redundant load instructions within basic
/// blocks. A load is considered redundant if there is an earlier load from
/// the same pointer with the same MemorySSA version (TLVar) and no intervening
/// memory writes.
///
/// The pass is conservative and operates only within basic blocks to ensure
/// correctness. Interprocedural effects are handled through MemorySSA's TLVars
/// which encode memory state across function calls.
class IPRedundantLoadElimination : public ModulePass {
public:
  /// @brief Unique pass identifier
  static char ID;

  /// @brief Default constructor
  IPRedundantLoadElimination() : ModulePass(ID) {}

  /// @brief Run the redundant load elimination pass on a module
  /// @param M The LLVM module to process
  /// @return true if any loads were eliminated, false otherwise
  bool runOnModule(Module &M) override {
    if (M.begin() == M.end())
      return false;

    unsigned NumRemoved = 0;
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      for (BasicBlock &BB : F) {
        // Map from (TLVar, Ptr) to the dominating load instruction.
        DenseMap<std::pair<const Value *, const Value *>, LoadInst *> SeenLoads;

        for (auto It = BB.begin(), Et = BB.end(); It != Et;) {
          Instruction *I = &*It;

          // Fix Bug 10: only invalidate the cache on instructions that write
          // to memory. Pure reads (including shadow.mem.load) do not change
          // the memory state encoded by TLVars, so they should not clear the
          // cache. Shadow.mem calls are intrinsics that report mayReadOrWrite,
          // but they are markers — not real memory operations — so we skip
          // them for invalidation purposes.
          if (I->mayWriteToMemory()) {
            // If this is a shadow.mem marker, do not invalidate — it is a
            // bookkeeping call, not a real write.
            bool IsShadowMem = false;
            if (const CallBase *CB2 = dyn_cast<CallBase>(I)) {
              if (CB2->getCalledFunction() &&
                  CB2->getCalledFunction()->getName().startswith(
                      "shadow.mem")) {
                IsShadowMem = true;
              }
            }
            if (!IsShadowMem) {
              SeenLoads.clear();
            }
          }

          CallBase *CB = dyn_cast<CallBase>(I);
          if (!CB || !isMemSSALoad(CB, OnlySingletonRLE)) {
            ++It;
            continue;
          }

          auto NextIt = std::next(It);
          if (NextIt == BB.end()) {
            ++It;
            continue;
          }
          LoadInst *LI = dyn_cast<LoadInst>(&*NextIt);
          if (!LI) {
            ++It;
            continue;
          }

          const Value *TLVar = CB->getArgOperand(1);
          const Value *Ptr = LI->getPointerOperand()->stripPointerCasts();
          auto Key = std::make_pair(TLVar, Ptr);

          auto Found = SeenLoads.find(Key);
          if (Found == SeenLoads.end()) {
            SeenLoads.insert({Key, LI});
            ++It;
            continue;
          }

          // We have an earlier dominating load with the same TLVar and pointer.
          LoadInst *DomLoad = Found->second;
          LI->replaceAllUsesWith(DomLoad);

          // Fix Bug 11: save a stable iterator before erasing instructions.
          // After erasing LI (NextIt), NextIt is dangling — do not use
          // std::prev(NextIt). Instead, advance It past CB before any erasure,
          // then erase LI and optionally CB.
          //
          // Current state: It -> CB, NextIt -> LI
          // We want to continue from the instruction after LI.
          auto AfterLI = std::next(NextIt);

          LI->eraseFromParent(); // NextIt (LI) is now invalid.

          if (CB->use_empty()) {
            // CB is at It; erase it and set It to AfterLI.
            CB->eraseFromParent(); // It (CB) is now invalid.
            It = AfterLI;
          } else {
            // CB still has uses; advance past it normally.
            It = AfterLI;
          }

          NumRemoved++;
        }
      }
    }

    if (NumRemoved > 0) {
      errs() << "IP-RLE: removed " << NumRemoved << " redundant loads\n";
    }
    return NumRemoved > 0;
  }

  /// @brief Specify analysis dependencies and preserves
  /// @param AU Analysis usage information to populate
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // Fix Bug 12: do NOT call setPreservesCFG() — this pass erases load
    // instructions, which invalidates analyses that track instruction pointers
    // (e.g., MemorySSA, DominatorTree). Declare no preserved analyses so the
    // pass manager invalidates them correctly.
    (void)AU;
  }

  /// @brief Get the name of this pass
  /// @return The pass name as a string reference
  StringRef getPassName() const override {
    return "Interprocedural Redundant Load Elimination";
  }
};

char IPRedundantLoadElimination::ID = 0;

static RegisterPass<IPRedundantLoadElimination>
    X("ip-rle", "Interprocedural Redundant Load Elimination");

} // namespace transforms
} // namespace previrt
