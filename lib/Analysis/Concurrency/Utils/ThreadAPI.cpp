/**
 * @file ThreadAPI.cpp
 * @brief Thread API Recognition Implementation
 *
 * This file implements the ThreadAPI singleton class for recognizing and
 * categorizing thread-related API calls in multithreaded programs.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

/*
 *
 * Author: rainoftime
 */
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include "Analysis/Concurrency/MPI/MPISymbol.h"
#include "Analysis/Concurrency/Utils/CppThreading.h"
#include "Analysis/Concurrency/Utils/LinuxKernel.h"

#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdio.h>

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h> // for StringMap
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace std;
using namespace llvm;

ThreadAPI *ThreadAPI::tdAPI = nullptr;

/**
 * @struct ei_pair
 * @brief Maps thread API function names to their types
 *
 * Internal structure mapping function name strings to their corresponding
 * TD_TYPE enumeration values for classification.
 */
struct ei_pair {
  const char *n;        ///< Function name
  ThreadAPI::TD_TYPE t; ///< Thread API type
};

static StringRef stripAPIGlobalPrefix(StringRef name) {
  while (name.startswith("\01")) {
    name = name.drop_front();
  }
  return name;
}

static bool looksLikeMPISymbol(StringRef name) {
  name = stripAPIGlobalPrefix(name);
  return name.startswith("MPI_") || name.startswith("PMPI_") ||
         name.startswith("mpi_") || name.startswith("pmpi_") ||
         name.startswith("ompi_mpi_") || name.startswith("__wrap_MPI_") ||
         name.startswith("__wrap_PMPI_");
}

static std::string normalizeGenericAPIName(StringRef name) {
  return stripAPIGlobalPrefix(name).str();
}

static std::string normalizeConfiguredAPIName(StringRef name) {
  name = stripAPIGlobalPrefix(name);
  if (looksLikeMPISymbol(name)) {
    return mpi::normalizeMPISymbolName(name);
  }
  return name.str();
}

static std::string normalizeLegacyUnderscoreAlias(StringRef name) {
  name = stripAPIGlobalPrefix(name);
  if (name.startswith("_") && !name.startswith("__wrap_")) {
    return name.drop_front().str();
  }
  return name.str();
}

static bool appendLookupName(SmallVectorImpl<std::string> &names,
                             const std::string &candidate) {
  if (candidate.empty()) {
    return false;
  }
  if (llvm::is_contained(names, candidate)) {
    return false;
  }
  names.push_back(candidate);
  return true;
}

static SmallVector<std::string, 4> getConfiguredLookupNames(StringRef name) {
  SmallVector<std::string, 4> names;
  const std::string generic = normalizeGenericAPIName(name);
  appendLookupName(names, generic);

  const std::string legacy_alias = normalizeLegacyUnderscoreAlias(name);
  appendLookupName(names, legacy_alias);

  if (looksLikeMPISymbol(name)) {
    appendLookupName(names, mpi::normalizeMPISymbolName(stripAPIGlobalPrefix(name)));
  }
  return names;
}

static std::string normalizeAPIName(StringRef name) {
  return normalizeGenericAPIName(name);
}

static ThreadAPI::RuntimeLibrary parseRuntimeLibrary(StringRef value) {
  std::string lowered = value.lower();
  if (lowered == "pthread")
    return ThreadAPI::RuntimeLibrary::PThread;
  if (lowered == "openmp")
    return ThreadAPI::RuntimeLibrary::OpenMP;
  if (lowered == "mpi")
    return ThreadAPI::RuntimeLibrary::MPI;
  if (lowered == "cpp")
    return ThreadAPI::RuntimeLibrary::Cpp;
  if (lowered == "linux-kernel")
    return ThreadAPI::RuntimeLibrary::LinuxKernel;
  if (lowered == "hare")
    return ThreadAPI::RuntimeLibrary::Hare;
  if (lowered == "custom")
    return ThreadAPI::RuntimeLibrary::Custom;
  return ThreadAPI::RuntimeLibrary::Unknown;
}

/**
 * @brief Thread API mapping table
 *
 * Maps common thread library function names to their semantic types.
 * This enables static recognition of thread operations in LLVM IR.
 */
static const ei_pair ei_pairs[] = {
    // The current llvm-gcc puts in the \01.
    {"pthread_create", ThreadAPI::TD_FORK},
    {"apr_thread_create", ThreadAPI::TD_FORK},
    {"pthread_join", ThreadAPI::TD_JOIN},
    {"\01_pthread_join", ThreadAPI::TD_JOIN},
    {"pthread_cancel", ThreadAPI::TD_CANCEL},
    {"pthread_mutex_lock", ThreadAPI::TD_ACQUIRE},
    {"sem_wait", ThreadAPI::TD_ACQUIRE},
    {"_spin_lock", ThreadAPI::TD_ACQUIRE},
    {"SRE_SplSpecLockEx", ThreadAPI::TD_ACQUIRE},
    {"pthread_rwlock_rdlock", ThreadAPI::TD_RWLOCK_RDLOCK},
    {"pthread_rwlock_wrlock", ThreadAPI::TD_RWLOCK_WRLOCK},
    {"pthread_mutex_trylock", ThreadAPI::TD_TRY_ACQUIRE},
    {"pthread_mutex_unlock", ThreadAPI::TD_RELEASE},
    {"pthread_rwlock_unlock", ThreadAPI::TD_RELEASE},
    {"sem_post", ThreadAPI::TD_RELEASE},
    {"_spin_unlock", ThreadAPI::TD_RELEASE},
    {"SRE_SplSpecUnlockEx", ThreadAPI::TD_RELEASE},
    //    {"pthread_cancel", ThreadAPI::TD_CANCEL},
    {"pthread_exit", ThreadAPI::TD_EXIT},
    {"pthread_detach", ThreadAPI::TD_DETACH},
    {"pthread_cond_wait", ThreadAPI::TD_COND_WAIT},
    {"pthread_cond_signal", ThreadAPI::TD_COND_SIGNAL},
    {"pthread_cond_broadcast", ThreadAPI::TD_COND_BROADCAST},
    {"pthread_cond_init", ThreadAPI::TD_CONDVAR_INI},
    {"pthread_cond_destroy", ThreadAPI::TD_CONDVAR_DESTROY},
    {"pthread_mutex_init", ThreadAPI::TD_MUTEX_INI},
    {"pthread_mutex_destroy", ThreadAPI::TD_MUTEX_DESTROY},
    {"pthread_barrier_init", ThreadAPI::TD_BAR_INIT},
    {"pthread_barrier_wait", ThreadAPI::TD_BAR_WAIT},

    // Hare APIs
    {"hare_parallel_for", ThreadAPI::HARE_PAR_FOR},

    // This must be the last entry.
    {0, ThreadAPI::TD_DUMMY}

};

/**
 * @brief Initialize the thread API map
 *
 * Populates the tdAPIMap with function name to type mappings from ei_pairs.
 * Validates that entries are grouped by type for maintainability.
 * Then loads config/thread.spec and config/concurrency_api.spec. API
 * recognition precedence: exact tdAPIMap first, then match rules (first match
 * in order; prefer more specific rules when adding new ones), then
 * C++/OpenMP/MPI/kernel name checks.
 */
void ThreadAPI::init() {
  set<TD_TYPE> t_seen;
  TD_TYPE prev_t = TD_DUMMY;
  t_seen.insert(TD_DUMMY);
  for (const ei_pair *p = ei_pairs; p->n; ++p) {
    if (p->t != prev_t) {
      // This will detect if you move an entry to another block
      //   but forget to change the type.
      if (t_seen.count(p->t)) {
        fputs(p->n, stderr);
        putc('\n', stderr);
        assert(!"ei_pairs not grouped by type");
      }
      t_seen.insert(p->t);
      prev_t = p->t;
    }
    if (tdAPIMap.count(p->n)) {
      fputs(p->n, stderr);
      putc('\n', stderr);
      assert(!"duplicate name in ei_pairs");
    }
    tdAPIMap[p->n] = p->t;
  }
  // Load optional thread.spec so custom APIs (e.g. kernel mutex_lock) are
  // recognized
  loadConfig("config/thread.spec");
  loadConfig("../config/thread.spec");
  loadSemanticConfig("config/concurrency_api.spec");
  loadSemanticConfig("../config/concurrency_api.spec");
  loadSemanticConfig("config/openmp_api.spec");
  loadSemanticConfig("../config/openmp_api.spec");
  loadSemanticConfig("config/mpi_api.spec");
  loadSemanticConfig("../config/mpi_api.spec");
}

void ThreadAPI::addEntry(const std::string &name, TD_TYPE type) {
  tdAPIMap[normalizeConfiguredAPIName(name)] = type;
}

void ThreadAPI::addDescription(const std::string &name,
                               const APIDescription &description) {
  m_api_descriptions[normalizeConfiguredAPIName(name)] = description;
}

void ThreadAPI::addMatchRule(const std::string &pattern, MatchKind kind,
                             const APIDescription &description) {
  MatchRule rule;
  rule.pattern = normalizeConfiguredAPIName(pattern);
  rule.kind = kind;
  rule.description = description;
  m_match_rules.push_back(std::move(rule));
}

bool ThreadAPI::hasMappedAPIEntry(const Function *F) const {
  if (!F)
    return false;

  for (const std::string &name : getConfiguredLookupNames(F->getName())) {
    if (tdAPIMap.count(name) != 0) {
      return true;
    }
  }
  return false;
}

bool ThreadAPI::isCppThreadLikeFork(const Function *F) const {
  if (!F)
    return false;
  StringRef name = F->getName();
  return CppThreadingModel::isFork(name) ||
         CppThreadingModel::isJthreadConstructor(name) ||
         CppThreadingModel::isAsync(name);
}

bool ThreadAPI::isDefiniteAsyncLaunch(const Instruction *inst) const {
  const CallBase *cb = getLLVMCallSite(inst);
  const Function *callee = getCallee(inst);
  if (!cb || !callee || getType(callee) != TD_ASYNC)
    return false;

  if (cb->arg_size() == 0)
    return true;

  const Value *policy = cb->getArgOperand(0)->stripPointerCasts();
  const auto *launch_bits = dyn_cast<ConstantInt>(policy);
  if (!launch_bits)
    return false;

  // std::launch::async is specified as bit 0 in all supported libstdc++/libc++
  // implementations we target here.
  return (launch_bits->getZExtValue() & 0x1ULL) != 0;
}

bool ThreadAPI::isProvablyDeferredAsyncLaunch(const Instruction *inst) const {
  const CallBase *cb = getLLVMCallSite(inst);
  const Function *callee = getCallee(inst);
  if (!cb || !callee || getType(callee) != TD_ASYNC || cb->arg_size() == 0) {
    return false;
  }

  const Value *policy = cb->getArgOperand(0)->stripPointerCasts();
  const auto *launch_bits = dyn_cast<ConstantInt>(policy);
  if (!launch_bits) {
    return false;
  }

  return (launch_bits->getZExtValue() & 0x1ULL) == 0;
}

unsigned
ThreadAPI::getCppForkCallableSearchStart(const Instruction *inst) const {
  const CallBase *cb = getLLVMCallSite(inst);
  const Function *callee = getCallee(inst);
  if (!cb || !callee) {
    return 1;
  }
  if (getType(callee) != TD_ASYNC) {
    return 1;
  }
  if (cb->arg_size() == 0) {
    return 0;
  }

  const Value *first_arg = cb->getArgOperand(0);
  if (!first_arg) {
    return 0;
  }
  first_arg = first_arg->stripPointerCasts();
  if (isa<ConstantInt>(first_arg) || first_arg->getType()->isIntegerTy()) {
    return 1;
  }
  return 0;
}

const Value *ThreadAPI::getCallArg(const Instruction *inst,
                                   unsigned idx) const {
  const CallBase *cb = getLLVMCallSite(inst);
  if (!cb || idx >= cb->arg_size())
    return nullptr;
  return cb->getArgOperand(idx);
}

