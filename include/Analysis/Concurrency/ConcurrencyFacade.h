#pragma once

#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

namespace concurrency {

class ConcurrencyFacade {
public:
  struct OpenMPSummary {
    size_t task_count = 0;
    size_t task_with_dependencies_count = 0;
    size_t included_task_count = 0;
    size_t final_task_count = 0;
    size_t untied_task_count = 0;
    size_t detached_task_count = 0;
    size_t taskloop_count = 0;
    size_t taskyield_count = 0;
    size_t parallel_region_count = 0;
    size_t wait_boundary_count = 0;
    size_t partial_wait_boundary_count = 0;
    size_t barrier_count = 0;
    size_t taskgroup_region_count = 0;
    size_t single_region_count = 0;
    size_t master_region_count = 0;
    size_t ordered_region_count = 0;
    size_t sections_region_count = 0;
    size_t worksharing_loop_count = 0;
    size_t reduction_region_count = 0;
    size_t worksharing_region_count = 0;
    size_t critical_region_count = 0;
    size_t lock_api_count = 0;
    size_t atomic_region_count = 0;
    size_t flush_count = 0;
    size_t cancel_count = 0;
    size_t cancellation_point_count = 0;
    size_t target_region_count = 0;
    size_t target_data_region_count = 0;
    size_t detach_completion_count = 0;
    size_t happens_before_relation_count = 0;
    size_t exclusion_relation_count = 0;
    size_t unknown_relation_count = 0;
    size_t unknown_reason_bucket_count = 0;
    size_t deferred_wait_dep_count = 0;
    size_t deferred_conflict_count = 0;
  };

  struct MPISummary {
    size_t operation_count = 0;
    size_t init_count = 0;
    size_t finalize_count = 0;
    size_t blocking_point_to_point_count = 0;
    size_t nonblocking_operation_count = 0;
    size_t nonblocking_point_to_point_count = 0;
    size_t probe_operation_count = 0;
    size_t wait_operation_count = 0;
    size_t test_operation_count = 0;
    size_t collective_operation_count = 0;
    size_t blocking_collective_count = 0;
    size_t nonblocking_collective_count = 0;
    size_t communicator_management_count = 0;
    size_t request_management_count = 0;
    size_t sendrecv_operation_count = 0;
    size_t persistent_request_init_count = 0;
    size_t request_start_count = 0;
    size_t rma_window_count = 0;
    size_t rma_operation_count = 0;
    size_t rma_sync_count = 0;
    size_t may_complete_request_count = 0;
    size_t terminal_request_count = 0;
    size_t freed_request_count = 0;
    size_t rank_restricted_operation_count = 0;
    size_t wildcard_endpoint_operation_count = 0;
    size_t orphaned_request_count = 0;
    size_t potential_deadlock_count = 0;
    size_t mismatched_collective_count = 0;
    size_t conditional_collective_count = 0;
    size_t collective_partial_reachability_count = 0;
    size_t unsynchronized_rma_count = 0;
    size_t rma_race_count = 0;
    size_t tracked_window_count = 0;
    size_t leaked_window_count = 0;
    size_t collective_slot_count = 0;
    size_t deferred_semantic_lowering_count = 0;
    size_t normalization_exact_count = 0;
    size_t normalization_pmpi_wrapper_count = 0;
    size_t normalization_openmpi_forwarder_count = 0;
    size_t normalization_unknown_internal_count = 0;
  };

  static OpenMPSummary analyzeOpenMP(llvm::Module &module);
  static MPISummary analyzeMPI(llvm::Module &module);
  static void printOpenMPResults(llvm::Module &module, llvm::raw_ostream &os);
};

} // namespace concurrency
