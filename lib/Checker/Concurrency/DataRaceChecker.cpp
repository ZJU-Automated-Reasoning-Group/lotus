/*
 *
 * Author: rainoftime
 */
#include "Checker/Concurrency/DataRaceChecker.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/MHP/HappensBeforeAnalysis.h"
#include "Concurrency/Utils/CppAtomics.h"

#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;

namespace concurrency {

namespace {

bool isPrivateLike(OpenMP::DataSharingAttribute attribute) {
  switch (attribute) {
  case OpenMP::DataSharingAttribute::Private:
  case OpenMP::DataSharingAttribute::Firstprivate:
  case OpenMP::DataSharingAttribute::Lastprivate:
  case OpenMP::DataSharingAttribute::Linear:
    return true;
  default:
    return false;
  }
}

const Value *stripValue(const Value *value) {
  return value ? value->stripPointerCasts() : nullptr;
}

const Value *resolveRegionKey(const Value *value, const DataLayout &DL,
                              int64_t &offset, bool &has_precise_offset) {
  offset = 0;
  has_precise_offset = false;
  if (!value) {
    return nullptr;
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  worklist.push_back(value);
  const Value *resolved = nullptr;
  int64_t resolved_offset = 0;
  bool resolved_precise = false;

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    current = stripValue(current);
    if (const auto *load = dyn_cast<LoadInst>(current)) {
      worklist.push_back(load->getPointerOperand());
      continue;
    }
    if (const auto *phi = dyn_cast<PHINode>(current)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }
    if (const auto *select = dyn_cast<SelectInst>(current)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }

    int64_t current_offset = 0;
    bool current_precise = false;
    const Value *base = nullptr;
    if (current->getType()->isPointerTy()) {
      if (const Value *base_with_offset =
              GetPointerBaseWithConstantOffset(current, current_offset, DL)) {
        base = stripValue(base_with_offset);
        if (!(isa<GEPOperator>(current) && base == stripValue(current))) {
          current_precise = true;
        }
      }
    }
    if (!base && current->getType()->isPointerTy()) {
      base = stripValue(getUnderlyingObject(current));
    }
    if (!base) {
      base = current;
    }

    if (!resolved) {
      resolved = base;
      resolved_offset = current_offset;
      resolved_precise = current_precise;
    } else if (resolved != base) {
      has_precise_offset = false;
      offset = 0;
      return nullptr;
    } else if (resolved_precise && current_precise &&
               resolved_offset != current_offset) {
      resolved_precise = false;
    }
  }

  offset = resolved_precise ? resolved_offset : 0;
  has_precise_offset = resolved_precise;
  return resolved;
}

} // namespace

DataRaceChecker::DataRaceChecker(
    Module &module, IMHPAnalysis *mhpAnalysis, LockSetAnalysis *locksetAnalysis,
    EscapeAnalysis *escapeAnalysis,
    ThreadLocal::ThreadLocalAnalysis *threadLocalAnalysis,
    lotus::StaticThreadSharingAnalysis *staticThreadSharingAnalysis,
    AliasAnalysisWrapper *aliasAnalysis,
    HappensBeforeAnalysis *happensBeforeAnalysis)
    : m_module(module), m_mhpAnalysis(mhpAnalysis),
      m_locksetAnalysis(locksetAnalysis), m_escapeAnalysis(escapeAnalysis),
      m_threadLocalAnalysis(threadLocalAnalysis),
      m_staticThreadSharingAnalysis(staticThreadSharingAnalysis),
      m_aliasAnalysis(aliasAnalysis),
      m_happensBeforeAnalysis(happensBeforeAnalysis),
      m_threadAPI(ThreadAPI::getThreadAPI()) {}

bool DataRaceChecker::areIndependent(const Instruction *inst1,
                                     const Instruction *inst2) const {
  return !mayAccessSameLocation(inst1, inst2);
}

// Types that are never considered racy (sync objects, FILE, etc.). Borrowed
// from Goblint.
static bool isIgnorableTypeForRace(const Type *ty) {
  if (!ty || !ty->isPointerTy())
    return false;
  const Type *elem = cast<PointerType>(ty)->getPointerElementType();
  const StructType *st = dyn_cast<StructType>(elem);
  if (!st || !st->hasName())
    return false;
  StringRef name = st->getName();
  // Strip LLVM name prefix if present
  if (name.startswith("\01"))
    name = name.drop_front(1);
  static const char *ignorable[] = {
      // pthread primitives
      "pthread_mutex_t",
      "pthread_cond_t",
      "pthread_barrier_t",
      "pthread_rwlock_t",
      "pthread_spinlock_t",
      "pthread_once_t",
      "__pthread_mutex_s",
      "__pthread_cond_s",
      "__pthread_rwlock_arch_t",
      "pthread_condattr_t",
      "pthread_mutexattr_t",
      "pthread_barrierattr_t",
      "_pthread_cleanup_buffer",
      "__cancel_jmp_buf_tag",
      // C++ standard library synchronization primitives
      "mutex",
      "recursive_mutex",
      "shared_mutex",
      "shared_timed_mutex",
      "timed_mutex",
      "lock_guard",
      "unique_lock",
      "scoped_lock",
      "shared_lock",
      "condition_variable",
      "condition_variable_any",
      "once_flag",
      "promise",
      "future",
      "shared_future",
      "latch",
      "barrier",
      "counting_semaphore",
      "binary_semaphore",
      "jthread",
      // Atomic and low-level primitives
      "atomic_flag",
      "atomic_t",
      "atomic",
      "spinlock_t",
      "lock_class_key",
      // File I/O
      "FILE",
      "__FILE",
      "_IO_FILE",
      "__jmp_buf_tag",
  };
  for (const char *ig : ignorable)
    if (name.equals(ig))
      return true;
  if (name.startswith("__anon"))
    return true; // anonymous sync structs
  return false;
}

// Single predicate for "would we report a data race for this pair?" (borrowed
// from Goblint MCP idea).
bool DataRaceChecker::wouldReportDataRace(const Instruction *inst1,
                                          const Instruction *inst2) const {
  auto isDefinitelyThreadLocalAccess = [this](const Instruction *inst) {
    if (!inst) {
      return false;
    }
    if (m_threadLocalAnalysis &&
        m_threadLocalAnalysis->accessesThreadLocalStorage(inst)) {
      return true;
    }
    if (m_staticThreadSharingAnalysis &&
        m_staticThreadSharingAnalysis->classify(inst) ==
            lotus::StaticThreadSharingAnalysis::SharingClassification::
                DefinitelyThreadLocal) {
      return true;
    }
    return false;
  };

  if (isDefinitelyThreadLocalAccess(inst1) ||
      isDefinitelyThreadLocalAccess(inst2)) {
    return false;
  }
  if (isAtomicOperation(inst1) && isAtomicOperation(inst2))
    return false;
  if (!isWriteAccess(inst1) && !isWriteAccess(inst2))
    return false;
  if (!m_mhpAnalysis->mayHappenInParallel(inst1, inst2))
    return false;
  if (m_happensBeforeAnalysis &&
      (m_happensBeforeAnalysis->happensBefore(inst1, inst2) ||
       m_happensBeforeAnalysis->happensBefore(inst2, inst1)))
    return false;
  if (areIndependent(inst1, inst2))
    return false;

  if (m_locksetAnalysis) {
    const auto access_kind = [this](const Instruction *inst) {
      return isWriteAccess(inst) ? mhp::MemoryAccessKind::Write
                                 : mhp::MemoryAccessKind::Read;
    };
    if (m_locksetAnalysis->mustMutuallyExclude(inst1, access_kind(inst1), inst2,
                                               access_kind(inst2))) {
      return false;
    }
  }
  return true;
}

// Detects data races by checking all pairs of memory accesses.
// A data race occurs when:
//   1. Two instructions may happen in parallel (MHP analysis)
//   2. At least one is a write operation
//   3. They may access the same memory location (alias analysis)
//   4. The pair is not atomic-vs-atomic. Atomic-vs-non-atomic conflicts are
//      still races and must be checked here.
//   5. They are not protected by a common lock (LockSet analysis)
//   6. The memory location is shared/escaped (Escape analysis)
// Reports one bug per "racy component" (connected set of conflicting accesses),
// not per pair (borrowed from Goblint).
std::vector<ConcurrencyBugReport> DataRaceChecker::checkDataRaces() {
  buildSyncObjectSet();
  std::vector<const Instruction *> accesses;
  collectVariableAccesses(accesses);

  using Pair = std::pair<const Instruction *, const Instruction *>;
  std::vector<Pair> racyPairs;

  // Group accesses by underlying base object to avoid quadratic scans across
  // unrelated objects. For pointer-indirect accesses (e.g. *alias_ptr), also
  // add to groups for pointed-to objects so alias_ptr->shared_var races are
  // detected.
  std::unordered_map<const Value *, std::vector<const Instruction *>> byBase;
  byBase.reserve(accesses.size());
  for (const Instruction *I : accesses) {
    const Value *loc = getMemoryLocation(I);
    if (!loc)
      continue;
    const Value *base = llvm::getUnderlyingObject(loc);
    byBase[base].push_back(I);
    // Add to groups for pointed-to objects (fixes data_race_alias: *alias_ptr
    // and shared_var must be compared)
    if (m_aliasAnalysis && loc->getType()->isPointerTy()) {
      std::vector<const Value *> pts;
      if (m_aliasAnalysis->getPointsToSet(loc, pts)) {
        for (const Value *target : pts) {
          const Value *targetBase = llvm::getUnderlyingObject(target);
          if (targetBase && targetBase != base)
            byBase[targetBase].push_back(I);
        }
      }
    }
  }

  // Collect racy pairs using single predicate (within each base object group).
  for (auto &kv : byBase) {
    auto &vec = kv.second;
    for (size_t i = 0; i < vec.size(); ++i) {
      const Instruction *inst1 = vec[i];
      for (size_t j = i + 1; j < vec.size(); ++j) {
        const Instruction *inst2 = vec[j];
        if (wouldReportDataRace(inst1, inst2))
          racyPairs.emplace_back(inst1, inst2);
      }
    }
  }

  // Build union-find over instructions that appear in any racy pair (one report
  // per component)
  std::unordered_map<const Instruction *, const Instruction *> parent;
  auto findRoot = [&parent](const Instruction *i) -> const Instruction * {
    const Instruction *cur = i;
    std::vector<const Instruction *> path;
    for (;;) {
      auto it = parent.find(cur);
      if (it == parent.end())
        return cur;
      if (it->second == cur) { // root
        for (const Instruction *p : path)
          parent[p] = cur;
        return cur;
      }
      path.push_back(cur);
      cur = it->second;
    }
  };
  auto unite = [&findRoot, &parent](const Instruction *a,
                                    const Instruction *b) {
    const Instruction *ra = findRoot(a);
    const Instruction *rb = findRoot(b);
    if (ra != rb)
      parent[ra] = rb;
  };
  for (const Pair &p : racyPairs) {
    if (parent.find(p.first) == parent.end())
      parent[p.first] = p.first;
    if (parent.find(p.second) == parent.end())
      parent[p.second] = p.second;
    unite(p.first, p.second);
  }

  // Group by root: root -> list of instructions in component
  std::unordered_map<const Instruction *, std::vector<const Instruction *>>
      components;
  for (auto &kv : parent) {
    const Instruction *root = findRoot(kv.first);
    components[root].push_back(kv.first);
  }

  // One report per component (representative pair = first two in component)
  std::vector<ConcurrencyBugReport> reports;
  for (auto &kv : components) {
    std::vector<const Instruction *> &comp = kv.second;
    if (comp.empty())
      continue;
    const Instruction *rep1 = comp[0];
    const Instruction *rep2 = comp.size() > 1 ? comp[1] : comp[0];
    std::string desc = "Potential data race between " +
                       getInstructionLocation(rep1) + " and " +
                       getInstructionLocation(rep2);
    if (comp.size() > 2)
      desc += " (" + std::to_string(comp.size()) + " conflicting accesses)";

    ConcurrencyBugReport report(ConcurrencyBugType::DATA_RACE, desc,
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    report.setDataRaceInfo(getAccessPath(rep1), getAccessPath(rep2),
                           isWriteAccess(rep1), isWriteAccess(rep2),
                           getAccessPath(rep1) + " / " + getAccessPath(rep2));
    for (const Instruction *inst : comp)
      report.addStep(inst, isWriteAccess(inst) ? "Write" : "Read");
    reports.push_back(std::move(report));
  }
  return reports;
}

// Collect all candidate memory accesses into a flat list so that alias
// checks catch distinct pointer expressions referencing the same memory.
void DataRaceChecker::collectVariableAccesses(
    std::vector<const Instruction *> &accesses) {
  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      if (isMemoryAccess(&*I)) {
        const Value *memLoc = getMemoryLocation(&*I);
        if (memLoc) {
          if (m_threadLocalAnalysis &&
              m_threadLocalAnalysis->accessesThreadLocalStorage(&*I)) {
            continue;
          }
          if (m_staticThreadSharingAnalysis &&
              m_staticThreadSharingAnalysis->classify(&*I) ==
                  lotus::StaticThreadSharingAnalysis::SharingClassification::
                      DefinitelyThreadLocal) {
            continue;
          }
          if (isOpenMPPrivateLikeAccess(&*I, memLoc))
            continue;
          if (isSyncObjectAccess(memLoc))
            continue;
          if (isIgnorableTypeForRace(memLoc->getType()))
            continue;
          if (m_escapeAnalysis && !m_escapeAnalysis->isEscaped(memLoc)) {
            // If it's a local variable that hasn't escaped, it can't race
            // Check if it's a stack allocation (AllocaInst)
            const Value *baseObj = memLoc->stripPointerCasts();
            if (isa<AllocaInst>(baseObj)) {
              continue;
            }
            // For other types (globals, etc), let escape analysis decide.
            // Do NOT discard the access here so that shared globals are
            // considered.
          }
          accesses.push_back(&*I);
        }
      }
    }
  }
}