const Value *ThreadAPI::getCppThreadCallable(const Instruction *inst) const {
  const CallBase *cb = getLLVMCallSite(inst);
  if (!cb)
    return nullptr;

  const unsigned first_callable_idx = getCppForkCallableSearchStart(inst);

  // Skip the constructor `this` parameter and look for a direct function-like
  // operand. If none exists, callers fall back to unresolved-fork conservatism.
  for (unsigned idx = first_callable_idx; idx < cb->arg_size(); ++idx) {
    const Value *candidate = cb->getArgOperand(idx);
    if (!candidate)
      continue;
    candidate = candidate->stripPointerCasts();
    if (isa<Function>(candidate))
      return candidate;
  }
  return nullptr;
}

bool ThreadAPI::isConditionVariableAny(const Function *F) const {
  if (!F) {
    return false;
  }
  return normalizeAPIName(F->getName()).find("condition_variable_any") !=
         std::string::npos;
}

const Value *
ThreadAPI::getConditionVariableWaitMutex(const Instruction *inst) const {
  const CallBase *cb = getLLVMCallSite(inst);
  if (!cb || cb->arg_size() < 2) {
    return nullptr;
  }

  const Function *callee = getCallee(inst);
  const Value *lock_or_mutex = cb->getArgOperand(1);
  if (!lock_or_mutex || !isConditionVariableAny(callee)) {
    return lock_or_mutex;
  }

  const Value *lock_object = lock_or_mutex->stripPointerCasts();
  const Function *parent = inst ? inst->getFunction() : nullptr;
  if (!lock_object || !parent || parent->isDeclaration()) {
    return lock_or_mutex;
  }

  const Value *resolved_mutex = nullptr;
  auto recordCandidate = [&](const Value *candidate) -> bool {
    candidate = candidate ? candidate->stripPointerCasts() : nullptr;
    if (!candidate) {
      return false;
    }
    if (!resolved_mutex) {
      resolved_mutex = candidate;
      return true;
    }
    return resolved_mutex == candidate;
  };

  for (const Instruction &cursor : instructions(*parent)) {
    if (&cursor == inst) {
      break;
    }

    const auto *candidate_call = dyn_cast<CallBase>(&cursor);
    if (!candidate_call) {
      continue;
    }

    const Function *candidate_callee = getCallee(candidate_call);
    if (!candidate_callee) {
      continue;
    }

    TD_TYPE type = getType(candidate_callee);
    if (type != TD_LOCK_GUARD_CTOR && type != TD_UNIQUE_LOCK_CTOR &&
        type != TD_SHARED_LOCK_CTOR) {
      continue;
    }

    if (candidate_call->arg_size() < 2) {
      continue;
    }

    const Value *wrapper_object =
        candidate_call->getArgOperand(0)->stripPointerCasts();
    if (wrapper_object != lock_object) {
      continue;
    }

    if (!recordCandidate(candidate_call->getArgOperand(1))) {
      return lock_or_mutex;
    }
  }

  return resolved_mutex ? resolved_mutex : lock_or_mutex;
}

ThreadAPI::TD_TYPE ThreadAPI::stringToType(StringRef s) {
  static const auto *type_map =
      []() -> std::unordered_map<std::string, ThreadAPI::TD_TYPE> * {
    auto *map = new std::unordered_map<std::string, ThreadAPI::TD_TYPE>();
    for (int raw = static_cast<int>(ThreadAPI::TD_DUMMY);
         raw <= static_cast<int>(ThreadAPI::TD_KERNEL_MEMORY_BARRIER); ++raw) {
      ThreadAPI::TD_TYPE type = static_cast<ThreadAPI::TD_TYPE>(raw);
      const char *name = ThreadAPI::tdTypeToString(type);
      if (name && name[0] != '<') {
        (*map)[name] = type;
      }
    }
    return map;
  }();

  auto it = type_map->find(s.str());
  return it != type_map->end() ? it->second : ThreadAPI::TD_DUMMY;
}

ThreadAPI::ForkArgIndices
ThreadAPI::getForkArgIndices(const Function *F) const {
  if (!F)
    return ForkArgIndices{};
  auto it = m_fork_args.find(normalizeAPIName(F->getName()));
  if (it != m_fork_args.end())
    return it->second;
  return ForkArgIndices{};
}

ThreadAPI::JoinArgIndices
ThreadAPI::getJoinArgIndices(const Function *F) const {
  if (!F)
    return JoinArgIndices{};
  auto it = m_join_args.find(normalizeAPIName(F->getName()));
  if (it != m_join_args.end())
    return it->second;
  return JoinArgIndices{};
}

void ThreadAPI::loadConfig(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    return;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::stringstream ss(line);
    std::string name, typeStr;
    if (ss >> name >> typeStr) {
      TD_TYPE type = ThreadAPI::stringToType(typeStr);
      if (type != TD_DUMMY) {
        addEntry(name, type);
        const std::string normalized_name = normalizeConfiguredAPIName(name);
        if (type == TD_FORK) {
          unsigned t = 0, s = 2, a = 3;
          if (ss >> t >> s >> a)
            m_fork_args[normalized_name] = ForkArgIndices{t, s, a};
        } else if (type == TD_JOIN) {
          unsigned t = 0, r = 1;
          if (ss >> t >> r)
            m_join_args[normalized_name] = JoinArgIndices{t, r};
        }
      }
    }
  }
}

void ThreadAPI::loadSemanticConfig(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    return;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::stringstream ss(line);
    std::string name;
    std::string typeStr;
    if (!(ss >> name >> typeStr)) {
      continue;
    }

    TD_TYPE type = ThreadAPI::stringToType(typeStr);
    if (type == TD_DUMMY) {
      continue;
    }

    APIDescription description;
    description.type = type;
    description.from_config = true;
    description.library = inferLibrary(type);
    MatchKind match_kind = MatchKind::Exact;

    std::string token;
    while (ss >> token) {
      size_t eq = token.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      StringRef key(token.data(), eq);
      StringRef value(token.data() + eq + 1, token.size() - eq - 1);
      if (key.equals("library")) {
        description.library = parseRuntimeLibrary(value);
      } else if (key.equals("semantic")) {
        description.semantic_tag = value.str();
      } else if (key.equals("match")) {
        std::string lowered = value.lower();
        match_kind = lowered == "prefix" ? MatchKind::Prefix : MatchKind::Exact;
      } else if (key.equals("traits")) {
        SmallVector<StringRef, 8> entries;
        value.split(entries, ',', -1, false);
        for (StringRef entry : entries) {
          if (!entry.empty()) {
            description.traits.push_back(entry.trim().str());
          }
        }
      }
    }

    if (match_kind == MatchKind::Exact) {
      addEntry(name, type);
      addDescription(name, description);
    }
    addMatchRule(name, match_kind, description);
  }
}

