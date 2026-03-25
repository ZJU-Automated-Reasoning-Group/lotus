# OpenMP API metadata.
# Format: SymbolOrPrefix TD_TYPE library=openmp semantic=<tag> [traits=<...>] [match=exact|prefix]

# OpenMP exact matches
__kmpc_fork_call TD_FORK library=openmp semantic=fork
__kmpc_barrier TD_BAR_WAIT library=openmp semantic=barrier
__kmpc_critical TD_ACQUIRE library=openmp semantic=critical-enter
__kmpc_end_critical TD_RELEASE library=openmp semantic=critical-exit
omp_set_lock TD_ACQUIRE library=openmp semantic=lock
omp_unset_lock TD_RELEASE library=openmp semantic=unlock
omp_test_lock TD_TRY_ACQUIRE library=openmp semantic=try-lock
omp_init_lock TD_MUTEX_INI library=openmp semantic=init-lock
omp_destroy_lock TD_MUTEX_DESTROY library=openmp semantic=destroy-lock
omp_set_nest_lock TD_ACQUIRE library=openmp semantic=nest-lock
omp_unset_nest_lock TD_RELEASE library=openmp semantic=nest-unlock
omp_test_nest_lock TD_TRY_ACQUIRE library=openmp semantic=nest-try-lock
omp_init_nest_lock TD_MUTEX_INI library=openmp semantic=init-nest-lock
omp_destroy_nest_lock TD_MUTEX_DESTROY library=openmp semantic=destroy-nest-lock
__kmpc_omp_task TD_OMP_TASK library=openmp semantic=task traits=omp-task-op
__kmpc_omp_task_begin_if0 TD_OMP_TASK library=openmp semantic=task-inline traits=omp-task-op
__kmpc_omp_taskwait TD_OMP_TASKWAIT library=openmp semantic=taskwait traits=omp-task-op
__kmpc_omp_wait_deps TD_OMP_TASKWAIT_DEPS library=openmp semantic=taskwait-deps traits=omp-task-op
__kmpc_omp_taskwait_deps_51 TD_OMP_TASKWAIT_DEPS library=openmp semantic=taskwait-deps traits=omp-task-op
__kmpc_omp_taskyield TD_OMP_TASKYIELD library=openmp semantic=taskyield traits=omp-task-op
__kmpc_taskgroup TD_OMP_TASKGROUP_START library=openmp semantic=taskgroup-start traits=omp-task-op
__kmpc_end_taskgroup TD_OMP_TASKGROUP_END library=openmp semantic=taskgroup-end traits=omp-task-op
__kmpc_taskloop TD_OMP_TASKLOOP library=openmp semantic=taskloop traits=omp-task-op
__kmpc_taskloop_nowait TD_OMP_TASKLOOP library=openmp semantic=taskloop-nowait traits=omp-task-op
__kmpc_omp_task_complete TD_OMP_TASK_COMPLETE library=openmp semantic=task-complete traits=omp-task-op
__kmpc_omp_task_complete_if0 TD_OMP_TASK_COMPLETE library=openmp semantic=task-complete-inline traits=omp-task-op
__kmpc_critical_with_hint TD_ACQUIRE library=openmp semantic=critical-enter
__kmpc_single TD_OMP_SINGLE_START library=openmp semantic=single-start traits=omp-task-op
__kmpc_end_single TD_OMP_SINGLE_END library=openmp semantic=single-end traits=omp-task-op,barrier-wait-like
__kmpc_master TD_OMP_MASTER_START library=openmp semantic=master-start traits=omp-task-op
__kmpc_end_master TD_OMP_MASTER_END library=openmp semantic=master-end traits=omp-task-op
__kmpc_ordered TD_OMP_ORDERED_START library=openmp semantic=ordered-start traits=omp-task-op
__kmpc_end_ordered TD_OMP_ORDERED_END library=openmp semantic=ordered-end traits=omp-task-op
__kmpc_reduce TD_OMP_REDUCE_START library=openmp semantic=reduce-start traits=omp-task-op,barrier-wait-like
__kmpc_end_reduce TD_OMP_REDUCE_END library=openmp semantic=reduce-end traits=omp-task-op
__kmpc_reduce_nowait TD_OMP_REDUCE_NOWAIT_START library=openmp semantic=reduce-nowait-start traits=omp-task-op
__kmpc_end_reduce_nowait TD_OMP_REDUCE_NOWAIT_END library=openmp semantic=reduce-nowait-end traits=omp-task-op
__kmpc_for_static_fini TD_OMP_FOR_STATIC_FINI library=openmp semantic=for-static-fini traits=omp-task-op,barrier-wait-like
__kmpc_sections_init TD_OMP_SECTIONS_INIT library=openmp semantic=sections-init traits=omp-task-op
__kmpc_next_section TD_OMP_SECTIONS_NEXT library=openmp semantic=sections-next traits=omp-task-op
__kmpc_end_sections TD_OMP_SECTIONS_END library=openmp semantic=sections-end traits=omp-task-op,barrier-wait-like
__kmpc_atomic_start TD_OMP_ATOMIC_START library=openmp semantic=atomic-start
__kmpc_atomic_end TD_OMP_ATOMIC_END library=openmp semantic=atomic-end
__kmpc_flush TD_OMP_FLUSH library=openmp semantic=flush
__kmpc_cancel TD_OMP_CANCEL library=openmp semantic=cancel
__kmpc_cancellationpoint TD_OMP_CANCEL library=openmp semantic=cancellation-point
__tgt_target_data_begin TD_OMP_TARGET_DATA_BEGIN library=openmp semantic=target-data-begin traits=omp-target-op,omp-target-data-op
__tgt_target_data_end TD_OMP_TARGET_DATA_END library=openmp semantic=target-data-end traits=omp-target-op,omp-target-data-op
__tgt_target_data_update TD_OMP_TARGET_DATA_UPDATE library=openmp semantic=target-data-update traits=omp-target-op,omp-target-data-op
__tgt_target_data_begin_nowait TD_OMP_TARGET_DATA_BEGIN library=openmp semantic=target-data-begin-nowait traits=omp-target-op,omp-target-data-op
__tgt_target_data_end_nowait TD_OMP_TARGET_DATA_END library=openmp semantic=target-data-end-nowait traits=omp-target-op,omp-target-data-op
__tgt_target_data_update_nowait TD_OMP_TARGET_DATA_UPDATE library=openmp semantic=target-data-update-nowait traits=omp-target-op,omp-target-data-op
__tgt_target_enter_data TD_OMP_TARGET_DATA_BEGIN library=openmp semantic=target-enter-data traits=omp-target-op,omp-target-data-op
__tgt_target_exit_data TD_OMP_TARGET_DATA_END library=openmp semantic=target-exit-data traits=omp-target-op,omp-target-data-op
__tgt_target_update TD_OMP_TARGET_DATA_UPDATE library=openmp semantic=target-update traits=omp-target-op,omp-target-data-op
GOMP_barrier TD_BAR_WAIT library=openmp semantic=barrier traits=omp-task-op,barrier-wait-like
GOMP_parallel TD_FORK library=openmp semantic=fork traits=parallel-explicit-end
GOMP_parallel_start TD_FORK library=openmp semantic=fork traits=parallel-explicit-end
GOMP_parallel_end TD_BAR_WAIT library=openmp semantic=parallel-end-barrier traits=omp-task-op,barrier-wait-like,parallel-end
GOMP_taskwait TD_OMP_TASKWAIT library=openmp semantic=taskwait traits=omp-task-op
GOMP_taskgroup_start TD_OMP_TASKGROUP_START library=openmp semantic=taskgroup-start traits=omp-task-op
GOMP_taskgroup_end TD_OMP_TASKGROUP_END library=openmp semantic=taskgroup-end traits=omp-task-op
GOMP_task TD_OMP_TASK library=openmp semantic=task traits=omp-task-op
GOMP_taskyield TD_OMP_TASKYIELD library=openmp semantic=taskyield traits=omp-task-op
GOMP_taskloop TD_OMP_TASKLOOP library=openmp semantic=taskloop traits=omp-task-op match=prefix