bool DataRaceChecker::isOpenMPPrivateLikeAccess(const Instruction *inst,
                                                const Value *loc) const {
  if (!inst || !loc || !m_mhpAnalysis) {
    return false;
  }
  auto *regionMHP = dynamic_cast<const MHPAnalysis *>(m_mhpAnalysis);
  if (!regionMHP) {
    return false;
  }
  const OpenMP::OpenMPSemantics *semantics = regionMHP->getOpenMPSemantics();
  if (!semantics) {
    return false;
  }

  const Function *func = inst->getFunction();
  if (!func) {
    return false;
  }

  int64_t offset = 0;
  bool precise = false;
  const Value *base =
      resolveRegionKey(loc, m_module.getDataLayout(), offset, precise);
  if (!base) {
    base = stripValue(loc);
  }

  bool saw_matching_task = false;
  for (const auto &task_uptr : semantics->getTasks()) {
    const OpenMP::Task *task = task_uptr.get();
    if (!task || task->task_function != func) {
      continue;
    }
    saw_matching_task = true;
    for (const OpenMP::DataSharingEntry &entry : task->data_sharing_entries) {
      if (!isPrivateLike(entry.attribute) || !entry.canonical_base) {
        continue;
      }
      if (entry.canonical_base != base) {
        continue;
      }
      if (entry.has_precise_offset && precise && entry.offset != offset) {
        continue;
      }
      return true;
    }
  }

  if (!saw_matching_task) {
    return false;
  }

  return false;
}