ThreadAPI::RuntimeLibrary ThreadAPI::inferLibrary(TD_TYPE type) const {
  switch (type) {
  case TD_FORK:
  case TD_JOIN:
  case TD_DETACH:
  case TD_ACQUIRE:
  case TD_TRY_ACQUIRE:
  case TD_RWLOCK_RDLOCK:
  case TD_RWLOCK_WRLOCK:
  case TD_RELEASE:
  case TD_EXIT:
  case TD_CANCEL:
  case TD_COND_WAIT:
  case TD_COND_SIGNAL:
  case TD_COND_BROADCAST:
  case TD_MUTEX_INI:
  case TD_MUTEX_DESTROY:
  case TD_CONDVAR_INI:
  case TD_CONDVAR_DESTROY:
  case TD_BAR_INIT:
  case TD_BAR_WAIT:
    return RuntimeLibrary::PThread;
  case HARE_PAR_FOR:
    return RuntimeLibrary::Hare;
  case TD_SHARED_RDLOCK:
  case TD_SHARED_WRLOCK:
  case TD_SHARED_UNLOCK:
  case TD_CALL_ONCE:
  case TD_FUTURE_GET:
  case TD_FUTURE_WAIT:
  case TD_PROMISE_SET:
  case TD_ASYNC:
  case TD_LOCK_GUARD_CTOR:
  case TD_LOCK_GUARD_DTOR:
  case TD_UNIQUE_LOCK_CTOR:
  case TD_UNIQUE_LOCK_DTOR:
  case TD_UNIQUE_LOCK_LOCK:
  case TD_UNIQUE_LOCK_UNLOCK:
  case TD_SCOPED_LOCK_CTOR:
  case TD_SCOPED_LOCK_DTOR:
  case TD_SHARED_LOCK_CTOR:
  case TD_SHARED_LOCK_DTOR:
  case TD_JTHREAD_FORK:
  case TD_JTHREAD_JOIN:
  case TD_JTHREAD_DTOR:
  case TD_ATOMIC_WAIT:
  case TD_ATOMIC_NOTIFY_ONE:
  case TD_ATOMIC_NOTIFY_ALL:
  case TD_LATCH_COUNT_DOWN:
  case TD_LATCH_WAIT:
  case TD_LATCH_ARRIVE_WAIT:
  case TD_BARRIER_ARRIVE_WAIT:
  case TD_BARRIER_ARRIVE:
  case TD_BARRIER_WAIT_CPP20:
  case TD_SEMAPHORE_ACQUIRE:
  case TD_SEMAPHORE_RELEASE:
  case TD_SEMAPHORE_TRY_ACQUIRE:
    return RuntimeLibrary::Cpp;
  case TD_OMP_TASK:
  case TD_OMP_TASKWAIT:
  case TD_OMP_TASKWAIT_DEPS:
  case TD_OMP_TASKYIELD:
  case TD_OMP_TASKGROUP_START:
  case TD_OMP_TASKGROUP_END:
  case TD_OMP_TASK_WITH_DEPS:
  case TD_OMP_TASKLOOP:
  case TD_OMP_TASK_COMPLETE:
  case TD_OMP_SINGLE_START:
  case TD_OMP_SINGLE_END:
  case TD_OMP_MASTER_START:
  case TD_OMP_MASTER_END:
  case TD_OMP_ORDERED_START:
  case TD_OMP_ORDERED_END:
  case TD_OMP_REDUCE_START:
  case TD_OMP_REDUCE_END:
  case TD_OMP_REDUCE_NOWAIT_START:
  case TD_OMP_REDUCE_NOWAIT_END:
  case TD_OMP_FOR_STATIC_INIT:
  case TD_OMP_FOR_STATIC_FINI:
  case TD_OMP_FOR_DISPATCH_INIT:
  case TD_OMP_FOR_DISPATCH_NEXT:
  case TD_OMP_FOR_DISPATCH_FINI:
  case TD_OMP_SECTIONS_INIT:
  case TD_OMP_SECTIONS_NEXT:
  case TD_OMP_SECTIONS_END:
  case TD_OMP_ATOMIC_START:
  case TD_OMP_ATOMIC_END:
  case TD_OMP_FLUSH:
  case TD_OMP_CRITICAL_START:
  case TD_OMP_CRITICAL_END:
  case TD_OMP_PARALLEL_START:
  case TD_OMP_TARGET:
  case TD_OMP_TARGET_DATA_BEGIN:
  case TD_OMP_TARGET_DATA_END:
  case TD_OMP_TARGET_DATA_UPDATE:
  case TD_OMP_TEAMS:
  case TD_OMP_TEAMS_HOST:
  case TD_OMP_TEAMS_DISTRIBUTE:
  case TD_OMP_DISTRIBUTE:
  case TD_OMP_DISTRIBUTE_STATIC:
  case TD_OMP_DISTRIBUTE_DYNAMIC:
  case TD_OMP_DISTRIBUTE_GUIDANCE:
  case TD_OMP_LOOP_STATIC_INIT:
  case TD_OMP_LOOP_DYNAMIC_INIT:
  case TD_OMP_LOOP_GUIDANCE_INIT:
  case TD_OMP_AFFINITY:
  case TD_OMP_SCOPE_START:
  case TD_OMP_SCOPE_END:
  case TD_OMP_TASKLOOP_SIMD:
  case TD_OMP_TASKLOOP_FINI:
  case TD_OMP_INTEROP_INIT:
  case TD_OMP_INTEROP_FINI:
  case TD_OMP_DOACROSS_INIT:
  case TD_OMP_DOACROSS_WAIT:
  case TD_OMP_DOACROSS_SUBMIT:
    return RuntimeLibrary::OpenMP;
  case TD_MPI_INIT:
  case TD_MPI_FINALIZE:
  case TD_MPI_SESSION_INIT:
  case TD_MPI_SESSION_FINALIZE:
  case TD_MPI_SESSION_GET_INFO:
  case TD_MPI_SESSION_GET_NUM_ERRCODES:
  case TD_MPI_SESSION_GET_ERRHANDLER:
  case TD_MPI_SESSION_SET_ERRHANDLER:
  case TD_MPI_ERRHANDLER_CREATE:
  case TD_MPI_ERRHANDLER_FREE:
  case TD_MPI_COMM_GET_ERRHANDLER:
  case TD_MPI_COMM_SET_ERRHANDLER:
  case TD_MPI_COMM_CALL_ERRHANDLER:
  case TD_MPI_WIN_GET_ERRHANDLER:
  case TD_MPI_WIN_SET_ERRHANDLER:
  case TD_MPI_FILE_GET_ERRHANDLER:
  case TD_MPI_FILE_SET_ERRHANDLER:
  case TD_MPI_ERROR_CLASS:
  case TD_MPI_ERROR_STRING:
  case TD_MPI_INFO_CREATE:
  case TD_MPI_INFO_DUP:
  case TD_MPI_INFO_FREE:
  case TD_MPI_INFO_GET:
  case TD_MPI_INFO_GET_VALUELEN:
  case TD_MPI_INFO_GET_NKEYS:
  case TD_MPI_INFO_GET_NTHKEY:
  case TD_MPI_INFO_GET_KEYVAL:
  case TD_MPI_INFO_SET:
  case TD_MPI_INFO_DELETE:
  case TD_MPI_INFO_C2F:
  case TD_MPI_INFO_CREATE_ENV:
  case TD_MPI_INFO_FREE_ENV:
  case TD_MPI_GET_COUNT:
  case TD_MPI_GET_ELEMENTS:
  case TD_MPI_GET_ELEMENTS_X:
  case TD_MPI_STATUS_SIZE:
  case TD_MPI_STATUS_SET_ELEMENTS:
  case TD_MPI_STATUS_SET_ELEMENTS_X:
  case TD_MPI_SEND:
  case TD_MPI_RECV:
  case TD_MPI_SENDRECV:
  case TD_MPI_PROBE:
  case TD_MPI_ISEND:
  case TD_MPI_IRECV:
  case TD_MPI_IPROBE:
  case TD_MPI_MPROBE:
  case TD_MPI_IMPROBE:
  case TD_MPI_IMRECV:
  case TD_MPI_MRECV:
  case TD_MPI_PERSISTENT_SEND_INIT:
  case TD_MPI_PERSISTENT_RECV_INIT:
  case TD_MPI_REQUEST_START:
  case TD_MPI_WAIT:
  case TD_MPI_WAITALL:
  case TD_MPI_WAITANY:
  case TD_MPI_WAITSOME:
  case TD_MPI_TEST:
  case TD_MPI_TESTALL:
  case TD_MPI_TESTANY:
  case TD_MPI_TESTSOME:
  case TD_MPI_BARRIER:
  case TD_MPI_BCAST:
  case TD_MPI_SCATTER:
  case TD_MPI_GATHER:
  case TD_MPI_ALLGATHER:
  case TD_MPI_ALLTOALL:
  case TD_MPI_REDUCE:
  case TD_MPI_ALLREDUCE:
  case TD_MPI_REDUCE_SCATTER:
  case TD_MPI_SCAN:
  case TD_MPI_WIN_CREATE:
  case TD_MPI_WIN_FREE:
  case TD_MPI_PUT:
  case TD_MPI_GET:
  case TD_MPI_ACCUMULATE:
  case TD_MPI_WIN_FENCE:
  case TD_MPI_WIN_LOCK:
  case TD_MPI_WIN_UNLOCK:
  case TD_MPI_WIN_FLUSH:
  case TD_MPI_WIN_SYNC:
  case TD_MPI_WIN_POST:
  case TD_MPI_WIN_START:
  case TD_MPI_WIN_COMPLETE:
  case TD_MPI_WIN_WAIT:
  case TD_MPI_WIN_TEST:
  case TD_MPI_COMM_DUP:
  case TD_MPI_COMM_SPLIT:
  case TD_MPI_COMM_CREATE:
  case TD_MPI_COMM_FREE:
  case TD_MPI_REQUEST_FREE:
  case TD_MPI_CANCEL:
  case TD_MPI_TYPE_CONTIGUOUS:
  case TD_MPI_TYPE_VECTOR:
  case TD_MPI_TYPE_HVECTOR:
  case TD_MPI_TYPE_INDEXED:
  case TD_MPI_TYPE_HINDEXED:
  case TD_MPI_TYPE_STRUCT:
  case TD_MPI_TYPE_CREATE_DLPACK:
  case TD_MPI_TYPE_CREATE_SUBARRAY:
  case TD_MPI_TYPE_CREATE_DARRAY:
  case TD_MPI_TYPE_CREATE_RESIZED:
  case TD_MPI_TYPE_CREATE_HINDEXED:
  case TD_MPI_TYPE_CREATE_HVECTOR:
  case TD_MPI_TYPE_GET_EXTENT:
  case TD_MPI_TYPE_GET_TRUE_EXTENT:
  case TD_MPI_TYPE_SIZE:
  case TD_MPI_TYPE_COMMIT:
  case TD_MPI_CART_CREATE:
  case TD_MPI_CART_DIMS_CREATE:
  case TD_MPI_CART_GET:
  case TD_MPI_CART_SHIFT:
  case TD_MPI_CART_COORDS:
  case TD_MPI_CART_RANK:
  case TD_MPI_CART_SUB:
  case TD_MPI_DIST_GRAPH_CREATE:
  case TD_MPI_DIST_GRAPH_CREATE_ADJACENT:
  case TD_MPI_DIST_GRAPH_NEIGHBORS:
  case TD_MPI_DIST_GRAPH_NEIGHBORS_COUNT:
  case TD_MPI_GRAPH_CREATE:
  case TD_MPI_GRAPH_GET:
  case TD_MPI_GRAPH_NEIGHBORS:
  case TD_MPI_GRAPH_NEIGHBORS_COUNT:
  case TD_MPI_GRAPH_DIMS_GET:
  case TD_MPI_GRAPH_MAP:
    return RuntimeLibrary::MPI;
  default:
    return RuntimeLibrary::Unknown;
  }
}

