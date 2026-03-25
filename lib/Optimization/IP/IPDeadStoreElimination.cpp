/*
   Inter-procedural Dead Store Elimination (IP-DSE) using ShadowMem/MemorySSA.

   Intent:
     Drop stores (and some global initializers) whose MemorySSA def-use chains
     never reach a shadow.mem.load. Works across calls via shadow.mem.arg.*,
     shadow.mem.in/out.

   Pseudocode (high level):
     worklist = { all shadow.mem.store, all global init markers }
     mark all their concrete stores/inits as "removable by default"
     while worklist not empty:
       pop <shadowMemInst, origin, len>
       if origin already proven needed: continue
       if shadowMemInst has a shadow.mem.load user: mark origin keep; continue
       if len == max_len: mark origin keep; continue
       for each user U of shadowMemInst:
         if U is PHI: enqueue(U, origin, len+1)
         else if U is shadow.mem.arg.mod/ref_mod: jump into callee via
             shadow.mem.in to corresponding formal and enqueue
         else if U is shadow.mem.out: jump back to callers via arg.primed
         else if U is shadow.mem.arg.ref: mark keep (read-only use)
         else if U is another shadow.mem.store: skip (kills forwarding)
         else: warn/ignore
     erase stores still marked removable; tag useless global initializers
     strip all shadow.mem calls
 */

//===----------------------------------------------------------------------===//
/// @file IPDeadStoreElimination.cpp
/// @brief Inter-procedural Dead Store Elimination pass implementation
///
/// This file implements an inter-procedural dead store elimination pass that
/// removes memory stores that are never read. The pass uses SeaDSA's ShadowMem
/// instrumentation and MemorySSA to track def-use chains across function
/// boundaries.
///
/// The algorithm works by:
/// 1. Identifying all store instructions and global initializers
/// 2. Walking def-use chains backwards from each store
/// 3. Determining if the store value ever reaches a load
/// 4. Removing stores that are proven dead
///
/// @note This pass requires SeaDSA's ShadowMem pass to be run first to
///       instrument the code with shadow.mem calls.
///
///===----------------------------------------------------------------------===//

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"

#include "Alias/seadsa/InitializePasses.hh"
#include "Alias/seadsa/ShadowMem.hh"
#include "IR/MemorySSA/MemorySSA.h"

#include <cstddef>
#include <functional>
#include <unordered_set>

static llvm::cl::opt<bool> OnlySingleton(
    "ipdse-only-singleton",
    llvm::cl::desc(
        "IP DSE: remove store only if operand is a singleton global var"),
    llvm::cl::Hidden, llvm::cl::init(true));

static llvm::cl::opt<unsigned>
    MaxLenDefUse("ipdse-max-def-use",
                 llvm::cl::desc("IP DSE: maximum length of the def-use chain"),
                 llvm::cl::Hidden, llvm::cl::init(UINT_MAX));

// #define DSE_LOG(...) __VA_ARGS__
#define DSE_LOG(...)

namespace previrt {
namespace transforms {

using namespace llvm;
using namespace analysis;

/// @brief Check if a function has a function pointer parameter
/// @param F The function to check
/// @return true if the function has a function pointer parameter, false
/// otherwise
static bool hasFunctionPtrParam(Function *F) {
  FunctionType *FTy = F->getFunctionType();
  for (unsigned i = 0, e = FTy->getNumParams(); i < e; ++i) {
    // Fix Bug 5: getPointerElementType() is deprecated in LLVM 14 with opaque
    // pointers. Use the function type's parameter type directly via
    // isFunctionTy() on the pointee, obtained through the function type
    // attribute rather than pointer element type.
    Type *ParamTy = FTy->getParamType(i);
    if (ParamTy->isPointerTy()) {
      // In opaque-pointer mode we cannot inspect the pointee type.
      // Conservatively treat any pointer parameter as potentially a function
      // pointer to avoid miscompilation.
#if LLVM_VERSION_MAJOR >= 14
      // With opaque pointers we cannot determine the pointee type; be
      // conservative and return true (skip this function).
      return true;
#else
      if (PointerType *PT = dyn_cast<PointerType>(ParamTy)) {
        if (isa<FunctionType>(PT->getPointerElementType())) {
          return true;
        }
      }
#endif
    }
  }
  return false;
}

class IPDeadStoreElimination : public ModulePass {

