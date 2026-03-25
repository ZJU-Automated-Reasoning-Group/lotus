#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>

namespace OpenMPModel {

// Helper: return true if funcName equals any name in names.
// Defined as an inline function (not in an anonymous namespace) to avoid
// ODR violations when this header is included in multiple translation units.
inline bool matchesAny(const llvm::StringRef &funcName,
                       const std::vector<llvm::StringRef> &names) {
  for (auto const &name : names) {
    if (funcName.equals(name))
      return true;
  }
  return false;
}

inline bool isFork(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_fork_call");
}
inline bool isFork(const llvm::CallBase *callInst) {
  if (!callInst)
    return false;
  auto const func = callInst->getCalledFunction();
  // Guard against null callee (indirect call) and unnamed functions.
  if (!func || !func->hasName())
    return false;
  return isFork(func->getName());
}

inline bool isForStaticInit(const llvm::StringRef &funcName) {
  // Each version functions the same, only argument types slightly differ
  return matchesAny(funcName,
                    {"__kmpc_for_static_init_4", "__kmpc_for_static_init_4u",
                     "__kmpc_for_static_init_8", "__kmpc_for_static_init_8u"});
}
inline bool isForStaticFini(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_for_static_fini");
}

inline bool isForDispatchInit(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_dispatch_init");
}
inline bool isForDispatchNext(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_dispatch_next");
}
inline bool isForDispatchFini(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_dispatch_fini");
}

inline bool isSingleStart(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_single");
}
inline bool isSingleEnd(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_end_single");
}

inline bool isBarrier(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_barrier");
}

inline bool isReduceStart(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_reduce");
}
inline bool isReduceEnd(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_end_reduce");
}

inline bool isReduceNowaitStart(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_reduce_nowait");
}
inline bool isReduceNowaitEnd(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_end_reduce_nowait");
}

inline bool isCriticalStart(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_critical");
}
inline bool isCriticalEnd(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_end_critical");
}

inline bool isMasterStart(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_master");
}
inline bool isMasterEnd(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_end_master");
}

inline bool isSetLock(const llvm::StringRef &funcName) {
  return funcName.equals("omp_set_lock");
}
inline bool isUnsetLock(const llvm::StringRef &funcName) {
  return funcName.equals("omp_unset_lock");
}

inline bool isSetNestLock(const llvm::StringRef &funcName) {
  return funcName.equals("omp_set_nest_lock");
}
inline bool isUnsetNestLock(const llvm::StringRef &funcName) {
  return funcName.equals("omp_unset_nest_lock");
}

inline bool isOrderedStart(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_ordered");
}
inline bool isOrderedEnd(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_end_ordered");
}

// Return true for omp calls that do not need to be modelled (e.g.
// push_num_threads)
inline bool isNoEffect(const llvm::StringRef &funcName) {
  return matchesAny(funcName,
                    {"__kmpc_push_num_threads", "__kmpc_global_thread_num",
                     "__kmpc_copyprivate"})
         // we dont rely on reduce end to find end of reduce region
         || isReduceEnd(funcName) || isReduceNowaitEnd(funcName);
}

// Used only for debug to try and catch unhandled OpenMP calls
inline bool isOpenMP(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc") || funcName.startswith("GOMP_");
}

// Matches any OpenMP outlined functions, including the outer debug outlined
// functions
inline bool isOutlined(const llvm::StringRef &funcName) {
  return funcName.startswith(".omp_outlined.");
}

// When OpenMP is compiled with debug info an outer "debug" outline function is
// generated
inline bool isDebugOutlined(const llvm::StringRef &funcName) {
  return funcName.startswith(".omp_outlined._debug");
}

inline bool isTaskAlloc(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_omp_task_alloc");
}

inline bool isGetThreadNum(const llvm::StringRef &funcName) {
  return funcName.equals("omp_get_thread_num");
}

// OpenMP 3.0+ Task Support
inline bool isTask(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_omp_task") ||
         funcName.equals("__kmpc_omp_task_begin_if0");
}
inline bool isTask(const llvm::CallBase *callInst) {
  if (!callInst)
    return false;
  auto const func = callInst->getCalledFunction();
  if (!func || !func->hasName())
    return false;
  return isTask(func->getName());
}

inline bool isTaskwait(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_omp_taskwait");
}

inline bool isTaskyield(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_omp_taskyield");
}

inline bool isTaskgroupStart(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_taskgroup");
}

inline bool isTaskgroupEnd(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_end_taskgroup");
}

// OpenMP 4.0+ Task Dependencies
inline bool isTaskWithDeps(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_omp_task_with_deps");
}

inline bool isTaskwaitWithDeps(const llvm::StringRef &funcName) {
  return matchesAny(funcName,
                    {"__kmpc_omp_wait_deps", "__kmpc_omp_taskwait_deps_51"});
}

// OpenMP 4.5+ Taskloop Support
inline bool isTaskloop(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_taskloop");
}

inline bool isTaskloopNoWait(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_taskloop_nowait");
}

// OpenMP 4.0+ Target Offloading
inline bool isTargetInit(const llvm::StringRef &funcName) {
  return funcName.startswith("__tgt_target");
}

inline bool isTargetDataBegin(const llvm::StringRef &funcName) {
  return funcName.equals("__tgt_target_data_begin");
}

inline bool isTargetDataEnd(const llvm::StringRef &funcName) {
  return funcName.equals("__tgt_target_data_end");
}

inline bool isTargetDataUpdate(const llvm::StringRef &funcName) {
  return funcName.equals("__tgt_target_data_update");
}

// OpenMP 5.0+ Task Detach
inline bool isTaskDetach(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_omp_task_complete") ||
         funcName.equals("__kmpc_omp_task_complete_if0");
}

// OpenMP Sections Support
inline bool isSectionsInit(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_sections_init");
}

inline bool isSectionsNext(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_next_section");
}

inline bool isSectionsEnd(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_end_sections");
}

// OpenMP Worksharing Constructs
inline bool isWorkshareStart(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_workshare");
}

inline bool isWorkshareEnd(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_end_workshare");
}

// OpenMP Atomic Operations
inline bool isAtomicStart(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_atomic_start");
}

inline bool isAtomicEnd(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_atomic_end");
}

// OpenMP Flush (memory fence)
inline bool isFlush(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_flush");
}

// OpenMP Cancellation
inline bool isCancel(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_cancel");
}

inline bool isCancellationPoint(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_cancellationpoint");
}

// Check if function is a task-related operation (for aggregation)
inline bool isTaskRelated(const llvm::StringRef &funcName) {
  return isTask(funcName) || isTaskwait(funcName) ||
         isTaskwaitWithDeps(funcName) || isTaskyield(funcName) ||
         isTaskgroupStart(funcName) || isTaskgroupEnd(funcName) ||
         isTaskWithDeps(funcName) || isTaskloop(funcName) ||
         isTaskloopNoWait(funcName) || isTaskDetach(funcName);
}

// Update isNoEffect to include new constructs that don't need modeling
inline bool isNoEffectExtended(const llvm::StringRef &funcName) {
  return isNoEffect(funcName) || funcName.equals("__kmpc_push_proc_bind") ||
         funcName.equals("__kmpc_push_num_teams") ||
         funcName.equals("__kmpc_set_thread_limit");
}

// OpenMP 5.0+ Teams Construct
inline bool isTeams(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_teams");
}

inline bool isTeamsHost(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_teams_host");
}

inline bool isTeamsDistribute(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_teams_distribute");
}

// OpenMP 5.0+ Distribute Construct
inline bool isDistribute(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_distribute");
}

inline bool isDistributeStatic(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_distribute_static");
}

inline bool isDistributeDynamic(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_distribute_dynamic");
}

inline bool isDistributeGuidance(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_distribute_guidance");
}

// OpenMP 5.0+ Loop Construct
inline bool isLoopStaticInit(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_loop_static");
}

inline bool isLoopDynamicInit(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_loop_dynamic");
}

inline bool isLoopGuidanceInit(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_loop_guidance");
}

// OpenMP 5.0+ Affinity Construct
inline bool isAffinity(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_affinity");
}

// OpenMP 5.0+ Scope Construct
inline bool isScopeStart(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_scope");
}

inline bool isScopeEnd(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_end_scope");
}

// OpenMP 5.0+ Taskloop Reduce/Taskloop Simd
inline bool isTaskloopSimd(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_taskloop_simd");
}

inline bool isTaskloopFini(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_taskloop_fini");
}

// OpenMP 5.0+ Cancel constructs
inline bool isCancelBarrier(const llvm::StringRef &funcName) {
  return funcName.equals("__kmpc_cancel_barrier");
}

// OpenMP 5.0+ Interop Construct
inline bool isInteropInit(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_interop");
}

inline bool isInteropFini(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_interop_fini");
}

// OpenMP 5.1+ Doacross support
inline bool isDoacrossInit(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_doacross");
}

inline bool isDoacrossWait(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_doacross_wait");
}

inline bool isDoacrossSubmit(const llvm::StringRef &funcName) {
  return funcName.startswith("__kmpc_doacross_submit");
}

// Check if function is a teams-related operation
inline bool isTeamsRelated(const llvm::StringRef &funcName) {
  return isTeams(funcName) || isTeamsHost(funcName) ||
         isTeamsDistribute(funcName) || isDistribute(funcName);
}

// Check if function is a loop-related operation (worksharing + teams)
inline bool isLoopRelated(const llvm::StringRef &funcName) {
  return isLoopStaticInit(funcName) || isLoopDynamicInit(funcName) ||
         isLoopGuidanceInit(funcName) || isDistribute(funcName);
}

} // namespace OpenMPModel
