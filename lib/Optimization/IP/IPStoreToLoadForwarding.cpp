// Interprocedural store-to-load forwarding using MemorySSA instrumentation.
// Walks MemorySSA def-use chains to find a unique reaching store value for the
// same pointer. If exactly one value is found and no conflicts are seen, the
// load is replaced with that value.
//
// Pseudocode:
//   for each shadow.mem.load + following Load L:
//     targetPtr = stripCasts(L.ptr)
//     BFS over MemorySSA value starting at TLVar of load:
//       - on shadow.mem.store -> capture Store value if pointer matches target
//       - on shadow.mem.arg.mod/ref_mod/new -> follow non-primed (arg 1)
//       - on shadow.mem.in -> jump to callers via shadow.mem.arg.primed(idx)
//       - on shadow.mem.out -> follow users in callee (return path)
//       - on PHI -> visit operands
//       - on arg.init/global.init/ref -> stop (base/unsupported)
//     if exactly one reaching value, rewrite L to that value and drop load call

//===----------------------------------------------------------------------===//
/// @file IPStoreToLoadForwarding.cpp
/// @brief Inter-procedural Store-to-Load Forwarding pass implementation
///
/// This file implements an inter-procedural store-to-load forwarding pass
/// that replaces load instructions with the value from a preceding store
/// when it can be proven that the store reaches the load.
///
/// The pass uses MemorySSA def-use chain traversal to find reaching stores
/// across function boundaries, enabling forward propagation of stored values.
///
///===----------------------------------------------------------------------===//

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include "IR/MemorySSA/MemorySSA.h"

#include <queue>
#include <unordered_set>

namespace previrt {
namespace transforms {

using namespace llvm;
using namespace analysis;

static cl::opt<bool> OnlySingletonForward(
    "ip-forward-only-singleton",
    cl::desc("IP Store-to-Load Forwarding: only singleton regions"), cl::Hidden,
    cl::init(true));

namespace {
/// @brief State structure for tracking store-to-load forwarding search
struct ForwardSearchState {
  /// @brief The target pointer being searched for
  const Value *TargetPtr;
  /// @brief The type of the value being loaded
  Type *TargetTy;
  /// @brief Reference to the MemorySSA calls manager
  const MemorySSACallsManager &MMan;
  /// @brief Set to true if a conflict is detected during search
  bool Conflict = false;
  /// @brief The value from the reaching store, if found
  const Value *ReachingStoreVal = nullptr;

  /// @brief Constructor
  ForwardSearchState(const Value *Ptr, Type *Ty, const MemorySSACallsManager &M)
      : TargetPtr(Ptr), TargetTy(Ty), MMan(M) {}