// Checks if two instructions may access the same memory location using alias
// and points-to analysis (so that *alias_ptr and shared_var are recognized when
// alias_ptr points to shared_var).
bool DataRaceChecker::mayAccessSameLocation(const Instruction *inst1,
                                            const Instruction *inst2) const {
  const Instruction *a = inst1 < inst2 ? inst1 : inst2;
  const Instruction *b = inst1 < inst2 ? inst2 : inst1;
  if (a && b) {
    auto cache_it = m_location_overlap_cache.find({a, b});
    if (cache_it != m_location_overlap_cache.end()) {
      return cache_it->second;
    }
  }

  const Value *ptr1 = getMemoryLocation(inst1);
  const Value *ptr2 = getMemoryLocation(inst2);
  if (mayAlias(ptr1, ptr2)) {
    return (a && b) ? (m_location_overlap_cache[{a, b}] = true) : true;
  }
  // Points-to: if one pointer may point to the other's object, they may access
  // same location.
  std::vector<const Value *> pts1, pts2;
  if (m_aliasAnalysis && m_aliasAnalysis->getPointsToSet(ptr1, pts1)) {
    for (const Value *target : pts1) {
      if (target == ptr2 || (m_aliasAnalysis->mayAlias(target, ptr2))) {
        return (a && b) ? (m_location_overlap_cache[{a, b}] = true) : true;
      }
    }
  }
  if (m_aliasAnalysis && m_aliasAnalysis->getPointsToSet(ptr2, pts2)) {
    for (const Value *target : pts2) {
      if (target == ptr1 || (m_aliasAnalysis->mayAlias(target, ptr1))) {
        return (a && b) ? (m_location_overlap_cache[{a, b}] = true) : true;
      }
    }
  }
  // Conservative fallback: two globals where one is pointer-typed (e.g.
  // alias_ptr = &shared_var). Alias analysis may not connect *alias_ptr and
  // shared_var; treat as may-access-same.
  if (ptr1 && ptr2 && ptr1 != ptr2) {
    const auto *g1 = dyn_cast<GlobalValue>(ptr1);
    const auto *g2 = dyn_cast<GlobalValue>(ptr2);
    if (g1 && g2 &&
        (ptr1->getType()->isPointerTy() || ptr2->getType()->isPointerTy()))
      return (a && b) ? (m_location_overlap_cache[{a, b}] = true) : true;
  }
  return (a && b) ? (m_location_overlap_cache[{a, b}] = false) : false;
}