namespace {

using Owner = ThreadAPI::SemanticLoweringOwner;
using LoweringKind = ThreadAPI::SemanticLoweringKind;
using LoweringInfo = ThreadAPI::SemanticLoweringInfo;
using TD = ThreadAPI::TD_TYPE;

constexpr uint32_t ownerMask(Owner owner) {
  return static_cast<uint32_t>(owner);
}

constexpr uint32_t ownerMask(Owner owner_a, Owner owner_b) {
  return ownerMask(owner_a) | ownerMask(owner_b);
}

constexpr uint32_t ownerMask(Owner owner_a, Owner owner_b, Owner owner_c) {
  return ownerMask(owner_a, owner_b) | ownerMask(owner_c);
}

bool isExplicitFallbackType(TD type) {
  switch (type) {
  case TD::TD_DUMMY:
  case TD::TD_OMP_ATOMIC_START:
  case TD::TD_OMP_ATOMIC_END:
  case TD::TD_OMP_TARGET_DATA_UPDATE:
  case TD::TD_OMP_CANCEL:
  case TD::TD_OMP_TEAMS:
  case TD::TD_OMP_TEAMS_HOST:
  case TD::TD_OMP_TEAMS_DISTRIBUTE:
  case TD::TD_OMP_DISTRIBUTE:
  case TD::TD_OMP_DISTRIBUTE_STATIC:
  case TD::TD_OMP_DISTRIBUTE_DYNAMIC:
  case TD::TD_OMP_DISTRIBUTE_GUIDANCE:
  case TD::TD_OMP_LOOP_STATIC_INIT:
  case TD::TD_OMP_LOOP_DYNAMIC_INIT:
  case TD::TD_OMP_LOOP_GUIDANCE_INIT:
  case TD::TD_OMP_AFFINITY:
  case TD::TD_OMP_SCOPE_START:
  case TD::TD_OMP_SCOPE_END:
  case TD::TD_OMP_TASKLOOP_SIMD:
  case TD::TD_OMP_TASKLOOP_FINI:
  case TD::TD_OMP_INTEROP_INIT:
  case TD::TD_OMP_INTEROP_FINI:
  case TD::TD_MPI_SESSION_GET_INFO:
  case TD::TD_MPI_SESSION_GET_NUM_ERRCODES:
  case TD::TD_MPI_SESSION_GET_ERRHANDLER:
  case TD::TD_MPI_SESSION_SET_ERRHANDLER:
  case TD::TD_MPI_ERRHANDLER_CREATE:
  case TD::TD_MPI_ERRHANDLER_FREE:
  case TD::TD_MPI_COMM_GET_ERRHANDLER:
  case TD::TD_MPI_COMM_SET_ERRHANDLER:
  case TD::TD_MPI_COMM_CALL_ERRHANDLER:
  case TD::TD_MPI_WIN_GET_ERRHANDLER:
  case TD::TD_MPI_WIN_SET_ERRHANDLER:
  case TD::TD_MPI_FILE_GET_ERRHANDLER:
  case TD::TD_MPI_FILE_SET_ERRHANDLER:
  case TD::TD_MPI_ERROR_CLASS:
  case TD::TD_MPI_ERROR_STRING:
  case TD::TD_MPI_INFO_CREATE:
  case TD::TD_MPI_INFO_DUP:
  case TD::TD_MPI_INFO_FREE:
  case TD::TD_MPI_INFO_GET:
  case TD::TD_MPI_INFO_GET_VALUELEN:
  case TD::TD_MPI_INFO_GET_NKEYS:
  case TD::TD_MPI_INFO_GET_NTHKEY:
  case TD::TD_MPI_INFO_GET_KEYVAL:
  case TD::TD_MPI_INFO_SET:
  case TD::TD_MPI_INFO_DELETE:
  case TD::TD_MPI_INFO_C2F:
  case TD::TD_MPI_INFO_CREATE_ENV:
  case TD::TD_MPI_INFO_FREE_ENV:
  case TD::TD_MPI_GET_COUNT:
  case TD::TD_MPI_GET_ELEMENTS:
  case TD::TD_MPI_GET_ELEMENTS_X:
  case TD::TD_MPI_STATUS_SIZE:
  case TD::TD_MPI_STATUS_SET_ELEMENTS:
  case TD::TD_MPI_STATUS_SET_ELEMENTS_X:
    return true;
  default:
    return false;
  }
}

bool isLockLikeType(TD type) {
  switch (type) {
  case TD::TD_ACQUIRE:
  case TD::TD_TRY_ACQUIRE:
  case TD::TD_RWLOCK_RDLOCK:
  case TD::TD_RWLOCK_WRLOCK:
  case TD::TD_RELEASE:
  case TD::TD_SHARED_RDLOCK:
  case TD::TD_SHARED_WRLOCK:
  case TD::TD_SHARED_UNLOCK:
  case TD::TD_LOCK_GUARD_CTOR:
  case TD::TD_LOCK_GUARD_DTOR:
  case TD::TD_UNIQUE_LOCK_CTOR:
  case TD::TD_UNIQUE_LOCK_DTOR:
  case TD::TD_UNIQUE_LOCK_LOCK:
  case TD::TD_UNIQUE_LOCK_UNLOCK:
  case TD::TD_SCOPED_LOCK_CTOR:
  case TD::TD_SCOPED_LOCK_DTOR:
  case TD::TD_SHARED_LOCK_CTOR:
  case TD::TD_SHARED_LOCK_DTOR:
  case TD::TD_OMP_ORDERED_START:
  case TD::TD_OMP_ORDERED_END:
  case TD::TD_KERNEL_SPIN_LOCK:
  case TD::TD_KERNEL_SPIN_UNLOCK:
  case TD::TD_KERNEL_SPIN_TRYLOCK:
  case TD::TD_KERNEL_MUTEX_LOCK:
  case TD::TD_KERNEL_MUTEX_UNLOCK:
  case TD::TD_KERNEL_MUTEX_TRYLOCK:
  case TD::TD_KERNEL_DOWN:
  case TD::TD_KERNEL_UP:
  case TD::TD_KERNEL_READ_LOCK:
  case TD::TD_KERNEL_READ_UNLOCK:
  case TD::TD_KERNEL_WRITE_LOCK:
  case TD::TD_KERNEL_WRITE_UNLOCK:
  case TD::TD_KERNEL_DOWN_READ:
  case TD::TD_KERNEL_UP_READ:
  case TD::TD_KERNEL_DOWN_WRITE:
  case TD::TD_KERNEL_UP_WRITE:
    return true;
  default:
    return false;
  }
}

bool isSemaphoreTypeForLowering(TD type) {
  return type == TD::TD_SEMAPHORE_ACQUIRE ||
         type == TD::TD_SEMAPHORE_RELEASE ||
         type == TD::TD_SEMAPHORE_TRY_ACQUIRE;
}

bool isBarrierHBType(TD type) {
  switch (type) {
  case TD::TD_BAR_WAIT:
  case TD::TD_LATCH_COUNT_DOWN:
  case TD::TD_LATCH_WAIT:
  case TD::TD_LATCH_ARRIVE_WAIT:
  case TD::TD_BARRIER_ARRIVE_WAIT:
  case TD::TD_BARRIER_ARRIVE:
  case TD::TD_BARRIER_WAIT_CPP20:
  case TD::TD_CALL_ONCE:
  case TD::TD_FUTURE_GET:
  case TD::TD_FUTURE_WAIT:
  case TD::TD_PROMISE_SET:
  case TD::TD_OMP_TASK:
  case TD::TD_OMP_TASKWAIT:
  case TD::TD_OMP_TASKWAIT_DEPS:
  case TD::TD_OMP_TASKGROUP_START:
  case TD::TD_OMP_TASKGROUP_END:
  case TD::TD_OMP_TASK_WITH_DEPS:
  case TD::TD_OMP_TASKLOOP:
  case TD::TD_OMP_TASK_COMPLETE:
  case TD::TD_OMP_SINGLE_END:
  case TD::TD_OMP_REDUCE_START:
  case TD::TD_OMP_FOR_STATIC_FINI:
  case TD::TD_OMP_FOR_DISPATCH_FINI:
  case TD::TD_OMP_SECTIONS_END:
    return true;
  default:
    return false;
  }
}

bool isMHPThreadType(TD type) {
  switch (type) {
  case TD::TD_FORK:
  case TD::TD_JOIN:
  case TD::TD_DETACH:
  case TD::TD_EXIT:
  case TD::TD_CANCEL:
  case TD::TD_COND_WAIT:
  case TD::TD_COND_SIGNAL:
  case TD::TD_COND_BROADCAST:
  case TD::TD_BAR_WAIT:
  case TD::TD_JTHREAD_FORK:
  case TD::TD_JTHREAD_JOIN:
  case TD::TD_JTHREAD_DTOR:
  case TD::TD_ASYNC:
  case TD::HARE_PAR_FOR:
    return true;
  default:
    return false;
  }
}

bool isOpenMPTaskType(TD type) {
  switch (type) {
  case TD::TD_OMP_TASK:
  case TD::TD_OMP_TASKWAIT:
  case TD::TD_OMP_TASKWAIT_DEPS:
  case TD::TD_OMP_TASKYIELD:
  case TD::TD_OMP_TASKGROUP_START:
  case TD::TD_OMP_TASKGROUP_END:
  case TD::TD_OMP_TASK_WITH_DEPS:
  case TD::TD_OMP_TASKLOOP:
  case TD::TD_OMP_TASK_COMPLETE:
  case TD::TD_OMP_DOACROSS_WAIT:
  case TD::TD_OMP_DOACROSS_SUBMIT:
  case TD::TD_OMP_SINGLE_START:
  case TD::TD_OMP_SINGLE_END:
  case TD::TD_OMP_MASTER_START:
  case TD::TD_OMP_MASTER_END:
  case TD::TD_OMP_ORDERED_START:
  case TD::TD_OMP_ORDERED_END:
  case TD::TD_OMP_REDUCE_START:
  case TD::TD_OMP_REDUCE_END:
  case TD::TD_OMP_REDUCE_NOWAIT_START:
  case TD::TD_OMP_REDUCE_NOWAIT_END:
  case TD::TD_OMP_FOR_STATIC_INIT:
  case TD::TD_OMP_FOR_STATIC_FINI:
  case TD::TD_OMP_FOR_DISPATCH_INIT:
  case TD::TD_OMP_FOR_DISPATCH_NEXT:
  case TD::TD_OMP_FOR_DISPATCH_FINI:
  case TD::TD_OMP_SECTIONS_INIT:
  case TD::TD_OMP_SECTIONS_NEXT:
  case TD::TD_OMP_SECTIONS_END:
  case TD::TD_FORK:
    return true;
  default:
    return false;
  }
}

LoweringInfo makeLoweringInfo(TD type, ThreadAPI::RuntimeLibrary library,
                              llvm::StringRef semantic_tag) {
  if (type == TD::TD_DUMMY) {
    return {LoweringKind::RecognizedButUnmodeled, "unknown-api",
            ownerMask(Owner::ExplicitFallback)};
  }
  if (type == TD::TD_ASYNC) {
    return {LoweringKind::Deferred, "async-launch-policy-witness",
            ownerMask(Owner::HB, Owner::MHP)};
  }
  if (type == TD::TD_ATOMIC_WAIT) {
    return {LoweringKind::RecognizedButUnmodeled,
            "cpp-atomic-wait-runtime-unmodeled",
            ownerMask(Owner::ExplicitFallback)};
  }
  if (type == TD::TD_ATOMIC_NOTIFY_ONE || type == TD::TD_ATOMIC_NOTIFY_ALL) {
    return {LoweringKind::RecognizedButUnmodeled,
            "cpp-atomic-notify-runtime-unmodeled",
            ownerMask(Owner::ExplicitFallback)};
  }
  if (isSemaphoreTypeForLowering(type)) {
    return {LoweringKind::RecognizedButUnmodeled,
            "counting-semaphore-runtime-unmodeled",
            ownerMask(Owner::ExplicitFallback)};
  }
  if (type == TD::TD_OMP_ATOMIC_START || type == TD::TD_OMP_ATOMIC_END) {
    return {LoweringKind::RecognizedButUnmodeled,
            "openmp-atomic-runtime-unmodeled",
            ownerMask(Owner::OpenMP, Owner::ExplicitFallback)};
  }
  if (isExplicitFallbackType(type)) {
    const char *reason = library == ThreadAPI::RuntimeLibrary::MPI
                             ? "metadata-only-mpi-api"
                             : "recognized-openmp-runtime-unmodeled";
    uint32_t owners = ownerMask(Owner::ExplicitFallback);
    if (library == ThreadAPI::RuntimeLibrary::MPI) {
      owners |= ownerMask(Owner::MPI);
    } else if (library == ThreadAPI::RuntimeLibrary::OpenMP) {
      owners |= ownerMask(Owner::OpenMP);
    }
    return {LoweringKind::RecognizedButUnmodeled, reason, owners};
  }

  uint32_t owners = 0;
  switch (library) {
  case ThreadAPI::RuntimeLibrary::OpenMP:
    owners |= ownerMask(Owner::OpenMP);
    break;
  case ThreadAPI::RuntimeLibrary::MPI:
    owners |= ownerMask(Owner::MPI);
    break;
  case ThreadAPI::RuntimeLibrary::PThread:
  case ThreadAPI::RuntimeLibrary::Cpp:
  case ThreadAPI::RuntimeLibrary::LinuxKernel:
  case ThreadAPI::RuntimeLibrary::Hare:
  case ThreadAPI::RuntimeLibrary::Custom:
    owners |= ownerMask(Owner::MHP);
    break;
  case ThreadAPI::RuntimeLibrary::Unknown:
    break;
  }

  if (isLockLikeType(type)) {
    owners |= ownerMask(Owner::LockSet, Owner::MHP);
  }
  if (isBarrierHBType(type)) {
    owners |= ownerMask(Owner::HB);
  }
  if (isMHPThreadType(type)) {
    owners |= ownerMask(Owner::MHP);
  }
  if (isOpenMPTaskType(type) || semantic_tag.startswith("task") ||
      semantic_tag.startswith("taskgroup") || semantic_tag.startswith("doacross")) {
    owners |= ownerMask(Owner::HB, Owner::MHP);
  }
  if (library == ThreadAPI::RuntimeLibrary::MPI &&
      type == TD::TD_MPI_COMM_DUP &&
      semantic_tag.equals("comm-idup")) {
    owners |= ownerMask(Owner::MPI);
  }
  if (owners == 0) {
    owners = ownerMask(Owner::ExplicitFallback);
  }
  return {LoweringKind::Modeled, "modeled", owners};
}

} // namespace

bool ThreadAPI::isLibraryEnabled(RuntimeLibrary library) const {
  switch (library) {
  case RuntimeLibrary::OpenMP:
    return m_config.enable_openmp();
  case RuntimeLibrary::MPI:
    return m_config.enable_mpi();
  case RuntimeLibrary::Cpp:
    return m_config.enable_cpp11();
  case RuntimeLibrary::LinuxKernel:
    return m_config.enable_linux_kernel();
  case RuntimeLibrary::PThread:
  case RuntimeLibrary::Hare:
  case RuntimeLibrary::Custom:
  case RuntimeLibrary::Unknown:
    return true;
  }
  return true;
}

const ThreadAPI::APIDescription *
ThreadAPI::lookupDescription(const Function *F) const {
  if (!F) {
    return nullptr;
  }
  for (const std::string &name : getConfiguredLookupNames(F->getName())) {
    auto it = m_api_descriptions.find(name);
    if (it != m_api_descriptions.end()) {
      return &it->second;
    }
  }
  return nullptr;
}

const ThreadAPI::MatchRule *
ThreadAPI::lookupMatchRule(StringRef normalized_name) const {
  const MatchRule *best_prefix = nullptr;
  for (const MatchRule &rule : m_match_rules) {
    if (rule.kind == MatchKind::Exact) {
      if (normalized_name.equals(rule.pattern)) {
        return &rule;
      }
      continue;
    }
    if (!normalized_name.startswith(rule.pattern)) {
      continue;
    }
    if (!best_prefix || rule.pattern.size() > best_prefix->pattern.size()) {
      best_prefix = &rule;
    }
  }
  return best_prefix;
}

ThreadAPI::TD_TYPE
ThreadAPI::getConfiguredType(StringRef normalized_name) const {
  for (const std::string &lookup_name : getConfiguredLookupNames(normalized_name)) {
    TDAPIMap::const_iterator it = tdAPIMap.find(lookup_name);
    if (it != tdAPIMap.end()) {
      auto desc_it = m_api_descriptions.find(lookup_name);
      if (desc_it == m_api_descriptions.end() ||
          isLibraryEnabled(desc_it->second.library)) {
        return it->second;
      }
      return TD_DUMMY;
    }
    if (const MatchRule *rule = lookupMatchRule(lookup_name)) {
      return isLibraryEnabled(rule->description.library)
                 ? rule->description.type
                 : TD_DUMMY;
    }
  }

  if (looksLikeMPISymbol(normalized_name)) {
    const std::string normalized_mpi =
        mpi::normalizeMPISymbolName(stripAPIGlobalPrefix(normalized_name));
    for (const auto &entry : tdAPIMap) {
      StringRef configured = entry.first();
      if (!configured.startswith("MPI_")) {
        continue;
      }
      if (!mpi::equalsCaseInsensitiveASCII(configured, normalized_mpi)) {
        continue;
      }
      auto desc_it = m_api_descriptions.find(configured.str());
      if (desc_it == m_api_descriptions.end() ||
          isLibraryEnabled(desc_it->second.library)) {
        return entry.second;
      }
      return TD_DUMMY;
    }
  }

  return TD_DUMMY;
}

