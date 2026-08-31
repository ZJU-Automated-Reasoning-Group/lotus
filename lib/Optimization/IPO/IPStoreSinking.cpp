#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include "IR/ShadowMemSSA/ShadowMemSSA.h"

//===----------------------------------------------------------------------===//
/// @file IPStoreSinking.cpp
/// @brief Inter-procedural Store Sinking pass implementation
///
/// This file implements a conservative store sinking pass that moves store
/// instructions closer to their uses within a basic block while preserving
/// program semantics.
///
/// Store sinking reduces register pressure by moving stores as close as
/// possible to their first observable use, while ensuring that no side-effect
/// free instructions are moved past.
///
///===----------------------------------------------------------------------===//

namespace previrt {
namespace transforms {

using namespace llvm;
using namespace analysis;

static cl::opt<bool> OnlySingletonSink(
    "ip-sink-only-singleton",
    cl::desc("IP Store Sinking: only singleton memory regions"), cl::Hidden,
    cl::init(true));

// Conservative store sinking that keeps stores before their first observable
// use while moving them closer to that use. We only sink inside a single basic
// block and only past instructions that are side-effect free.
//
// Pseudocode:
//   for each shadow.mem.store + Store S pair in BB:
//     find earliest user U of the shadow.mem value inside BB that is after S
//     if no U: skip
//     verify no instruction between S and U:
//       - reads/writes memory (mayReadOrWriteMemory)
//       - is a terminator
//       - writes to the same memory location as S (alias check)
//     if safe: move S before U, move shadow.mem.store just before S
//

/// @brief Inter-procedural Store Sinking pass
///
/// This pass moves store instructions closer to their uses within a basic
/// block. It is conservative and only sinks stores past side-effect-free
/// instructions to maintain program semantics.
class IPStoreSinking : public ModulePass {
public:
  /// @brief Unique pass identifier
  static char ID;

  /// @brief Default constructor
  IPStoreSinking() : ModulePass(ID) {}

  /// @brief Run the store sinking pass on a module
  /// @param M The LLVM module to process
  /// @return true if any stores were sunk, false otherwise
  bool runOnModule(Module &M) override {
    if (M.begin() == M.end())
      return false;

    unsigned NumSunk = 0;

    for (Function &F : M) {
      if (F.isDeclaration())
        continue;

      for (BasicBlock &BB : F) {
        bool Changed = true;
        while (Changed) {
          Changed = false;
          for (auto It = BB.begin(), Et = BB.end(); It != Et; ++It) {
            CallBase *CB = dyn_cast<CallBase>(&*It);
            if (!CB || !isShadowMemStore(CB, OnlySingletonSink))
              continue;

            auto NextIt = std::next(It);
            if (NextIt == BB.end())
              continue;
            StoreInst *SI = dyn_cast<StoreInst>(&*NextIt);
            if (!SI)
              continue;

            // Find the earliest user of CB (shadow.mem.store) in this block
            // that comes after SI.
            Instruction *FirstUser = nullptr;
            for (Use &U : CB->uses()) {
              if (Instruction *UI = dyn_cast<Instruction>(U.getUser())) {
                if (UI->getParent() != &BB)
                  continue;
                if (UI == CB || UI == SI)
                  continue;
                if (!SI->comesBefore(UI))
                  continue;
                if (FirstUser == nullptr || UI->comesBefore(FirstUser)) {
                  FirstUser = UI;
                }
              }
            }

            if (!FirstUser)
              continue;

            if (std::next(NextIt) == BB.end() || &*std::next(NextIt) == FirstUser) {
              continue;
            }

            bool Safe = true;
            const Value *StorePtr =
                SI->getPointerOperand()->stripPointerCasts();
            for (auto MoveIt = std::next(NextIt);
                 MoveIt != BB.end() && &*MoveIt != FirstUser; ++MoveIt) {
              Instruction *Between = &*MoveIt;
              if (Between->isTerminator()) {
                Safe = false;
                break;
              }

              if (Between->mayReadOrWriteMemory()) {
                // Allow shadow.mem marker calls through — they are bookkeeping,
                // not real memory operations.
                bool IsShadowMem = false;
                if (const CallBase *CB2 = dyn_cast<CallBase>(Between)) {
                  if (CB2->getCalledFunction() &&
                      CB2->getCalledFunction()->getName().startswith(
                          "shadow.mem")) {
                    IsShadowMem = true;
                  }
                }
                if (!IsShadowMem) {
                  Safe = false;
                  break;
                }
              }

            }

            if (!Safe)
              continue;

            // Perform the sink: move SI and CB just before FirstUser.
            SI->moveBefore(FirstUser);
            CB->moveBefore(SI);
            NumSunk++;
            Changed = true;
            break;
          }
        }
      }
    }

    if (NumSunk > 0) {
      errs() << "IP-Sink: sunk " << NumSunk << " stores\n";
    }
    return NumSunk > 0;
  }

  /// @brief Specify analysis dependencies and preserves
  /// @param AU Analysis usage information to populate
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // This pass moves instructions within a basic block but does not change
    // the CFG structure (no edges added/removed). setPreservesCFG() is correct
    // here since we only reorder instructions, not erase them.
    AU.setPreservesCFG();
  }

  /// @brief Get the name of this pass
  /// @return The pass name as a string reference
  StringRef getPassName() const override {
    return "Interprocedural Store Sinking";
  }
};

char IPStoreSinking::ID = 0;

static RegisterPass<IPStoreSinking> X("ip-sink",
                                      "Interprocedural Store Sinking");

llvm::ModulePass *createIPStoreSinkingPass() { return new IPStoreSinking(); }

} // namespace transforms
} // namespace previrt