// Returns true if two values may alias (point to overlapping memory).
// Uses alias analysis wrapper when available, otherwise conservatively assumes
// aliasing.
bool DataRaceChecker::mayAlias(const Value *v1, const Value *v2) const {
  if (!v1 || !v2)
    return false;
  if (v1 == v2)
    return true;
  if (m_aliasAnalysis) {
    return m_aliasAnalysis->mayAlias(v1, v2);
  }
  return true; // Conservative: assume may alias if we can't prove otherwise.
}

bool DataRaceChecker::isMemoryAccess(const Instruction *inst) const {
  return isa<LoadInst>(inst) || isa<StoreInst>(inst) ||
         isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

bool DataRaceChecker::isWriteAccess(const Instruction *inst) const {
  return isa<StoreInst>(inst) || isa<AtomicRMWInst>(inst) ||
         isa<AtomicCmpXchgInst>(inst);
}

bool DataRaceChecker::isAtomicOperation(const Instruction *inst) const {
  // Use enhanced CppAtomics module for better atomic recognition
  if (CppAtomics::isAtomic(inst))
    return true;

  // Legacy check for backward compatibility
  if (isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst))
    return true;
  if (const auto *L = dyn_cast<LoadInst>(inst))
    return L->isAtomic();
  if (const auto *S = dyn_cast<StoreInst>(inst))
    return S->isAtomic();

  // Check if it's a fence instruction
  if (CppAtomics::isFence(inst))
    return true;

  return false;
}