ThreadAPI::APIDescription ThreadAPI::describe(const Function *F) const {
  APIDescription description;
  if (!F) {
    return description;
  }
  if (const APIDescription *configured = lookupDescription(F)) {
    return *configured;
  }
  for (const std::string &lookup_name : getConfiguredLookupNames(F->getName())) {
    if (const MatchRule *rule = lookupMatchRule(lookup_name)) {
      return rule->description;
    }
  }
  description.type = getType(F);
  description.library = inferLibrary(description.type);
  return description;
}

ThreadAPI::SemanticLoweringInfo
ThreadAPI::getSemanticLoweringInfo(TD_TYPE type) const {
  return makeLoweringInfo(type, inferLibrary(type), "");
}

ThreadAPI::SemanticLoweringInfo
ThreadAPI::getSemanticLoweringInfo(const Function *F) const {
  if (!F) {
    return getSemanticLoweringInfo(TD_DUMMY);
  }
  const APIDescription description = describe(F);
  SemanticLoweringInfo info =
      makeLoweringInfo(description.type, description.library,
                       description.semantic_tag);
  const bool has_semaphore_trait =
      llvm::is_contained(description.traits, std::string("semaphore"));
  const bool is_binary_semaphore =
      llvm::is_contained(description.traits, std::string("binary-semaphore")) ||
      F->getName().contains("binary_semaphore");
    if (isSemaphoreTypeForLowering(description.type) || has_semaphore_trait) {
    if (is_binary_semaphore) {
      info.kind = SemanticLoweringKind::Modeled;
      info.reason = "modeled";
      info.owners = semanticLoweringOwnerMask(SemanticLoweringOwner::LockSet,
                                              SemanticLoweringOwner::MHP);
    } else {
      info.kind = SemanticLoweringKind::RecognizedButUnmodeled;
      info.reason = "counting-semaphore-runtime-unmodeled";
      info.owners = semanticLoweringOwnerMask(
          SemanticLoweringOwner::ExplicitFallback,
          SemanticLoweringOwner::HB);
    }
  }
  return info;
}

ThreadAPI::TD_TYPE ThreadAPI::getType(const Function *F) const {
  if (!F)
    return TD_DUMMY;

  // 1. Exact match (including loaded config)
  std::string nameStr = normalizeAPIName(F->getName());
  TD_TYPE configured_type = getConfiguredType(nameStr);
  if (configured_type != TD_DUMMY) {
    return configured_type;
  }

  StringRef name = nameStr;

  // 2. C++11/17/20 Support (if enabled)
  if (m_config.enable_cpp11()) {
    // Basic threading
    if (CppThreadingModel::isFork(name))
      return TD_FORK;
    if (CppThreadingModel::isJoin(name))
      return TD_JOIN;
    if (CppThreadingModel::isDetach(name))
      return TD_DETACH;

    // Basic mutex operations
    if (CppThreadingModel::isAcquire(name))
      return TD_ACQUIRE;
    if (CppThreadingModel::isTryAcquire(name))
      return TD_TRY_ACQUIRE;
    if (CppThreadingModel::isRelease(name))
      return TD_RELEASE;

    // Condition variables
    if (CppThreadingModel::isCondWait(name))
      return TD_COND_WAIT;
    if (CppThreadingModel::isCondSignal(name))
      return TD_COND_SIGNAL;
    if (CppThreadingModel::isCondBroadcast(name))
      return TD_COND_BROADCAST;

    // C++17 shared_mutex
    if (CppThreadingModel::isSharedLockAcquire(name) ||
        CppThreadingModel::isSharedTimedLockAcquire(name))
      return TD_SHARED_RDLOCK;
    if (CppThreadingModel::isSharedLockExclusiveAcquire(name) ||
        CppThreadingModel::isSharedTimedLockExclusiveAcquire(name))
      return TD_SHARED_WRLOCK;
    if (CppThreadingModel::isSharedLockRelease(name) ||
        CppThreadingModel::isSharedLockExclusiveRelease(name) ||
        CppThreadingModel::isSharedTimedLockRelease(name) ||
        CppThreadingModel::isSharedTimedLockExclusiveRelease(name))
      return TD_SHARED_UNLOCK;

    // RAII lock wrappers
    if (CppThreadingModel::isLockGuardConstructor(name))
      return TD_LOCK_GUARD_CTOR;
    if (CppThreadingModel::isLockGuardDestructor(name))
      return TD_LOCK_GUARD_DTOR;
    if (CppThreadingModel::isUniqueLockConstructor(name))
      return TD_UNIQUE_LOCK_CTOR;
    if (CppThreadingModel::isUniqueLockDestructor(name))
      return TD_UNIQUE_LOCK_DTOR;
    if (CppThreadingModel::isUniqueLockLock(name))
      return TD_UNIQUE_LOCK_LOCK;
    if (CppThreadingModel::isUniqueLockUnlock(name))
      return TD_UNIQUE_LOCK_UNLOCK;
    if (CppThreadingModel::isScopedLockConstructor(name))
      return TD_SCOPED_LOCK_CTOR;
    if (CppThreadingModel::isScopedLockDestructor(name))
      return TD_SCOPED_LOCK_DTOR;
    if (CppThreadingModel::isSharedLockConstructor(name))
      return TD_SHARED_LOCK_CTOR;
    if (CppThreadingModel::isSharedLockDestructor(name))
      return TD_SHARED_LOCK_DTOR;

    // std::call_once
    if (CppThreadingModel::isCallOnce(name))
      return TD_CALL_ONCE;

    // Future/Promise synchronization
    if (CppThreadingModel::isFutureGet(name))
      return TD_FUTURE_GET;
    if (CppThreadingModel::isFutureWait(name))
      return TD_FUTURE_WAIT;
    if (CppThreadingModel::isPromiseSetValue(name) ||
        CppThreadingModel::isPromiseSetException(name))
      return TD_PROMISE_SET;
    if (CppThreadingModel::isAsync(name))
      return TD_ASYNC;

    // C++20 jthread
    if (CppThreadingModel::isJthreadConstructor(name))
      return TD_JTHREAD_FORK;
    if (CppThreadingModel::isJthreadJoin(name))
      return TD_JTHREAD_JOIN;
    if (CppThreadingModel::isJthreadDestructor(name))
      return TD_JTHREAD_DTOR;

    // C++20 atomic wait/notify
    if (CppThreadingModel::isAtomicWait(name))
      return TD_ATOMIC_WAIT;
    if (CppThreadingModel::isAtomicNotifyOne(name))
      return TD_ATOMIC_NOTIFY_ONE;
    if (CppThreadingModel::isAtomicNotifyAll(name))
      return TD_ATOMIC_NOTIFY_ALL;

    // C++20 latch
    if (CppThreadingModel::isLatchCountDown(name))
      return TD_LATCH_COUNT_DOWN;
    if (CppThreadingModel::isLatchWait(name))
      return TD_LATCH_WAIT;
    if (CppThreadingModel::isLatchArriveAndWait(name))
      return TD_LATCH_ARRIVE_WAIT;

    // C++20 barrier
    if (CppThreadingModel::isBarrierArriveAndWait(name))
      return TD_BARRIER_ARRIVE_WAIT;
    if (CppThreadingModel::isBarrierArrive(name))
      return TD_BARRIER_ARRIVE;
    if (CppThreadingModel::isBarrierWait(name))
      return TD_BARRIER_WAIT_CPP20;

    // C++20 semaphore
    if (CppThreadingModel::isSemaphoreAcquire(name))
      return TD_SEMAPHORE_ACQUIRE;
    if (CppThreadingModel::isSemaphoreRelease(name))
      return TD_SEMAPHORE_RELEASE;
    if (CppThreadingModel::isSemaphoreTryAcquire(name))
      return TD_SEMAPHORE_TRY_ACQUIRE;
  }

  // 4. Linux Kernel Support (if enabled)
  if (m_config.enable_linux_kernel()) {
    // Spinlocks
    if (LinuxKernelModel::isSpinLockInit(name))
      return TD_KERNEL_SPIN_LOCK_INIT;
    if (LinuxKernelModel::isSpinLock(name))
      return TD_KERNEL_SPIN_LOCK;
    if (LinuxKernelModel::isSpinUnlock(name))
      return TD_KERNEL_SPIN_UNLOCK;
    if (LinuxKernelModel::isSpinTryLock(name))
      return TD_KERNEL_SPIN_TRYLOCK;

    // Mutexes
    if (LinuxKernelModel::isMutexInit(name))
      return TD_KERNEL_MUTEX_INIT;
    if (LinuxKernelModel::isMutexLock(name))
      return TD_KERNEL_MUTEX_LOCK;
    if (LinuxKernelModel::isMutexUnlock(name))
      return TD_KERNEL_MUTEX_UNLOCK;
    if (LinuxKernelModel::isMutexTryLock(name))
      return TD_KERNEL_MUTEX_TRYLOCK;

    // Semaphores
    if (LinuxKernelModel::isSemaInit(name))
      return TD_KERNEL_SEMA_INIT;
    if (LinuxKernelModel::isDown(name))
      return TD_KERNEL_DOWN;
    if (LinuxKernelModel::isUp(name))
      return TD_KERNEL_UP;

    // Read-Write Locks
    if (LinuxKernelModel::isReadLock(name))
      return TD_KERNEL_READ_LOCK;
    if (LinuxKernelModel::isReadUnlock(name))
      return TD_KERNEL_READ_UNLOCK;
    if (LinuxKernelModel::isWriteLock(name))
      return TD_KERNEL_WRITE_LOCK;
    if (LinuxKernelModel::isWriteUnlock(name))
      return TD_KERNEL_WRITE_UNLOCK;

    // Read-Write Semaphores
    if (LinuxKernelModel::isDownRead(name))
      return TD_KERNEL_DOWN_READ;
    if (LinuxKernelModel::isUpRead(name))
      return TD_KERNEL_UP_READ;
    if (LinuxKernelModel::isDownWrite(name))
      return TD_KERNEL_DOWN_WRITE;
    if (LinuxKernelModel::isUpWrite(name))
      return TD_KERNEL_UP_WRITE;
    if (LinuxKernelModel::isInitRwsem(name))
      return TD_KERNEL_INIT_RWSEM;

    // RCU
    if (LinuxKernelModel::isRcuReadLock(name))
      return TD_KERNEL_RCU_READ_LOCK;
    if (LinuxKernelModel::isRcuReadUnlock(name))
      return TD_KERNEL_RCU_READ_UNLOCK;
    if (LinuxKernelModel::isSynchronizeRcu(name))
      return TD_KERNEL_SYNCHRONIZE_RCU;
    if (LinuxKernelModel::isCallRcu(name))
      return TD_KERNEL_CALL_RCU;
    if (LinuxKernelModel::isRcuDereference(name))
      return TD_KERNEL_RCU_DEREFERENCE;
    if (LinuxKernelModel::isRcuAssignPointer(name))
      return TD_KERNEL_RCU_ASSIGN_POINTER;

    // Seq Locks
    if (LinuxKernelModel::isSeqlockInit(name))
      return TD_KERNEL_SEQLOCK_INIT;
    if (LinuxKernelModel::isReadSeqbegin(name))
      return TD_KERNEL_READ_SEQBEGIN;
    if (LinuxKernelModel::isReadSeqretry(name))
      return TD_KERNEL_READ_SEQRETRY;
    if (LinuxKernelModel::isWriteSeqlock(name))
      return TD_KERNEL_WRITE_SEQLOCK;
    if (LinuxKernelModel::isWriteSequnlock(name))
      return TD_KERNEL_WRITE_SEQUNLOCK;

    // Completion Variables
    if (LinuxKernelModel::isInitCompletion(name))
      return TD_KERNEL_INIT_COMPLETION;
    if (LinuxKernelModel::isWaitForCompletion(name))
      return TD_KERNEL_WAIT_FOR_COMPLETION;
    if (LinuxKernelModel::isCompleteAll(name))
      return TD_KERNEL_COMPLETE_ALL;
    if (LinuxKernelModel::isCompleteOne(name))
      return TD_KERNEL_COMPLETE;

    // Wait Queues
    if (LinuxKernelModel::isInitWaitqueueHead(name))
      return TD_KERNEL_INIT_WAITQUEUE_HEAD;
    if (LinuxKernelModel::isWaitEvent(name))
      return TD_KERNEL_WAIT_EVENT;
    if (LinuxKernelModel::isWakeUp(name))
      return TD_KERNEL_WAKE_UP;
    if (LinuxKernelModel::isPrepareToWait(name))
      return TD_KERNEL_PREPARE_TO_WAIT;
    if (LinuxKernelModel::isFinishWait(name))
      return TD_KERNEL_FINISH_WAIT;

    // Memory Barriers
    if (LinuxKernelModel::isMemoryBarrier(name))
      return TD_KERNEL_MEMORY_BARRIER;
  }

  return TD_DUMMY;
}

