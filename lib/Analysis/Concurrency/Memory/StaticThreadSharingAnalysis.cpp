/*
Scalable thread sharing analysis, ICSE 16. Jeff Huang

This analysis identifies memory locations that are accessed by multiple threads.
It serves as a prerequisite for race detection.

Dependencies:
- SeaDSA (DsaAnalysis): shared-ness is reported per allocation/field from
SeaDSA's graph; aggressive collapsing can over-approximate shared, weak
precision can under-approximate.
- m_threads_complete: when false, some thread entry points were unknown (e.g.
  indirect fork); sharing results may under-approximate (fewer locations marked
  shared) and should be interpreted conservatively.

Soundness/Approximations:
- Shared Data: Considers data shared if accessed by >1 thread, or written by a
multi-run thread.
- Uncollapsed Globals: Uses the accessing pointer as a proxy for the memory
location, which is a reasonable approximation when SeaDSA collapses globals.
- Immutable Data: Distinguishes between reads and writes to ignore immutable
shared data.
*/
#include "Analysis/Concurrency/Memory/StaticThreadSharingAnalysis.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "Alias/seadsa/DsaAnalysis.hh"
#include "Alias/seadsa/Global.hh"
#include "Alias/seadsa/Graph.hh"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include "Analysis/Concurrency/Utils/ThreadMultiplicity.h"

using namespace llvm;
using namespace seadsa;

namespace {

constexpr int kSummaryField = -1;

struct MemoryAccess {
  const Value *Ptr = nullptr;
  bool IsWrite = false;
};

const Value *getGlobalAccessKey(const Value *V) {
  if (!V) {
    return nullptr;
  }
  V = V->stripPointerCasts();
  if (const auto *CE = dyn_cast<ConstantExpr>(V)) {
    if (CE->isCast()) {
      V = CE->getOperand(0)->stripPointerCasts();
    }
  }
  return dyn_cast<GlobalValue>(V);
}

const Value *getAccessKey(const Value *V) {
  if (!V) {
    return nullptr;
  }
  V = V->stripPointerCasts();
  if (const auto *CE = dyn_cast<ConstantExpr>(V)) {
    if (CE->isCast()) {
      V = CE->getOperand(0)->stripPointerCasts();
    }
  }
  return V;
}

bool isStableAccessKey(const Value *V) {
  V = getAccessKey(V);
  return V && (isa<Argument>(V) || isa<AllocaInst>(V) || isa<GlobalValue>(V) ||
               isa<CallBase>(V));
}

std::vector<const Value *> collectAccessKeys(const Value *Ptr, Node *N,
                                             Graph &G) {
  std::set<const Value *> keys;
  const auto &allocSites = N->getAllocSites();
  for (const Value *allocSite : allocSites) {
    keys.insert(allocSite);
  }

  if (keys.empty() && N->getNodeType().global) {
    for (const auto &KV : G.globals()) {
      if (!KV.second) {
        continue;
      }
      if (KV.second->getNode() != N) {
        continue;
      }
      if (const Value *globalKey = getGlobalAccessKey(KV.first)) {
        keys.insert(globalKey);
      }
    }

    if (keys.empty()) {
      if (const Value *globalKey = getGlobalAccessKey(Ptr)) {
        keys.insert(globalKey);
      }
    }
  }

  if (keys.empty()) {
    if (isStableAccessKey(Ptr)) {
      const Value *ptrKey = getAccessKey(Ptr);
      keys.insert(ptrKey);
    }
    if (const Value *obj = getUnderlyingObject(Ptr)) {
      if (isStableAccessKey(obj)) {
        const Value *objKey = getAccessKey(obj);
        keys.insert(objKey);
      }
    }
  }

  return std::vector<const Value *>(keys.begin(), keys.end());
}

bool isPerInstanceThreadLocalAllocSite(const Value *allocSite) {
  if (!allocSite) {
    return false;
  }
  return isa<AllocaInst>(allocSite->stripPointerCasts());
}

SmallVector<MemoryAccess, 2> collectMemoryAccesses(const Instruction &I) {
  SmallVector<MemoryAccess, 2> accesses;

  if (const auto *LI = dyn_cast<LoadInst>(&I)) {
    accesses.push_back({LI->getPointerOperand(), false});
    return accesses;
  }
  if (const auto *SI = dyn_cast<StoreInst>(&I)) {
    accesses.push_back({SI->getPointerOperand(), true});
    return accesses;
  }
  if (const auto *RMW = dyn_cast<AtomicRMWInst>(&I)) {
    accesses.push_back({RMW->getPointerOperand(), false});
    accesses.push_back({RMW->getPointerOperand(), true});
    return accesses;
  }
  if (const auto *CmpXchg = dyn_cast<AtomicCmpXchgInst>(&I)) {
    accesses.push_back({CmpXchg->getPointerOperand(), false});
    accesses.push_back({CmpXchg->getPointerOperand(), true});
    return accesses;
  }
  if (const auto *MemSet = dyn_cast<MemSetInst>(&I)) {
    accesses.push_back({MemSet->getDest(), true});
    return accesses;
  }
  if (const auto *Transfer = dyn_cast<MemTransferInst>(&I)) {
    accesses.push_back({Transfer->getSource(), false});
    accesses.push_back({Transfer->getDest(), true});
    return accesses;
  }

  return accesses;
}

bool isArrayLikeAccess(const Value *Ptr, const Node &N) {
  if (N.isArray()) {
    return true;
  }

  const Value *current = Ptr;
  while (current) {
    current = current->stripPointerCasts();
    if (const auto *GEP = dyn_cast<GEPOperator>(current)) {
      if (GEP->getSourceElementType()->isArrayTy()) {
        return true;
      }
      current = GEP->getPointerOperand();
      continue;
    }
    break;
  }

  if (const Value *obj = getUnderlyingObject(Ptr)) {
    if (Type *type = obj->getType()) {
      if (type->isPointerTy() && type->getPointerElementType()->isArrayTy()) {
        return true;
      }
    }
  }

  return false;
}

int getAccessFieldKey(const Value *Ptr, const Cell &C, const Node &N) {
  if (N.isOffsetCollapsed() || isArrayLikeAccess(Ptr, N)) {
    return kSummaryField;
  }
  return static_cast<int>(C.getOffset());
}

} // namespace