// Extracts the memory location (pointer operand) from a memory access
// instruction.
const Value *DataRaceChecker::getMemoryLocation(const Instruction *inst) const {
  if (const auto *load = dyn_cast<LoadInst>(inst))
    return load->getPointerOperand();
  if (const auto *store = dyn_cast<StoreInst>(inst))
    return store->getPointerOperand();
  if (const auto *rmw = dyn_cast<AtomicRMWInst>(inst))
    return rmw->getPointerOperand();
  if (const auto *cmpxchg = dyn_cast<AtomicCmpXchgInst>(inst))
    return cmpxchg->getPointerOperand();
  return nullptr;
}

std::string
DataRaceChecker::getInstructionLocation(const Instruction *inst) const {
  std::string location;
  raw_string_ostream os(location);
  if (const Function *func = inst->getFunction())
    os << func->getName();
  if (const BasicBlock *bb = inst->getParent())
    os << ":" << bb->getName();
  return os.str();
}

void DataRaceChecker::buildSyncObjectSet() {
  m_syncObjects.clear();
  for (Function &F : m_module) {
    if (F.isDeclaration())
      continue;
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      const Instruction *inst = &*I;
      const CallBase *cb = dyn_cast<CallBase>(inst);
      if (!cb || !m_threadAPI->getCallee(inst))
        continue;
      const Value *v = nullptr;

      // Traditional pthread primitives
      if (m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst))
        v = m_threadAPI->getAnalysisLockIdentity(inst);
      else if (m_threadAPI->isTDCondWait(inst) ||
               m_threadAPI->isTDCondSignal(inst) ||
               m_threadAPI->isTDCondBroadcast(inst))
        v = m_threadAPI->getCondVal(inst);
      else if (m_threadAPI->isTDBarWait(inst))
        v = m_threadAPI->getBarrierVal(inst);

      // Modern C++ synchronization primitives
      else {
        ThreadAPI::TD_TYPE type = m_threadAPI->getType(cb);

        switch (type) {
        case ThreadAPI::TD_SHARED_RDLOCK:
        case ThreadAPI::TD_SHARED_WRLOCK:
        case ThreadAPI::TD_SHARED_UNLOCK:
        case ThreadAPI::TD_LOCK_GUARD_CTOR:
        case ThreadAPI::TD_LOCK_GUARD_DTOR:
        case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
        case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
        case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
        case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
        case ThreadAPI::TD_SCOPED_LOCK_CTOR:
        case ThreadAPI::TD_SCOPED_LOCK_DTOR:
        case ThreadAPI::TD_SHARED_LOCK_CTOR:
        case ThreadAPI::TD_SHARED_LOCK_DTOR:
        case ThreadAPI::TD_SEMAPHORE_ACQUIRE:
        case ThreadAPI::TD_SEMAPHORE_RELEASE:
          // These take mutex/semaphore as argument
          if (cb->arg_size() >= 1)
            v = cb->getArgOperand(0);
          break;

        case ThreadAPI::TD_CALL_ONCE:
          // Takes once_flag as first argument
          if (cb->arg_size() >= 1)
            v = cb->getArgOperand(0);
          break;

        case ThreadAPI::TD_FUTURE_GET:
        case ThreadAPI::TD_FUTURE_WAIT:
        case ThreadAPI::TD_PROMISE_SET:
          // Future/promise are synchronization objects themselves
          if (cb->arg_size() >= 1)
            v = cb->getArgOperand(0);
          break;

        case ThreadAPI::TD_LATCH_COUNT_DOWN:
        case ThreadAPI::TD_LATCH_WAIT:
        case ThreadAPI::TD_LATCH_ARRIVE_WAIT:
          // Latch object
          if (cb->arg_size() >= 1)
            v = cb->getArgOperand(0);
          break;

        case ThreadAPI::TD_BARRIER_ARRIVE_WAIT:
        case ThreadAPI::TD_BARRIER_ARRIVE:
        case ThreadAPI::TD_BARRIER_WAIT_CPP20:
        case ThreadAPI::TD_CUDA_BARRIER:
        case ThreadAPI::TD_CUDA_WARP_BARRIER:
          // Barrier object
          if (cb->arg_size() >= 1)
            v = cb->getArgOperand(0);
          break;

        default:
          break;
        }
      }

      if (v)
        m_syncObjects.insert(v->stripPointerCasts());
    }
  }
}

bool DataRaceChecker::isSyncObjectAccess(const Value *loc) const {
  if (!loc)
    return false;
  Value *stripped = const_cast<Value *>(loc)->stripPointerCasts();
  if (m_syncObjects.count(stripped))
    return true;
  if (!m_aliasAnalysis)
    return false;
  for (const Value *sync : m_syncObjects)
    if (m_aliasAnalysis->mustAlias(stripped, sync))
      return true;
  return false;
}

std::string DataRaceChecker::getAccessPath(const Instruction *inst) const {
  const Value *loc = getMemoryLocation(inst);
  if (!loc)
    return getInstructionLocation(inst);
  std::string s;
  raw_string_ostream os(s);
  loc->printAsOperand(os, true);
  return os.str();
}

} // namespace concurrency