/*!
 * Get the callee function from an instruction
 */
const Function *ThreadAPI::getCallee(const Instruction *inst) const {
  if (const CallBase *cb = dyn_cast<CallBase>(inst)) {
    if (const Function *direct = cb->getCalledFunction())
      return direct;
    if (const Value *called = cb->getCalledOperand())
      return dyn_cast<Function>(called->stripPointerCasts());
  }
  return nullptr;
}

/*!
 * Get the callee function from a CallBase
 */
const Function *ThreadAPI::getCallee(const CallBase *cb) const {
  if (cb) {
    if (const Function *direct = cb->getCalledFunction())
      return direct;
    if (const Value *called = cb->getCalledOperand())
      return dyn_cast<Function>(called->stripPointerCasts());
  }
  return nullptr;
}

/*!
 * Get the CallBase from an instruction
 */
const CallBase *ThreadAPI::getLLVMCallSite(const Instruction *inst) const {
  return dyn_cast<CallBase>(inst);
}

/*!
 *
 */
void ThreadAPI::statInit(llvm::StringMap<u32_t> &tdAPIStatMap) {

  tdAPIStatMap["pthread_create"] = 0;

  tdAPIStatMap["pthread_join"] = 0;

  tdAPIStatMap["pthread_mutex_lock"] = 0;

  tdAPIStatMap["pthread_mutex_trylock"] = 0;

  tdAPIStatMap["pthread_mutex_unlock"] = 0;

  tdAPIStatMap["pthread_cancel"] = 0;

  tdAPIStatMap["pthread_exit"] = 0;

  tdAPIStatMap["pthread_detach"] = 0;

  tdAPIStatMap["pthread_cond_wait"] = 0;

  tdAPIStatMap["pthread_cond_signal"] = 0;

  tdAPIStatMap["pthread_cond_broadcast"] = 0;

  tdAPIStatMap["pthread_cond_init"] = 0;

  tdAPIStatMap["pthread_cond_destroy"] = 0;

  tdAPIStatMap["pthread_mutex_init"] = 0;

  tdAPIStatMap["pthread_mutex_destroy"] = 0;

  tdAPIStatMap["pthread_barrier_init"] = 0;

  tdAPIStatMap["pthread_barrier_wait"] = 0;

  tdAPIStatMap["hare_parallel_for"] = 0;
}

void ThreadAPI::performAPIStat(Module *module) {

  llvm::StringMap<u32_t> tdAPIStatMap;

  statInit(tdAPIStatMap);

  for (Module::iterator it = module->begin(), eit = module->end(); it != eit;
       ++it) {

    for (inst_iterator II = inst_begin(*it), E = inst_end(*it); II != E; ++II) {
      const Instruction *inst = &*II;
      if (!llvm::isa<CallInst>(inst) && !llvm::isa<InvokeInst>(inst))
        continue;
      const Function *fun = getCallee(inst);
      TD_TYPE type = getType(fun);
      switch (type) {
      case TD_FORK: {
        tdAPIStatMap["pthread_create"]++;
        break;
      }
      case TD_JOIN: {
        tdAPIStatMap["pthread_join"]++;
        break;
      }
      case TD_ACQUIRE: {
        tdAPIStatMap["pthread_mutex_lock"]++;
        break;
      }
      case TD_TRY_ACQUIRE: {
        tdAPIStatMap["pthread_mutex_trylock"]++;
        break;
      }
      case TD_RWLOCK_RDLOCK: {
        tdAPIStatMap["pthread_rwlock_rdlock"]++;
        break;
      }
      case TD_RWLOCK_WRLOCK: {
        tdAPIStatMap["pthread_rwlock_wrlock"]++;
        break;
      }
      case TD_RELEASE: {
        tdAPIStatMap["pthread_mutex_unlock"]++;
        break;
      }
      case TD_CANCEL: {
        tdAPIStatMap["pthread_cancel"]++;
        break;
      }
      case TD_EXIT: {
        tdAPIStatMap["pthread_exit"]++;
        break;
      }
      case TD_DETACH: {
        tdAPIStatMap["pthread_detach"]++;
        break;
      }
      case TD_COND_WAIT: {
        tdAPIStatMap["pthread_cond_wait"]++;
        break;
      }
      case TD_COND_SIGNAL: {
        tdAPIStatMap["pthread_cond_signal"]++;
        break;
      }
      case TD_COND_BROADCAST: {
        tdAPIStatMap["pthread_cond_broadcast"]++;
        break;
      }
      case TD_CONDVAR_INI: {
        tdAPIStatMap["pthread_cond_init"]++;
        break;
      }
      case TD_CONDVAR_DESTROY: {
        tdAPIStatMap["pthread_cond_destroy"]++;
        break;
      }
      case TD_MUTEX_INI: {
        tdAPIStatMap["pthread_mutex_init"]++;
        break;
      }
      case TD_MUTEX_DESTROY: {
        tdAPIStatMap["pthread_mutex_destroy"]++;
        break;
      }
      case TD_BAR_INIT: {
        tdAPIStatMap["pthread_barrier_init"]++;
        break;
      }
      case TD_BAR_WAIT: {
        tdAPIStatMap["pthread_barrier_wait"]++;
        break;
      }
      case HARE_PAR_FOR: {
        tdAPIStatMap["hare_parallel_for"]++;
        break;
      }
      case TD_DUMMY:
      default: {
        // Handle TD_DUMMY and all other thread API types (C++11/17/20, OpenMP,
        // MPI, etc.) These are not explicitly tracked in statistics
        break;
      }
      }
    }
  }

  StringRef n(module->getModuleIdentifier());
  StringRef name = n.split('/').second;
  name = name.split('.').first;
  std::string nameStr = name.str();
  std::cout << "################ (program : " << nameStr
            << ")###############\n";
  std::cout.flags(std::ios::left);
  unsigned field_width = 20;
  for (llvm::StringMap<u32_t>::iterator it = tdAPIStatMap.begin(),
                                        eit = tdAPIStatMap.end();
       it != eit; ++it) {
    std::string apiName = it->first().str();
    // format out put with width 20 space
    std::cout << std::setw(field_width) << apiName << " : " << it->second
              << "\n";
  }
  std::cout << "#######################################################"
            << "\n";
}