namespace lotus {

char StaticThreadSharingAnalysis::ID = 0;

StaticThreadSharingAnalysis::StaticThreadSharingAnalysis()
    : ModulePass(ID), m_dsa(nullptr) {}

void StaticThreadSharingAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DsaAnalysis>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.setPreservesAll();
}

bool StaticThreadSharingAnalysis::runOnModule(Module &M) {
  m_allocAccesses.clear();
  m_threads.clear();
  m_thread_spawn_counts.clear();
  m_threads_complete = true;
  m_access_paths_complete = true;

  // Get SeaDSA analysis
  DsaAnalysis &dsaPass = getAnalysis<DsaAnalysis>();
  m_dsa = &dsaPass.getDsaAnalysis();

  findStaticThreads(M);

  errs() << "StaticThreadSharingAnalysis: Found " << m_threads.size()
         << " static threads.\n";

  for (const Function *threadEntry : m_threads) {
    visitThread(threadEntry);
  }

  return false;
}

void StaticThreadSharingAnalysis::findStaticThreads(Module &M) {
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  concurrency::ThreadMultiplicityAnalysis multiplicity(
      M, &getAnalysis<CallGraphWrapperPass>().getCallGraph());

  auto recordThreadEntry = [this](const Function *entryFunc, bool may_repeat) {
    if (!entryFunc) {
      return;
    }
    ++m_thread_spawn_counts[entryFunc];
    if (may_repeat) {
      m_thread_spawn_counts[entryFunc] =
          std::max<unsigned>(m_thread_spawn_counts[entryFunc], 2);
    }
    if (std::find(m_threads.begin(), m_threads.end(), entryFunc) ==
        m_threads.end()) {
      m_threads.push_back(entryFunc);
    }
  };

  // Add main as a thread
  if (Function *Main = M.getFunction("main")) {
    m_threads.push_back(Main);
  }

  for (Function &F : M) {
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      if (api->isTDFork(&*I)) {
        const Value *entry = api->getForkedFun(&*I);
        const bool may_repeat =
            multiplicity.instructionMayExecuteMultipleTimes(&*I);
        if (!entry) {
          m_threads_complete = false;
          continue;
        }
        if (const Function *entryFunc = dyn_cast<Function>(entry)) {
          recordThreadEntry(entryFunc, may_repeat);
        } else {
          const Value *stripped = entry->stripPointerCasts();
          if (const Function *f = dyn_cast<Function>(stripped)) {
            recordThreadEntry(f, may_repeat);
          } else {
            m_threads_complete = false;
          }
        }
      }
    }
  }
}