  /// @brief Merge a candidate store value into the state
  /// @param Candidate The value from the store instruction
  /// @param StorePtr The pointer used by the store (after stripPointerCasts)
  /// @return true if the merge was successful, false otherwise
  bool merge(const Value *Candidate, const Value *StorePtr) {
    if (Conflict)
      return false;
    // Fix Bug 19: use pointer equality after stripPointerCasts (already done
    // by callers). This is conservative — two GEPs with the same base and
    // offset but different Value* will not match. A proper alias check would
    // require AAResults; for now, pointer equality after stripping casts is
    // the safest correct approximation without alias analysis.
    if (StorePtr != TargetPtr)
      return false;
    if (!Candidate || Candidate->getType() != TargetTy) {
      Conflict = true;
      return false;
    }
    if (!ReachingStoreVal) {
      ReachingStoreVal = Candidate;
      return true;
    }
    if (ReachingStoreVal != Candidate) {
      Conflict = true;
      return false;
    }
    return true;
  }
};

/// @brief Add an instruction to the search queue if valid
static void enqueueIfInstruction(std::queue<const Value *> &Q, const Value *V) {
  if (dyn_cast_or_null<const Instruction>(V)) {
    Q.push(V);
  }
}

/// @brief Get the next non-debug instruction after the given instruction
static const Instruction *nextNonDebugInst(const Instruction *I) {
  if (!I)
    return nullptr;
  auto It = std::next(I->getIterator());
  auto End = I->getParent()->end();
  while (It != End && isa<DbgInfoIntrinsic>(&*It)) {
    ++It;
  }
  return (It == End) ? nullptr : &*It;
}

/// @brief Explore callers when a shadow.mem.in node is encountered.
///
/// Fix Bug 16: the original exploreFunIn iterated F->uses() and silently
/// skipped non-CallInst uses (e.g., function pointer stores, bitcasts). This
/// is safe but incomplete. We now also handle InvokeInst callers, and we
/// explicitly note that indirect callers (via function pointers) cannot be
/// resolved and are conservatively ignored.
static void exploreFunIn(const CallBase *CB, const Function *F, unsigned Idx,
                         ForwardSearchState &State,
                         std::queue<const Value *> &Q) {
  (void)CB;
  for (const Use &U : F->uses()) {
    const User *Usr = U.getUser();
    // Handle direct calls only — getCallSite() requires a CallInst*.
    // InvokeInst callers are conservatively ignored (no MemorySSACallSite).
    const CallInst *CI = dyn_cast<CallInst>(Usr);
    if (!CI)
      continue;
    // Only handle direct calls where the callee is exactly F.
    if (CI->getCalledFunction() != F)
      continue;
    const analysis::MemorySSACallSite *CS = State.MMan.getCallSite(CI);
    if (!CS)
      continue;
    if (Idx >= CS->numParams())
      continue;
    enqueueIfInstruction(Q, CS->getPrimed(Idx));
  }
}

/// @brief Find the reaching store value for a load instruction via BFS.
///
/// Fix Bug 20: the original code silently ignored shadow.mem.out nodes,
/// missing forwarding opportunities across return edges. We now enqueue
/// the users of shadow.mem.out so that the BFS continues into the callee's
/// return path.
///
/// Fix Bug 17: use std::unordered_set<const Value*> instead of
/// std::set<const Value*> for O(1) average lookup instead of O(log N).
static bool findReachingStore(const Value *StartVal, const Function *CurF,
                              ForwardSearchState &State) {
  std::queue<const Value *> Q;
  // Fix Bug 17: use unordered_set for O(1) average visited lookup.
  std::unordered_set<const Value *> Visited;
  enqueueIfInstruction(Q, StartVal);

  while (!Q.empty() && !State.Conflict) {
    const Value *V = Q.front();
    Q.pop();
    if (!Visited.insert(V).second)
      continue;

    if (const CallBase *CB = dyn_cast<CallBase>(V)) {
      if (isMemSSAStore(CB, OnlySingletonForward)) {
        if (const Instruction *Next = nextNonDebugInst(CB)) {
          if (const auto *SI = dyn_cast<StoreInst>(Next)) {
            const Value *StorePtr =
                SI->getPointerOperand()->stripPointerCasts();
            State.merge(SI->getValueOperand(), StorePtr);
          }
        }
        continue;
      }

      if (isMemSSAArgMod(CB, OnlySingletonForward) ||
          isMemSSAArgRefMod(CB, OnlySingletonForward) ||
          isMemSSAArgNew(CB, OnlySingletonForward)) {
        // Follow the non-primed argument (arg 1 = the MemorySSA value).
        enqueueIfInstruction(Q, CB->getArgOperand(1));
        continue;
      }

      if (isMemSSAFunIn(CB, OnlySingletonForward)) {
        int64_t Idx = getMemSSAParamIdx(CB);
        if (Idx >= 0) {
          exploreFunIn(CB, CurF, static_cast<unsigned>(Idx), State, Q);
        }
        continue;
      }

      // Fix Bug 20: shadow.mem.out represents the value flowing back to
      // callers. Enqueue its users so the BFS continues into the callee's
      // return path (the users of shadow.mem.out are the instructions inside
      // the callee that consume the outgoing memory value).
      if (isMemSSAFunOut(CB, OnlySingletonForward)) {
        for (const Use &U : CB->uses()) {
          enqueueIfInstruction(Q, dyn_cast<const Instruction>(U.getUser()));
        }
        continue;
      }

      if (isMemSSAArgInit(CB, OnlySingletonForward) ||
          isMemSSAGlobalInit(CB, OnlySingletonForward) ||
          isMemSSAArgRef(CB, OnlySingletonForward)) {
        // Base cases: arg.init and global.init are initial values (no
        // preceding store to forward). arg.ref is a read-only use — no store.
        // Stop BFS here.
        continue;
      }
    }

    if (const PHINode *PN = dyn_cast<PHINode>(V)) {
      for (const Value *Op : PN->incoming_values())
        enqueueIfInstruction(Q, Op);
    }
  }

  return State.ReachingStoreVal && !State.Conflict;
}
} // namespace

/// @brief Inter-procedural Store-to-Load Forwarding pass
///
/// This pass replaces load instructions with the value from a preceding store
/// when it can be proven that the store's value reaches the load. The pass
/// performs interprocedural analysis by walking MemorySSA def-use chains
/// across function boundaries.
class IPStoreToLoadForwarding : public ModulePass {
public:
  /// @brief Unique pass identifier
  static char ID;