  static inline void hashCombine(std::size_t &seed, std::size_t value) {
    seed ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) +
            (seed << 6U) + (seed >> 2U);
  }

  /// @brief Worklist element for tracking stores during analysis
  ///
  /// This structure represents an element in the worklist used by the DSE
  /// algorithm. It tracks a shadow memory instruction and its associated
  /// original store instruction or global initializer.
  struct QueueElem {
    /// @brief Last shadow mem instruction related to storeInstOrGvInit
    const Instruction *shadowMemInst;
    /// @brief The original instruction that we want to remove if we can prove
    /// it is redundant.
    Value *storeInstOrGvInit;
    /// @brief Number of steps (i.e., shadow mem instructions connect them)
    /// between storeInstOrGvInit and shadowMemInst
    unsigned length;

    QueueElem(const Instruction *I, Value *V, unsigned Len)
        : shadowMemInst(I), storeInstOrGvInit(V), length(Len) {}

    size_t hash() const {
      std::size_t seed = 0;
      hashCombine(seed, std::hash<const void *>{}(shadowMemInst));
      hashCombine(seed, std::hash<const void *>{}(storeInstOrGvInit));
      // Fix Bug 6: include length in the hash so that the same (inst, value)
      // pair at different chain lengths is treated as a distinct worklist item.
      hashCombine(seed, std::hash<unsigned>{}(length));
      return seed;
    }

    bool operator==(const QueueElem &o) const {
      // Fix Bug 6: include length in equality so shorter paths can re-explore
      // the same (shadowMemInst, storeInstOrGvInit) pair.
      return (shadowMemInst == o.shadowMemInst &&
              storeInstOrGvInit == o.storeInstOrGvInit && length == o.length);
    }

    void write(raw_ostream &o) const {
      o << "(" << *shadowMemInst << ", " << *storeInstOrGvInit << ")";
    }

    friend raw_ostream &operator<<(raw_ostream &o, const QueueElem &e) {
      e.write(o);
      return o;
    }
  };

  struct QueueElemHasher {
    size_t operator()(const QueueElem &e) const { return e.hash(); }
  };

  template <class Q, class QE> inline void enqueue(Q &queue, QE e) {
    DSE_LOG(errs() << "\tEnqueued " << e << "\n");
    queue.push_back(e);
  }

  // Three-state map for each store/global-init value:
  //   absent  -> not yet registered (should not be deleted)
  //   false   -> registered as removable candidate
  //   true    -> proven needed (must keep)
  //
  // Fix Bug 2: use an explicit enum/optional instead of relying on DenseMap's
  // default bool initialisation (which is false, indistinguishable from
  // "marked removable").
  enum class StoreState { Removable, Keep };
  DenseMap<Value *, StoreState> m_valueMap;

  inline bool isRegistered(Value *V) const { return m_valueMap.count(V) > 0; }

  inline bool isMarkedKeep(Value *V) const {
    auto It = m_valueMap.find(V);
    return It != m_valueMap.end() && It->second == StoreState::Keep;
  }

  inline void markToKeep(Value *V) {
    m_valueMap[V] = StoreState::Keep;
    DSE_LOG(errs() << "\tKeep " << *V << "\n";);
  }

  inline void markToRemove(Value *V) {
    // Fix Bug 1: if already marked Keep (e.g., via a different path), do not
    // downgrade to Removable — just leave it as Keep.
    if (isMarkedKeep(V)) {
      return;
    }
    m_valueMap[V] = StoreState::Removable;
  }