const char *ThreadAPI::tdTypeToString(TD_TYPE t) {
  switch (t) {
  case TD_DUMMY:
    return "TD_DUMMY";
  case TD_FORK:
    return "TD_FORK";
  case TD_JOIN:
    return "TD_JOIN";
  case TD_DETACH:
    return "TD_DETACH";
  case TD_ACQUIRE:
    return "TD_ACQUIRE";
  case TD_TRY_ACQUIRE:
    return "TD_TRY_ACQUIRE";
  case TD_RWLOCK_RDLOCK:
    return "TD_RWLOCK_RDLOCK";
  case TD_RWLOCK_WRLOCK:
    return "TD_RWLOCK_WRLOCK";
  case TD_RELEASE:
    return "TD_RELEASE";
  case TD_EXIT:
    return "TD_EXIT";
  case TD_CANCEL:
    return "TD_CANCEL";
  case TD_COND_WAIT:
    return "TD_COND_WAIT";
  case TD_COND_SIGNAL:
    return "TD_COND_SIGNAL";
  case TD_COND_BROADCAST:
    return "TD_COND_BROADCAST";
  case TD_MUTEX_INI:
    return "TD_MUTEX_INI";
  case TD_MUTEX_DESTROY:
    return "TD_MUTEX_DESTROY";
  case TD_CONDVAR_INI:
    return "TD_CONDVAR_INI";
  case TD_CONDVAR_DESTROY:
    return "TD_CONDVAR_DESTROY";
  case TD_BAR_INIT:
    return "TD_BAR_INIT";
  case TD_BAR_WAIT:
    return "TD_BAR_WAIT";
  case HARE_PAR_FOR:
    return "HARE_PAR_FOR";
  case TD_SHARED_RDLOCK:
    return "TD_SHARED_RDLOCK";
  case TD_SHARED_WRLOCK:
    return "TD_SHARED_WRLOCK";
  case TD_SHARED_UNLOCK:
    return "TD_SHARED_UNLOCK";
  case TD_CALL_ONCE:
    return "TD_CALL_ONCE";
  case TD_FUTURE_GET:
    return "TD_FUTURE_GET";
  case TD_FUTURE_WAIT:
    return "TD_FUTURE_WAIT";
  case TD_PROMISE_SET:
    return "TD_PROMISE_SET";
  case TD_ASYNC:
    return "TD_ASYNC";
  case TD_LOCK_GUARD_CTOR:
    return "TD_LOCK_GUARD_CTOR";
  case TD_LOCK_GUARD_DTOR:
    return "TD_LOCK_GUARD_DTOR";
  case TD_UNIQUE_LOCK_CTOR:
    return "TD_UNIQUE_LOCK_CTOR";
  case TD_UNIQUE_LOCK_DTOR:
    return "TD_UNIQUE_LOCK_DTOR";
  case TD_UNIQUE_LOCK_LOCK:
    return "TD_UNIQUE_LOCK_LOCK";
  case TD_UNIQUE_LOCK_UNLOCK:
    return "TD_UNIQUE_LOCK_UNLOCK";
  case TD_SCOPED_LOCK_CTOR:
    return "TD_SCOPED_LOCK_CTOR";
  case TD_SCOPED_LOCK_DTOR:
    return "TD_SCOPED_LOCK_DTOR";
  case TD_SHARED_LOCK_CTOR:
    return "TD_SHARED_LOCK_CTOR";
  case TD_SHARED_LOCK_DTOR:
    return "TD_SHARED_LOCK_DTOR";
  case TD_JTHREAD_FORK:
    return "TD_JTHREAD_FORK";
  case TD_JTHREAD_JOIN:
    return "TD_JTHREAD_JOIN";
  case TD_JTHREAD_DTOR:
    return "TD_JTHREAD_DTOR";
  case TD_ATOMIC_WAIT:
    return "TD_ATOMIC_WAIT";
  case TD_ATOMIC_NOTIFY_ONE:
    return "TD_ATOMIC_NOTIFY_ONE";
  case TD_ATOMIC_NOTIFY_ALL:
    return "TD_ATOMIC_NOTIFY_ALL";
  case TD_LATCH_COUNT_DOWN:
    return "TD_LATCH_COUNT_DOWN";
  case TD_LATCH_WAIT:
    return "TD_LATCH_WAIT";
  case TD_LATCH_ARRIVE_WAIT:
    return "TD_LATCH_ARRIVE_WAIT";
  case TD_BARRIER_ARRIVE_WAIT:
    return "TD_BARRIER_ARRIVE_WAIT";
  case TD_BARRIER_ARRIVE:
    return "TD_BARRIER_ARRIVE";
  case TD_BARRIER_WAIT_CPP20:
    return "TD_BARRIER_WAIT_CPP20";
  case TD_SEMAPHORE_ACQUIRE:
    return "TD_SEMAPHORE_ACQUIRE";
  case TD_SEMAPHORE_RELEASE:
    return "TD_SEMAPHORE_RELEASE";
  case TD_SEMAPHORE_TRY_ACQUIRE:
    return "TD_SEMAPHORE_TRY_ACQUIRE";
  case TD_OMP_TASK:
    return "TD_OMP_TASK";
  case TD_OMP_TASKWAIT:
    return "TD_OMP_TASKWAIT";
  case TD_OMP_TASKWAIT_DEPS:
    return "TD_OMP_TASKWAIT_DEPS";
  case TD_OMP_TASKYIELD:
    return "TD_OMP_TASKYIELD";
  case TD_OMP_TASKGROUP_START:
    return "TD_OMP_TASKGROUP_START";
  case TD_OMP_TASKGROUP_END:
    return "TD_OMP_TASKGROUP_END";
  case TD_OMP_TASK_WITH_DEPS:
    return "TD_OMP_TASK_WITH_DEPS";
  case TD_OMP_TASKLOOP:
    return "TD_OMP_TASKLOOP";
  case TD_OMP_TASK_COMPLETE:
    return "TD_OMP_TASK_COMPLETE";
  case TD_OMP_SINGLE_START:
    return "TD_OMP_SINGLE_START";
  case TD_OMP_SINGLE_END:
    return "TD_OMP_SINGLE_END";
  case TD_OMP_MASTER_START:
    return "TD_OMP_MASTER_START";
  case TD_OMP_MASTER_END:
    return "TD_OMP_MASTER_END";
  case TD_OMP_ORDERED_START:
    return "TD_OMP_ORDERED_START";
  case TD_OMP_ORDERED_END:
    return "TD_OMP_ORDERED_END";
  case TD_OMP_REDUCE_START:
    return "TD_OMP_REDUCE_START";
  case TD_OMP_REDUCE_END:
    return "TD_OMP_REDUCE_END";
  case TD_OMP_REDUCE_NOWAIT_START:
    return "TD_OMP_REDUCE_NOWAIT_START";
  case TD_OMP_REDUCE_NOWAIT_END:
    return "TD_OMP_REDUCE_NOWAIT_END";
  case TD_OMP_FOR_STATIC_INIT:
    return "TD_OMP_FOR_STATIC_INIT";
  case TD_OMP_FOR_STATIC_FINI:
    return "TD_OMP_FOR_STATIC_FINI";
  case TD_OMP_FOR_DISPATCH_INIT:
    return "TD_OMP_FOR_DISPATCH_INIT";
  case TD_OMP_FOR_DISPATCH_NEXT:
    return "TD_OMP_FOR_DISPATCH_NEXT";
  case TD_OMP_FOR_DISPATCH_FINI:
    return "TD_OMP_FOR_DISPATCH_FINI";
  case TD_OMP_SECTIONS_INIT:
    return "TD_OMP_SECTIONS_INIT";
  case TD_OMP_SECTIONS_NEXT:
    return "TD_OMP_SECTIONS_NEXT";
  case TD_OMP_SECTIONS_END:
    return "TD_OMP_SECTIONS_END";
  case TD_OMP_ATOMIC_START:
    return "TD_OMP_ATOMIC_START";
  case TD_OMP_ATOMIC_END:
    return "TD_OMP_ATOMIC_END";
  case TD_OMP_FLUSH:
    return "TD_OMP_FLUSH";
  case TD_OMP_CANCEL:
    return "TD_OMP_CANCEL";
  case TD_OMP_TARGET:
    return "TD_OMP_TARGET";
  case TD_OMP_TARGET_DATA_BEGIN:
    return "TD_OMP_TARGET_DATA_BEGIN";
  case TD_OMP_TARGET_DATA_END:
    return "TD_OMP_TARGET_DATA_END";
  case TD_OMP_TARGET_DATA_UPDATE:
    return "TD_OMP_TARGET_DATA_UPDATE";
  case TD_OMP_TEAMS:
    return "TD_OMP_TEAMS";
  case TD_OMP_TEAMS_HOST:
    return "TD_OMP_TEAMS_HOST";
  case TD_OMP_TEAMS_DISTRIBUTE:
    return "TD_OMP_TEAMS_DISTRIBUTE";
  case TD_OMP_DISTRIBUTE:
    return "TD_OMP_DISTRIBUTE";
  case TD_OMP_DISTRIBUTE_STATIC:
    return "TD_OMP_DISTRIBUTE_STATIC";
  case TD_OMP_DISTRIBUTE_DYNAMIC:
    return "TD_OMP_DISTRIBUTE_DYNAMIC";
  case TD_OMP_DISTRIBUTE_GUIDANCE:
    return "TD_OMP_DISTRIBUTE_GUIDANCE";
  case TD_OMP_LOOP_STATIC_INIT:
    return "TD_OMP_LOOP_STATIC_INIT";
  case TD_OMP_LOOP_DYNAMIC_INIT:
    return "TD_OMP_LOOP_DYNAMIC_INIT";
  case TD_OMP_LOOP_GUIDANCE_INIT:
    return "TD_OMP_LOOP_GUIDANCE_INIT";
  case TD_OMP_AFFINITY:
    return "TD_OMP_AFFINITY";
  case TD_OMP_SCOPE_START:
    return "TD_OMP_SCOPE_START";
  case TD_OMP_SCOPE_END:
    return "TD_OMP_SCOPE_END";
  case TD_OMP_TASKLOOP_SIMD:
    return "TD_OMP_TASKLOOP_SIMD";
  case TD_OMP_TASKLOOP_FINI:
    return "TD_OMP_TASKLOOP_FINI";
  case TD_OMP_INTEROP_INIT:
    return "TD_OMP_INTEROP_INIT";
  case TD_OMP_INTEROP_FINI:
    return "TD_OMP_INTEROP_FINI";
  case TD_OMP_DOACROSS_INIT:
    return "TD_OMP_DOACROSS_INIT";
  case TD_OMP_DOACROSS_WAIT:
    return "TD_OMP_DOACROSS_WAIT";
  case TD_OMP_DOACROSS_SUBMIT:
    return "TD_OMP_DOACROSS_SUBMIT";
  case TD_MPI_SESSION_INIT:
    return "TD_MPI_SESSION_INIT";
  case TD_MPI_SESSION_FINALIZE:
    return "TD_MPI_SESSION_FINALIZE";
  case TD_MPI_SESSION_GET_INFO:
    return "TD_MPI_SESSION_GET_INFO";
  case TD_MPI_SESSION_GET_NUM_ERRCODES:
    return "TD_MPI_SESSION_GET_NUM_ERRCODES";
  case TD_MPI_SESSION_GET_ERRHANDLER:
    return "TD_MPI_SESSION_GET_ERRHANDLER";
  case TD_MPI_SESSION_SET_ERRHANDLER:
    return "TD_MPI_SESSION_SET_ERRHANDLER";
  case TD_MPI_ERRHANDLER_CREATE:
    return "TD_MPI_ERRHANDLER_CREATE";
  case TD_MPI_ERRHANDLER_FREE:
    return "TD_MPI_ERRHANDLER_FREE";
  case TD_MPI_COMM_GET_ERRHANDLER:
    return "TD_MPI_COMM_GET_ERRHANDLER";
  case TD_MPI_COMM_SET_ERRHANDLER:
    return "TD_MPI_COMM_SET_ERRHANDLER";
  case TD_MPI_COMM_CALL_ERRHANDLER:
    return "TD_MPI_COMM_CALL_ERRHANDLER";
  case TD_MPI_WIN_GET_ERRHANDLER:
    return "TD_MPI_WIN_GET_ERRHANDLER";
  case TD_MPI_WIN_SET_ERRHANDLER:
    return "TD_MPI_WIN_SET_ERRHANDLER";
  case TD_MPI_FILE_GET_ERRHANDLER:
    return "TD_MPI_FILE_GET_ERRHANDLER";
  case TD_MPI_FILE_SET_ERRHANDLER:
    return "TD_MPI_FILE_SET_ERRHANDLER";
  case TD_MPI_ERROR_CLASS:
    return "TD_MPI_ERROR_CLASS";
  case TD_MPI_ERROR_STRING:
    return "TD_MPI_ERROR_STRING";
  case TD_MPI_INFO_CREATE:
    return "TD_MPI_INFO_CREATE";
  case TD_MPI_INFO_DUP:
    return "TD_MPI_INFO_DUP";
  case TD_MPI_INFO_FREE:
    return "TD_MPI_INFO_FREE";
  case TD_MPI_INFO_GET:
    return "TD_MPI_INFO_GET";
  case TD_MPI_INFO_GET_VALUELEN:
    return "TD_MPI_INFO_GET_VALUELEN";
  case TD_MPI_INFO_GET_NKEYS:
    return "TD_MPI_INFO_GET_NKEYS";
  case TD_MPI_INFO_GET_NTHKEY:
    return "TD_MPI_INFO_GET_NTHKEY";
  case TD_MPI_INFO_GET_KEYVAL:
    return "TD_MPI_INFO_GET_KEYVAL";
  case TD_MPI_INFO_SET:
    return "TD_MPI_INFO_SET";
  case TD_MPI_INFO_DELETE:
    return "TD_MPI_INFO_DELETE";
  case TD_MPI_INFO_C2F:
    return "TD_MPI_INFO_C2F";
  case TD_MPI_INFO_CREATE_ENV:
    return "TD_MPI_INFO_CREATE_ENV";
  case TD_MPI_INFO_FREE_ENV:
    return "TD_MPI_INFO_FREE_ENV";
  case TD_MPI_GET_COUNT:
    return "TD_MPI_GET_COUNT";
  case TD_MPI_GET_ELEMENTS:
    return "TD_MPI_GET_ELEMENTS";
  case TD_MPI_GET_ELEMENTS_X:
    return "TD_MPI_GET_ELEMENTS_X";
  case TD_MPI_STATUS_SIZE:
    return "TD_MPI_STATUS_SIZE";
  case TD_MPI_STATUS_SET_ELEMENTS:
    return "TD_MPI_STATUS_SET_ELEMENTS";
  case TD_MPI_STATUS_SET_ELEMENTS_X:
    return "TD_MPI_STATUS_SET_ELEMENTS_X";
  case TD_MPI_INIT:
    return "TD_MPI_INIT";
  case TD_MPI_FINALIZE:
    return "TD_MPI_FINALIZE";
  case TD_MPI_SEND:
    return "TD_MPI_SEND";
  case TD_MPI_RECV:
    return "TD_MPI_RECV";
  case TD_MPI_SENDRECV:
    return "TD_MPI_SENDRECV";
  case TD_MPI_PROBE:
    return "TD_MPI_PROBE";
  case TD_MPI_ISEND:
    return "TD_MPI_ISEND";
  case TD_MPI_IRECV:
    return "TD_MPI_IRECV";
  case TD_MPI_IPROBE:
    return "TD_MPI_IPROBE";
  case TD_MPI_MPROBE:
    return "TD_MPI_MPROBE";
  case TD_MPI_IMPROBE:
    return "TD_MPI_IMPROBE";
  case TD_MPI_IMRECV:
    return "TD_MPI_IMRECV";
  case TD_MPI_MRECV:
    return "TD_MPI_MRECV";
  case TD_MPI_PERSISTENT_SEND_INIT:
    return "TD_MPI_PERSISTENT_SEND_INIT";
  case TD_MPI_PERSISTENT_RECV_INIT:
    return "TD_MPI_PERSISTENT_RECV_INIT";
  case TD_MPI_REQUEST_START:
    return "TD_MPI_REQUEST_START";
  case TD_MPI_WAIT:
    return "TD_MPI_WAIT";
  case TD_MPI_WAITALL:
    return "TD_MPI_WAITALL";
  case TD_MPI_WAITANY:
    return "TD_MPI_WAITANY";
  case TD_MPI_WAITSOME:
    return "TD_MPI_WAITSOME";
  case TD_MPI_TEST:
    return "TD_MPI_TEST";
  case TD_MPI_TESTALL:
    return "TD_MPI_TESTALL";
  case TD_MPI_TESTANY:
    return "TD_MPI_TESTANY";
  case TD_MPI_TESTSOME:
    return "TD_MPI_TESTSOME";
  case TD_MPI_BARRIER:
    return "TD_MPI_BARRIER";
  case TD_MPI_BCAST:
    return "TD_MPI_BCAST";
  case TD_MPI_SCATTER:
    return "TD_MPI_SCATTER";
  case TD_MPI_GATHER:
    return "TD_MPI_GATHER";
  case TD_MPI_ALLGATHER:
    return "TD_MPI_ALLGATHER";
  case TD_MPI_ALLTOALL:
    return "TD_MPI_ALLTOALL";
  case TD_MPI_REDUCE:
    return "TD_MPI_REDUCE";
  case TD_MPI_ALLREDUCE:
    return "TD_MPI_ALLREDUCE";
  case TD_MPI_REDUCE_SCATTER:
    return "TD_MPI_REDUCE_SCATTER";
  case TD_MPI_SCAN:
    return "TD_MPI_SCAN";
  case TD_MPI_WIN_CREATE:
    return "TD_MPI_WIN_CREATE";
  case TD_MPI_WIN_FREE:
    return "TD_MPI_WIN_FREE";
  case TD_MPI_PUT:
    return "TD_MPI_PUT";
  case TD_MPI_GET:
    return "TD_MPI_GET";
  case TD_MPI_ACCUMULATE:
    return "TD_MPI_ACCUMULATE";
  case TD_MPI_WIN_FENCE:
    return "TD_MPI_WIN_FENCE";
  case TD_MPI_WIN_LOCK:
    return "TD_MPI_WIN_LOCK";
  case TD_MPI_WIN_UNLOCK:
    return "TD_MPI_WIN_UNLOCK";
  case TD_MPI_WIN_FLUSH:
    return "TD_MPI_WIN_FLUSH";
  case TD_MPI_WIN_SYNC:
    return "TD_MPI_WIN_SYNC";
  case TD_MPI_WIN_POST:
    return "TD_MPI_WIN_POST";
  case TD_MPI_WIN_START:
    return "TD_MPI_WIN_START";
  case TD_MPI_WIN_COMPLETE:
    return "TD_MPI_WIN_COMPLETE";
  case TD_MPI_WIN_WAIT:
    return "TD_MPI_WIN_WAIT";
  case TD_MPI_WIN_TEST:
    return "TD_MPI_WIN_TEST";
  case TD_MPI_COMM_DUP:
    return "TD_MPI_COMM_DUP";
  case TD_MPI_COMM_SPLIT:
    return "TD_MPI_COMM_SPLIT";
  case TD_MPI_COMM_CREATE:
    return "TD_MPI_COMM_CREATE";
  case TD_MPI_COMM_FREE:
    return "TD_MPI_COMM_FREE";
  case TD_MPI_REQUEST_FREE:
    return "TD_MPI_REQUEST_FREE";
  case TD_MPI_CANCEL:
    return "TD_MPI_CANCEL";
  case TD_MPI_TYPE_CONTIGUOUS:
    return "TD_MPI_TYPE_CONTIGUOUS";
  case TD_MPI_TYPE_VECTOR:
    return "TD_MPI_TYPE_VECTOR";
  case TD_MPI_TYPE_HVECTOR:
    return "TD_MPI_TYPE_HVECTOR";
  case TD_MPI_TYPE_INDEXED:
    return "TD_MPI_TYPE_INDEXED";
  case TD_MPI_TYPE_HINDEXED:
    return "TD_MPI_TYPE_HINDEXED";
  case TD_MPI_TYPE_STRUCT:
    return "TD_MPI_TYPE_STRUCT";
  case TD_MPI_TYPE_CREATE_DLPACK:
    return "TD_MPI_TYPE_CREATE_DLPACK";
  case TD_MPI_TYPE_CREATE_SUBARRAY:
    return "TD_MPI_TYPE_CREATE_SUBARRAY";
  case TD_MPI_TYPE_CREATE_DARRAY:
    return "TD_MPI_TYPE_CREATE_DARRAY";
  case TD_MPI_TYPE_CREATE_RESIZED:
    return "TD_MPI_TYPE_CREATE_RESIZED";
  case TD_MPI_TYPE_CREATE_HINDEXED:
    return "TD_MPI_TYPE_CREATE_HINDEXED";
  case TD_MPI_TYPE_CREATE_HVECTOR:
    return "TD_MPI_TYPE_CREATE_HVECTOR";
  case TD_MPI_TYPE_GET_EXTENT:
    return "TD_MPI_TYPE_GET_EXTENT";
  case TD_MPI_TYPE_GET_TRUE_EXTENT:
    return "TD_MPI_TYPE_GET_TRUE_EXTENT";
  case TD_MPI_TYPE_SIZE:
    return "TD_MPI_TYPE_SIZE";
  case TD_MPI_TYPE_COMMIT:
    return "TD_MPI_TYPE_COMMIT";
  case TD_MPI_CART_CREATE:
    return "TD_MPI_CART_CREATE";
  case TD_MPI_CART_DIMS_CREATE:
    return "TD_MPI_CART_DIMS_CREATE";
  case TD_MPI_CART_GET:
    return "TD_MPI_CART_GET";
  case TD_MPI_CART_SHIFT:
    return "TD_MPI_CART_SHIFT";
  case TD_MPI_CART_COORDS:
    return "TD_MPI_CART_COORDS";
  case TD_MPI_CART_RANK:
    return "TD_MPI_CART_RANK";
  case TD_MPI_CART_SUB:
    return "TD_MPI_CART_SUB";
  case TD_MPI_DIST_GRAPH_CREATE:
    return "TD_MPI_DIST_GRAPH_CREATE";
  case TD_MPI_DIST_GRAPH_CREATE_ADJACENT:
    return "TD_MPI_DIST_GRAPH_CREATE_ADJACENT";
  case TD_MPI_DIST_GRAPH_NEIGHBORS:
    return "TD_MPI_DIST_GRAPH_NEIGHBORS";
  case TD_MPI_DIST_GRAPH_NEIGHBORS_COUNT:
    return "TD_MPI_DIST_GRAPH_NEIGHBORS_COUNT";
  case TD_MPI_GRAPH_CREATE:
    return "TD_MPI_GRAPH_CREATE";
  case TD_MPI_GRAPH_GET:
    return "TD_MPI_GRAPH_GET";
  case TD_MPI_GRAPH_NEIGHBORS:
    return "TD_MPI_GRAPH_NEIGHBORS";
  case TD_MPI_GRAPH_NEIGHBORS_COUNT:
    return "TD_MPI_GRAPH_NEIGHBORS_COUNT";
  case TD_MPI_GRAPH_DIMS_GET:
    return "TD_MPI_GRAPH_DIMS_GET";
  case TD_MPI_GRAPH_MAP:
    return "TD_MPI_GRAPH_MAP";
  case TD_KERNEL_SPIN_LOCK_INIT:
    return "TD_KERNEL_SPIN_LOCK_INIT";
  case TD_KERNEL_SPIN_LOCK:
    return "TD_KERNEL_SPIN_LOCK";
  case TD_KERNEL_SPIN_UNLOCK:
    return "TD_KERNEL_SPIN_UNLOCK";
  case TD_KERNEL_SPIN_TRYLOCK:
    return "TD_KERNEL_SPIN_TRYLOCK";
  case TD_KERNEL_MUTEX_INIT:
    return "TD_KERNEL_MUTEX_INIT";
  case TD_KERNEL_MUTEX_LOCK:
    return "TD_KERNEL_MUTEX_LOCK";
  case TD_KERNEL_MUTEX_UNLOCK:
    return "TD_KERNEL_MUTEX_UNLOCK";
  case TD_KERNEL_MUTEX_TRYLOCK:
    return "TD_KERNEL_MUTEX_TRYLOCK";
  case TD_KERNEL_SEMA_INIT:
    return "TD_KERNEL_SEMA_INIT";
  case TD_KERNEL_DOWN:
    return "TD_KERNEL_DOWN";
  case TD_KERNEL_UP:
    return "TD_KERNEL_UP";
  case TD_KERNEL_READ_LOCK:
    return "TD_KERNEL_READ_LOCK";
  case TD_KERNEL_READ_UNLOCK:
    return "TD_KERNEL_READ_UNLOCK";
  case TD_KERNEL_WRITE_LOCK:
    return "TD_KERNEL_WRITE_LOCK";
  case TD_KERNEL_WRITE_UNLOCK:
    return "TD_KERNEL_WRITE_UNLOCK";
  case TD_KERNEL_DOWN_READ:
    return "TD_KERNEL_DOWN_READ";
  case TD_KERNEL_UP_READ:
    return "TD_KERNEL_UP_READ";
  case TD_KERNEL_DOWN_WRITE:
    return "TD_KERNEL_DOWN_WRITE";
  case TD_KERNEL_UP_WRITE:
    return "TD_KERNEL_UP_WRITE";
  case TD_KERNEL_INIT_RWSEM:
    return "TD_KERNEL_INIT_RWSEM";
  case TD_KERNEL_RCU_READ_LOCK:
    return "TD_KERNEL_RCU_READ_LOCK";
  case TD_KERNEL_RCU_READ_UNLOCK:
    return "TD_KERNEL_RCU_READ_UNLOCK";
  case TD_KERNEL_SYNCHRONIZE_RCU:
    return "TD_KERNEL_SYNCHRONIZE_RCU";
  case TD_KERNEL_CALL_RCU:
    return "TD_KERNEL_CALL_RCU";
  case TD_KERNEL_RCU_DEREFERENCE:
    return "TD_KERNEL_RCU_DEREFERENCE";
  case TD_KERNEL_RCU_ASSIGN_POINTER:
    return "TD_KERNEL_RCU_ASSIGN_POINTER";
  case TD_KERNEL_SEQLOCK_INIT:
    return "TD_KERNEL_SEQLOCK_INIT";
  case TD_KERNEL_READ_SEQBEGIN:
    return "TD_KERNEL_READ_SEQBEGIN";
  case TD_KERNEL_READ_SEQRETRY:
    return "TD_KERNEL_READ_SEQRETRY";
  case TD_KERNEL_WRITE_SEQLOCK:
    return "TD_KERNEL_WRITE_SEQLOCK";
  case TD_KERNEL_WRITE_SEQUNLOCK:
    return "TD_KERNEL_WRITE_SEQUNLOCK";
  case TD_KERNEL_INIT_COMPLETION:
    return "TD_KERNEL_INIT_COMPLETION";
  case TD_KERNEL_WAIT_FOR_COMPLETION:
    return "TD_KERNEL_WAIT_FOR_COMPLETION";
  case TD_KERNEL_COMPLETE:
    return "TD_KERNEL_COMPLETE";
  case TD_KERNEL_COMPLETE_ALL:
    return "TD_KERNEL_COMPLETE_ALL";
  case TD_KERNEL_INIT_WAITQUEUE_HEAD:
    return "TD_KERNEL_INIT_WAITQUEUE_HEAD";
  case TD_KERNEL_WAIT_EVENT:
    return "TD_KERNEL_WAIT_EVENT";
  case TD_KERNEL_WAKE_UP:
    return "TD_KERNEL_WAKE_UP";
  case TD_KERNEL_PREPARE_TO_WAIT:
    return "TD_KERNEL_PREPARE_TO_WAIT";
  case TD_KERNEL_FINISH_WAIT:
    return "TD_KERNEL_FINISH_WAIT";
  case TD_KERNEL_MEMORY_BARRIER:
    return "TD_KERNEL_MEMORY_BARRIER";
  default:
    return "<unknown TD_TYPE>";
  }
}