# OpenMP prefix matches
__kmpc_omp_task_with_deps TD_OMP_TASK_WITH_DEPS library=openmp semantic=task-with-deps traits=omp-task-op match=prefix
__kmpc_for_static_init TD_OMP_FOR_STATIC_INIT library=openmp semantic=for-static-init traits=omp-task-op match=prefix
__kmpc_dispatch_init TD_OMP_FOR_DISPATCH_INIT library=openmp semantic=dispatch-init traits=omp-task-op match=prefix
__kmpc_dispatch_next TD_OMP_FOR_DISPATCH_NEXT library=openmp semantic=dispatch-next traits=omp-task-op match=prefix
__kmpc_dispatch_fini TD_OMP_FOR_DISPATCH_FINI library=openmp semantic=dispatch-fini traits=omp-task-op,barrier-wait-like match=prefix
__tgt_target TD_OMP_TARGET library=openmp semantic=target traits=omp-target-op match=prefix

# OpenMP 5.0+ Teams and Distribute prefix matches
__kmpc_teams_host TD_OMP_TEAMS_HOST library=openmp semantic=teams-host match=prefix
__kmpc_teams_distribute TD_OMP_TEAMS_DISTRIBUTE library=openmp semantic=teams-distribute match=prefix
__kmpc_teams TD_OMP_TEAMS library=openmp semantic=teams match=prefix
__kmpc_distribute_static TD_OMP_DISTRIBUTE_STATIC library=openmp semantic=distribute-static match=prefix
__kmpc_distribute_dynamic TD_OMP_DISTRIBUTE_DYNAMIC library=openmp semantic=distribute-dynamic match=prefix
__kmpc_distribute_guidance TD_OMP_DISTRIBUTE_GUIDANCE library=openmp semantic=distribute-guidance match=prefix
__kmpc_distribute TD_OMP_DISTRIBUTE library=openmp semantic=distribute match=prefix
__kmpc_loop_static TD_OMP_LOOP_STATIC_INIT library=openmp semantic=loop-static-init match=prefix
__kmpc_loop_dynamic TD_OMP_LOOP_DYNAMIC_INIT library=openmp semantic=loop-dynamic-init match=prefix
__kmpc_loop_guidance TD_OMP_LOOP_GUIDANCE_INIT library=openmp semantic=loop-guidance-init match=prefix
__kmpc_loop TD_OMP_LOOP_STATIC_INIT library=openmp semantic=loop-static-init match=prefix
__kmpc_affinity TD_OMP_AFFINITY library=openmp semantic=affinity match=prefix
__kmpc_scope TD_OMP_SCOPE_START library=openmp semantic=scope-start match=prefix
__kmpc_end_scope TD_OMP_SCOPE_END library=openmp semantic=scope-end match=prefix
__kmpc_taskloop_simd TD_OMP_TASKLOOP_SIMD library=openmp semantic=taskloop-simd match=prefix
__kmpc_taskloop_fini TD_OMP_TASKLOOP_FINI library=openmp semantic=taskloop-fini match=prefix
__kmpc_interop TD_OMP_INTEROP_INIT library=openmp semantic=interop-init match=prefix
__kmpc_interop_fini TD_OMP_INTEROP_FINI library=openmp semantic=interop-fini match=prefix
__kmpc_doacross_wait TD_OMP_DOACROSS_WAIT library=openmp semantic=doacross-wait match=prefix
__kmpc_doacross_submit TD_OMP_DOACROSS_SUBMIT library=openmp semantic=doacross-submit match=prefix
__kmpc_doacross TD_OMP_DOACROSS_INIT library=openmp semantic=doacross-init match=prefix