  // Given a call to shadow.mem.arg.XXX it finds the nearest actual
  // callsite from the original program and returns the called function.
  //
  // Fix Bug 3: guard against the callsite being in a different block by
  // limiting the scan to the current block and returning nullptr gracefully
  // instead of crashing.
  const Function *findCalledFunction(const CallBase *MemSsaCB) {
    const Instruction *I = MemSsaCB;
    for (auto it = I->getIterator(), et = I->getParent()->end(); it != et;
         ++it) {
      if (const CallBase *CB = dyn_cast<const CallBase>(&*it)) {
        if (!CB->getCalledFunction()) {
          return nullptr;
        }
        if (CB->getCalledFunction()->getName().startswith("shadow.mem")) {
          continue;
        } else {
          return CB->getCalledFunction();
        }
      }
    }
    return nullptr;
  }

public:
  /// @brief Unique pass identifier
  static char ID;

  /// @brief Constructor that initializes the pass and required SeaDSA passes
  IPDeadStoreElimination() : ModulePass(ID) {
    // Initialize sea-dsa pass
    llvm::PassRegistry &Registry = *llvm::PassRegistry::getPassRegistry();
    llvm::initializeShadowMemPassPass(Registry);
  }

  /// @brief Run the dead store elimination pass on a module
  /// @param M The LLVM module to process
  /// @return true if any stores were eliminated, false otherwise
  virtual bool runOnModule(Module &M) override {
    if (M.begin() == M.end()) {
      return false;
    }

    errs() << "Started interprocedural dead store elimination...\n";

    // Populate worklist

    // --- collect all shadow.mem store instructions
    std::vector<QueueElem> queue;
    for (auto &F : M) {
      for (auto &I : instructions(&F)) {
        if (isMemSSAStore(&I, OnlySingleton)) {
          auto it = I.getIterator();
          ++it;
          auto end = I.getParent()->end();
          while (it != end && isa<DbgInfoIntrinsic>(&*it)) {
            ++it;
          }
          if (it == end) {
            continue;
          }
          if (StoreInst *SI = dyn_cast<StoreInst>(&*it)) {
            queue.push_back(QueueElem(&I, SI, 0));
            // All the store instructions will be removed unless the
            // opposite is proven.
            markToRemove(SI);
          }
          // If the shadow.mem.store is not immediately followed by a store,
          // skip it rather than crashing. This keeps the pass conservative.
          continue;
        }
      }
    }

    // --- collect all global initializers
    // Fix Bug 7: scan all functions for global init markers, not just main's
    // entry block. Global constructors may be in __attribute__((constructor))
    // functions or in functions other than main.
    auto collectGlobalInits = [&](Function *F) {
      if (!F || F->isDeclaration()) {
        return;
      }
      BasicBlock &entryBB = F->getEntryBlock();
      for (auto &I : entryBB) {
        if (isMemSSAArgInit(&I, true /*only if singleton*/) ||
            isMemSSAGlobalInit(&I,
                               false /* global.init cannot be singleton */)) {
          if (const CallBase *CB = dyn_cast<const CallBase>(&I)) {
            if (GlobalVariable *GV =
                    const_cast<GlobalVariable *>(dyn_cast<const GlobalVariable>(
                        getMemSSASingleton(CB, MemSSAOp::MEM_SSA_ARG_INIT)))) {
              if (GV->hasInitializer()) {
                queue.push_back(QueueElem(&I, GV, 0));
                markToRemove(GV);
              }
            }
          }
        }
      }
    };

    // Always check main if present.
    if (Function *main = M.getFunction("main")) {
      collectGlobalInits(main);
    }
    // Also check functions with the constructor attribute.
    for (auto &F : M) {
      if (F.hasFnAttribute("constructor") ||
          F.getName().startswith("__cxx_global_var_init") ||
          F.getName().startswith("_GLOBAL__sub_I_")) {
        collectGlobalInits(&F);
      }
    }

    // Process worklist

    unsigned numUselessStores = 0;
    unsigned numUselessGvInit = 0;
    unsigned skippedChains = 0;
    if (!queue.empty()) {
      errs() << "Number of stores: " << queue.size() << "\n";
      MemorySSACallsManager MMan(M, *this, OnlySingleton);

      DSE_LOG(errs() << "[IPDSE] BEGIN initial queue: \n";
              for (auto &e : queue) { errs() << e << "\n"; } errs()
              << "[IPDSE] END initial queue\n";);

      // A store is not useless if there is a def-use chain between a
      // store and a load instruction and there is not any other store
      // in between.
      std::unordered_set<QueueElem, QueueElemHasher> visited;
      while (!queue.empty()) {
        QueueElem w = queue.back();
        DSE_LOG(errs() << "[IPDSE] Processing " << *(w.shadowMemInst) << "\n");
        queue.pop_back();

        if (!visited.insert(w).second) {
          // this is not necessarily a cycle
          DSE_LOG(errs() << "\tAlready processed: skipped\n";);
          continue;
        }

        // Fix Bug 2: use isMarkedKeep() instead of m_valueMap[V] which
        // default-constructs to false for unregistered values.
        if (isMarkedKeep(w.storeInstOrGvInit)) {
          continue;
        }

        if (hasMemSSALoadUser(w.shadowMemInst, OnlySingleton)) {
          DSE_LOG(errs() << "\thas a load user: CANNOT be removed.\n");
          markToKeep(w.storeInstOrGvInit);
          continue;
        }

        if (w.length == MaxLenDefUse) {
          skippedChains++;
          markToKeep(w.storeInstOrGvInit);
          continue;
        }

        // w.storeInstOrGvInit is not useless if any of its direct or
        // indirect uses say it is not useless.
        for (auto &U : w.shadowMemInst->uses()) {

          // Fix Bug 2: use isMarkedKeep() for the early-exit check.
          if (isMarkedKeep(w.storeInstOrGvInit)) {
            break;
          }

          Instruction *I = dyn_cast<Instruction>(U.getUser());
          if (!I)
            continue;
          DSE_LOG(errs() << "\tChecking user " << *I << "\n");

          if (PHINode *PHI = dyn_cast<PHINode>(I)) {
            DSE_LOG(errs() << "\tPHI node: enqueuing lhs\n");
            enqueue(queue, QueueElem(PHI, w.storeInstOrGvInit, w.length + 1));
          } else if (CallBase *CB = dyn_cast<CallBase>(I)) {
            if (!CB->getCalledFunction())
              continue;
            if (isMemSSAStore(CB, OnlySingleton)) {
              DSE_LOG(errs() << "\tstore: skipped\n");
              continue;
            } else if (isMemSSAArgRef(CB, OnlySingleton)) {
              DSE_LOG(errs() << "\targ ref: CANNOT be removed\n");
              markToKeep(w.storeInstOrGvInit);
            } else if (isMemSSAArgMod(CB, OnlySingleton) ||
                       isMemSSAArgRefMod(CB, OnlySingleton)) {
              DSE_LOG(errs() << "\tRecurse inter-procedurally in the callee\n");
              // Inter-procedural step: we recurse on the uses of
              // the corresponding formal (non-primed) variable in
              // the callee.

              int64_t idx = getMemSSAParamIdx(CB);
              if (idx < 0) {
                report_fatal_error(
                    "[IPDSE] cannot find index in shadow.mem function");
              }
              // HACK: find the actual callsite associated with
              // shadow.mem.arg.ref_mod(...)
              const Function *calleeF = findCalledFunction(CB);
              if (!calleeF) {
                // Fix Bug 3: gracefully handle missing callee instead of
                // crashing — conservatively keep the store.
                errs() << "Warning: [IPDSE] cannot find callee for "
                          "shadow.mem.XXX; keeping store conservatively.\n";
                markToKeep(w.storeInstOrGvInit);
                continue;
              }
              const MemorySSAFunction *MemSsaFun = MMan.getFunction(calleeF);
              if (!MemSsaFun) {
                errs() << "Warning: [IPDSE] cannot find MemorySSAFunction for "
                       << calleeF->getName()
                       << "; keeping store conservatively.\n";
                markToKeep(w.storeInstOrGvInit);
                continue;
              }

              if (MemSsaFun->getNumInFormals() == 0) {
                // Probably the function has only shadow.mem.arg.init
                errs() << "TODO: unexpected case function without "
                          "shadow.mem.in.\n";
                markToKeep(w.storeInstOrGvInit);
                continue;
              }

              const Value *calleeInitArgV = MemSsaFun->getInFormal(idx);
              if (!calleeInitArgV) {
                report_fatal_error("[IPDSE] getInFormal returned nullptr");
              }

              if (const Instruction *calleeInitArg =
                      dyn_cast<const Instruction>(calleeInitArgV)) {
                enqueue(queue, QueueElem(calleeInitArg, w.storeInstOrGvInit,
                                         w.length + 1));
              } else {
                report_fatal_error("[IPDSE] expected to enqueue from callee");
              }

            } else if (isMemSSAFunIn(CB, OnlySingleton)) {
              // Fix Bug 8: shadow.mem.in represents the entry of a value into
              // a function. Enqueue its users so that stores inside callees
              // reachable via shadow.mem.in are also explored.
              DSE_LOG(errs() << "\tin: enqueue users\n");
              for (auto &InU : CB->uses()) {
                if (Instruction *InUI = dyn_cast<Instruction>(InU.getUser())) {
                  enqueue(queue,
                          QueueElem(InUI, w.storeInstOrGvInit, w.length + 1));
                }
              }
            } else if (isMemSSAFunOut(CB, OnlySingleton)) {
              DSE_LOG(errs() << "\tRecurse inter-procedurally in the caller\n");
              // Inter-procedural step: we recurse on the uses of
              // the corresponding actual (primed) variable in the
              // caller.

              int64_t idx = getMemSSAParamIdx(CB);
              if (idx < 0) {
                report_fatal_error(
                    "[IPDSE] cannot find index in shadow.mem function");
              }

              // Find callers
              Function *F = I->getParent()->getParent();
              for (auto &U : F->uses()) {
                if (CallInst *CI = dyn_cast<CallInst>(U.getUser())) {
                  const MemorySSACallSite *MemSsaCS = MMan.getCallSite(CI);
                  if (!MemSsaCS) {
                    report_fatal_error("[IPDSE] cannot find MemorySSACallSite");
                  }

                  // make things easier ...
                  if (!CI->getCalledFunction()) {
                    markToKeep(w.storeInstOrGvInit);
                    // Fix Bug 9: use continue instead of break so remaining
                    // callers are still checked.
                    continue;
                  }
                  if (hasFunctionPtrParam(CI->getCalledFunction())) {
                    markToKeep(w.storeInstOrGvInit);
                    // Fix Bug 9: continue, not break.
                    continue;
                  }

                  if (idx >= MemSsaCS->numParams()) {
                    // It's possible that the function has formal
                    // parameters but the call site does not have actual
                    // parameters. E.g., llvm can remove the return
                    // parameter from the callsite if it's not used.
                    errs() << "TODO: unexpected case of callsite with no "
                              "actual parameters.\n";
                    markToKeep(w.storeInstOrGvInit);
                    // Fix Bug 9: continue, not break — check remaining callers.
                    continue;
                  }

                  if (OnlySingleton) {
                    if ((!MemSsaCS->isRefMod(idx)) && (!MemSsaCS->isMod(idx)) &&
                        (!MemSsaCS->isNew(idx))) {
                      // XXX: if OnlySingleton then isRefMod, isMod, and
                      // isNew can only return true if the corresponding
                      // memory region is a singleton. We saw cases
                      // (e.g., curl) where we start from store to a
                      // singleton region but after following its
                      // def-use chain we end up having other shadow.mem
                      // instructions that do not correspond to a
                      // singleton region. This is a sea-dsa issue. For
                      // now, we play conservative and give up by
                      // keeping the store.
                      markToKeep(w.storeInstOrGvInit);
                      // Fix Bug 9: continue, not break.
                      continue;
                    }
                  }

                  assert(OnlySingleton || MemSsaCS->isRefMod(idx) ||
                         MemSsaCS->isMod(idx) || MemSsaCS->isNew(idx));
                  if (const Instruction *caller_primed =
                          dyn_cast<const Instruction>(
                              MemSsaCS->getPrimed(idx))) {
                    enqueue(queue, QueueElem(caller_primed, w.storeInstOrGvInit,
                                             w.length + 1));
                  } else {
                    report_fatal_error(
                        "[IPDSE] expected to enqueue from caller");
                  }
                }
              }
            } else {
              errs() << "Warning: unexpected case during worklist processing "
                     << *I << "\n";
            }
          }
        }
      }

      // Finally, we remove dead instructions and useless global
      // initializers
      for (auto &kv : m_valueMap) {
        if (kv.second == StoreState::Removable) {
          if (StoreInst *SI = dyn_cast<StoreInst>(kv.first)) {
            DSE_LOG(errs() << "[IPDSE] DELETED " << *SI << "\n");
            SI->eraseFromParent();
            numUselessStores++;
          } else if (GlobalVariable *GV = dyn_cast<GlobalVariable>(kv.first)) {
            DSE_LOG(errs() << "[IPDSE] USELESS INITIALIZER " << *GV << "\n");
            numUselessGvInit++;
            LLVMContext &C = M.getContext();
            MDNode *N = MDNode::get(C, MDString::get(C, "useless_initializer"));
            GV->setMetadata("ipdse.useless_initializer", N);
          }
        }
      }

      errs() << "\tNumber of deleted stores " << numUselessStores << "\n";
      errs() << "\tNumber of useless global initializers " << numUselessGvInit
             << "\n";
      errs() << "\tSkipped " << skippedChains
             << " def-use chains because they were too long\n";
      errs() << "Finished ip-dse\n";
    }

    // Make sure that we remove all the shadow.mem functions
    DSE_LOG(errs() << "Removing shadow.mem functions ... \n";);
    seadsa::StripShadowMemPass SSMP;
    SSMP.runOnModule(M);

    return (numUselessStores > 0 || numUselessGvInit > 0);
  }

  /// @brief Specify analysis dependencies and preserves
  /// @param AU Analysis usage information to populate
  virtual void getAnalysisUsage(AnalysisUsage &AU) const override {
    // Fix Bug 4: do NOT call AU.setPreservesAll() — this pass erases
    // instructions (StoreInst) and runs StripShadowMemPass, which invalidates
    // most analyses. Declare only what is actually required.

    // Required to place shadow.mem.in and shadow.mem.out
    AU.addRequired<llvm::UnifyFunctionExitNodesLegacyPass>();
    // This pass will instrument the code with shadow.mem calls
    AU.addRequired<seadsa::ShadowMemPass>();
  }

  /// @brief Get the name of this pass
  /// @return The pass name as a string reference
  virtual StringRef getPassName() const override {
    return "Interprocedural Dead Store Elimination";
  }
};

char IPDeadStoreElimination::ID = 0;
} // namespace transforms
} // namespace previrt

static llvm::RegisterPass<previrt::transforms::IPDeadStoreElimination>
    X("ipdse", "Inter-procedural Dead Store Elimination");