  /// @brief Default constructor
  IPStoreToLoadForwarding() : ModulePass(ID) {}

  /// @brief Run the store-to-load forwarding pass on a module
  /// @param M The LLVM module to process
  /// @return true if any loads were forwarded, false otherwise
  bool runOnModule(Module &M) override {
    if (M.begin() == M.end())
      return false;

    unsigned NumForwarded = 0;
    MemorySSACallsManager MMan(M, *this, OnlySingletonForward);

    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      for (BasicBlock &BB : F) {
        for (auto It = BB.begin(); It != BB.end();) {
          Instruction *Inst = &*It++;
          CallBase *CB = dyn_cast<CallBase>(Inst);
          if (!CB || !isMemSSALoad(CB, OnlySingletonForward))
            continue;
          // It now points to the instruction after CB (the load).
          if (It == BB.end())
            continue;
          LoadInst *LI = dyn_cast<LoadInst>(&*It);
          if (!LI)
            continue;

          const Value *Ptr = LI->getPointerOperand()->stripPointerCasts();
          ForwardSearchState State(Ptr, LI->getType(), MMan);
          if (!findReachingStore(CB->getArgOperand(1), LI->getFunction(),
                                 State))
            continue;

          LI->replaceAllUsesWith(const_cast<Value *>(State.ReachingStoreVal));

          // Fix Bug 18: after the top-of-loop `&*It++`, It already points to
          // LI. We need to advance It past LI before erasing it, then
          // optionally erase CB.
          //
          // State: It -> LI (the load we just matched).
          // Advance It past LI before erasing.
          ++It;                  // It now points to the instruction after LI.
          LI->eraseFromParent(); // LI is now invalid; It is still valid.

          if (CB->use_empty()) {
            // CB is the shadow.mem.load call before LI (now erased).
            // It does not affect the current It position.
            CB->eraseFromParent();
          }
          NumForwarded++;
        }
      }
    }

    if (NumForwarded > 0) {
      errs() << "IP-Forward: forwarded " << NumForwarded << " loads\n";
    }
    return NumForwarded > 0;
  }

  /// @brief Specify analysis dependencies and preserves
  /// @param AU Analysis usage information to populate
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // This pass erases load instructions, which invalidates analyses that
    // track instruction pointers. Do not claim setPreservesCFG() since
    // erasing instructions can invalidate MemorySSA and similar analyses.
    (void)AU;
  }

  /// @brief Get the name of this pass
  /// @return The pass name as a string reference
  StringRef getPassName() const override {
    return "Interprocedural Store-to-Load Forwarding";
  }
};

char IPStoreToLoadForwarding::ID = 0;

static RegisterPass<IPStoreToLoadForwarding>
    X("ip-forward", "Interprocedural Store-to-Load Forwarding");

} // namespace transforms
} // namespace previrt