void StaticThreadSharingAnalysis::visitThread(const Function *ThreadEntry) {
  std::set<const Function *> visited;
  visitMethod(ThreadEntry, ThreadEntry, visited);
}

void StaticThreadSharingAnalysis::visitMethod(
    const Function *F, const Function *ThreadEntry,
    std::set<const Function *> &Visited) {
  // Traverses the call graph reachable from ThreadEntry.
  // For each function, it iterates all instructions to find memory accesses
  // (Loads/Stores). It resolves the memory location using SeaDSA's graph.

  if (!F || F->isDeclaration() || Visited.count(F))
    return;
  Visited.insert(F);

  // Access analysis
  if (!m_dsa->hasGraph(*F)) {
    m_access_paths_complete = false;
  } else {
    Graph &G = m_dsa->getGraph(*F);
    for (const BasicBlock &BB : *F) {
      for (const Instruction &I : BB) {
        if (const auto *CB = dyn_cast<CallBase>(&I)) {
          const Value *called = CB->getCalledOperand();
          const Function *direct =
              CB->getCalledFunction()
                  ? CB->getCalledFunction()
                  : dyn_cast_or_null<Function>(
                        called ? called->stripPointerCasts() : nullptr);
          if (!direct) {
            m_access_paths_complete = false;
          }
        }
        for (const MemoryAccess &access : collectMemoryAccesses(I)) {
          recordAccess(access.Ptr, access.IsWrite, ThreadEntry, G);
        }
      }
    }
  }

  // Call graph traversal
  // Use LLVM CallGraph for basic traversal
  // Note: SeaDSA has its own CallGraph which might be more precise for indirect
  // calls But we use getAnalysis<CallGraphWrapperPass> for
  // simplicity/compatibility
  CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  CallGraphNode *CGN = CG[F];
  if (CGN) {
    for (auto &CallRecord : *CGN) {
      if (Function *Callee = CallRecord.second->getFunction()) {
        visitMethod(Callee, ThreadEntry, Visited);
      }
    }
  }
}

void StaticThreadSharingAnalysis::recordAccess(const Value *Ptr, bool isWrite,
                                               const Function *ThreadEntry,
                                               Graph &G) {
  if (!Ptr) {
    m_access_paths_complete = false;
    return;
  }

  if (!G.hasCell(*Ptr)) {
    m_access_paths_complete = false;
    return;
  }
  const Cell &C = G.getCell(*Ptr);
  Node *N = C.getNode();
  if (!N) {
    m_access_paths_complete = false;
    return;
  }

  const int fieldKey = getAccessFieldKey(Ptr, C, *N);
  std::vector<const Value *> accessKeys = collectAccessKeys(Ptr, N, G);
  if (accessKeys.empty()) {
    m_access_paths_complete = false;
    return;
  }
  for (const Value *Alloc : accessKeys) {
    AccessInfo &Info = m_allocAccesses[Alloc][fieldKey];
    if (isWrite)
      Info.Writers.insert(ThreadEntry);
    else
      Info.Readers.insert(ThreadEntry);
  }
}

bool StaticThreadSharingAnalysis::isMultiRunThread(
    const Function *ThreadEntry) const {
  if (!ThreadEntry || ThreadEntry->getName() == "main")
    return false;
  auto It = m_thread_spawn_counts.find(ThreadEntry);
  if (It == m_thread_spawn_counts.end())
    return false;
  return It->second > 1;
}

StaticThreadSharingAnalysis::SharingClassification
StaticThreadSharingAnalysis::classify(const Value *AllocSite) const {
  if (!m_threads_complete) {
    return SharingClassification::MaybeShared;
  }

  auto it = m_allocAccesses.find(AllocSite);
  if (it == m_allocAccesses.end()) {
    return m_access_paths_complete ? SharingClassification::DefinitelyThreadLocal
                                   : SharingClassification::MaybeShared;
  }

  // Check if any field is shared
  for (auto &pair : it->second) {
    const AccessInfo &info = pair.second;
    size_t writerCount = info.Writers.size();

    // Create a set of all unique threads accessing this location
    std::set<const Function *> allThreads = info.Readers;
    allThreads.insert(info.Writers.begin(), info.Writers.end());
    bool multi_run_writer =
        !isPerInstanceThreadLocalAllocSite(AllocSite) &&
        std::any_of(info.Writers.begin(), info.Writers.end(),
                    [this](const Function *F) { return isMultiRunThread(F); });

    if (allThreads.size() > 1 || multi_run_writer) {
      // According to the paper (Section 1, "Limitations of Escape Analysis"
      // point 4, and Algorithm 3), we must distinguish immutable data. Data is
      // shared only if there is at least one write. "thread-shared but
      // immutable data... our algorithm also distinguishes between reads and
      // writes"
      if (writerCount > 0) {
        return SharingClassification::DefinitelyShared;
      }
    }
  }
  return m_access_paths_complete ? SharingClassification::DefinitelyThreadLocal
                                 : SharingClassification::MaybeShared;
}

bool StaticThreadSharingAnalysis::isShared(const Value *AllocSite) const {
  return classify(AllocSite) != SharingClassification::DefinitelyThreadLocal;
}

StaticThreadSharingAnalysis::SharingClassification
StaticThreadSharingAnalysis::classify(const Instruction *Inst) const {
  if (!m_dsa || !m_threads_complete)
    return SharingClassification::MaybeShared;

  const Function *F = Inst->getFunction();
  if (!m_dsa->hasGraph(*F))
    return SharingClassification::MaybeShared;

  Graph &G = m_dsa->getGraph(*F);
  SharingClassification strongest =
      SharingClassification::DefinitelyThreadLocal;

  SmallVector<MemoryAccess, 2> accesses = collectMemoryAccesses(*Inst);
  if (accesses.empty()) {
    return SharingClassification::DefinitelyThreadLocal;
  }

  for (const MemoryAccess &access : accesses) {
    if (!access.Ptr || !G.hasCell(*access.Ptr)) {
      strongest = SharingClassification::MaybeShared;
      continue;
    }

    const Cell &C = G.getCell(*access.Ptr);
    Node *N = C.getNode();
    if (!N) {
      strongest = SharingClassification::MaybeShared;
      continue;
    }

    std::vector<const Value *> accessKeys = collectAccessKeys(access.Ptr, N, G);
    if (accessKeys.empty()) {
      strongest = SharingClassification::MaybeShared;
      continue;
    }

    for (const Value *Alloc : accessKeys) {
      SharingClassification classification = classify(Alloc);
      if (classification == SharingClassification::DefinitelyShared) {
        return classification;
      }
      if (classification == SharingClassification::MaybeShared) {
        strongest = SharingClassification::MaybeShared;
      }
    }
  }

  return strongest;
}

bool StaticThreadSharingAnalysis::isShared(const Instruction *Inst) const {
  return classify(Inst) != SharingClassification::DefinitelyThreadLocal;
}

} // namespace lotus
