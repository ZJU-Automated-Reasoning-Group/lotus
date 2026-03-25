#pragma once

#include "Analysis/Concurrency/ConcurrencyRelation.h"
#include "Analysis/Concurrency/OpenMP/DataSharingAnalysis.h"

#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace OpenMP {

enum class DependType {
  IN,
  OUT,
  INOUT,
  MUTEXINOUTSET
};

enum class DependencySourceKind {
  DirectAddress,
  RegionSummary,
  DepObj,
  Iterator,
  Unknown
};

enum class DependencyProof { Definite, Possible, Unknown };

enum class TaskExecutionMode { Deferred, Included, Final, Detached, Untied };

enum class DependencyConflict {
  NoConflict,
  MustConflict,
  MayConflict,
  Unknown
};

struct Dependency {
  DependType type;
  const llvm::Value *address;
  size_t size;
  const llvm::Value *canonical_base = nullptr;
  int64_t offset = 0;
  bool has_precise_offset = false;
  DependencySourceKind source_kind = DependencySourceKind::Unknown;
  DependencyProof proof = DependencyProof::Unknown;
};

struct Task {
  const llvm::Instruction *task_create;
  const llvm::Function *task_function;
  const llvm::Value *task_handle = nullptr;
  TaskExecutionMode execution_mode = TaskExecutionMode::Deferred;
  bool is_final = false;
  bool is_untied = false;
  bool is_detached = false;
  const llvm::Function *parent_context = nullptr;
  const llvm::Instruction *generating_context = nullptr;
  size_t scheduling_context_id = 0;
  size_t taskgroup_id = 0;
  size_t phase_id = 0;
  size_t sibling_group = 0;
  size_t sequence_index = 0;
  size_t event_order = 0;
  size_t region_id = 0;
  size_t semantic_entity_id = 0;
  std::vector<Dependency> dependencies;
  std::set<Task *> predecessors;
  std::set<Task *> successors;
  std::set<Task *> exclusions;
  std::set<const llvm::Value *> synchronization_objects;
  std::vector<DataSharingEntry> data_sharing_entries;
};

struct WaitBoundaryInfo {
  enum class Kind {
    Taskwait,
    TaskwaitDeps,
    TaskgroupEnd,
    SingleEnd,
    MasterEnd,
    OrderedEnd,
    SectionsEnd,
    ForFini,
    DispatchFini,
    Reduce,
    Flush,
    Barrier,
    CriticalEnd,
    DoacrossWait,
    Target,
    TargetNowait,
    TargetData,
    TargetDataNowait,
    ReduceNowait,
    Unknown
  };

  const llvm::Instruction *inst = nullptr;
  size_t scheduling_context_id = 0;
  size_t sequence_index = 0;
  size_t event_order = 0;
  size_t phase_id = 0;
  size_t taskgroup_id = 0;
  size_t region_id = 0;
  size_t semantic_entity_id = 0;
  bool is_taskgroup_end = false;
  bool is_partial_wait = false;
  Kind kind = Kind::Unknown;
};

struct WaitBoundaryRecord {
  const llvm::Instruction *inst = nullptr;
  size_t scheduling_context_id = 0;
  size_t sequence_index = 0;
  size_t event_order = 0;
  size_t sibling_group = 0;
  size_t taskgroup_id = 0;
  size_t semantic_entity_id = 0;
  bool is_taskgroup_end = false;
  bool is_partial_wait = false;
  WaitBoundaryInfo::Kind kind = WaitBoundaryInfo::Kind::Unknown;
  std::vector<Dependency> dependencies;
};

enum class SemanticEntityKind {
  SchedulingContext,
  ParallelRegion,
  ExplicitTask,
  Taskgroup,
  SingleRegion,
  MasterRegion,
  OrderedRegion,
  CriticalRegion,
  WorksharingLoop,
  SectionsRegion,
  ReductionRegion,
  TargetRegion,
  TargetDataRegion,
  TeamsRegion,
  WaitBoundary,
  Unknown
};

enum class SemanticEventKind {
  RegionBegin,
  RegionEnd,
  TaskCreate,
  Boundary,
  Barrier,
  Flush,
  TargetLaunch,
  Unknown
};

struct SemanticEntity {
  size_t id = 0;
  SemanticEntityKind kind = SemanticEntityKind::Unknown;
  const llvm::Instruction *anchor_inst = nullptr;
  const llvm::Function *function = nullptr;
  size_t scheduling_context_id = 0;
  size_t parent_id = 0;
  size_t region_id = 0;
  size_t phase_id = 0;
  size_t taskgroup_id = 0;
  std::vector<DataSharingEntry> data_sharing_entries;
};

struct SemanticEvent {
  size_t entity_id = 0;
  SemanticEventKind kind = SemanticEventKind::Unknown;
  const llvm::Instruction *inst = nullptr;
  size_t scheduling_context_id = 0;
  size_t sequence_index = 0;
  size_t event_order = 0;
  size_t region_id = 0;
  size_t phase_id = 0;
  bool is_partial = false;
};

struct OpenMPTaskEvent {
  enum class Kind {
    TaskCreate,
    TaskComplete,
    Taskwait,
    TaskwaitDeps,
    TaskgroupBegin,
    TaskgroupEnd,
    Barrier,
    Flush,
    DoacrossWait,
    DoacrossSubmit,
    TargetBoundary,
    ReductionNowaitBoundary
  };

  Kind kind = Kind::TaskCreate;
  const llvm::Instruction *inst = nullptr;
  const Task *task = nullptr;
  size_t scheduling_context_id = 0;
  size_t sequence_index = 0;
  size_t event_order = 0;
  size_t phase_id = 0;
  size_t taskgroup_id = 0;
  size_t region_id = 0;
  size_t semantic_entity_id = 0;
  bool is_partial_wait = false;
  bool is_taskgroup_end = false;
  WaitBoundaryInfo::Kind boundary_kind = WaitBoundaryInfo::Kind::Unknown;
  std::vector<Dependency> dependencies;
};

class OpenMPSemantics {
public:
  struct AnalysisSummary {
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
    size_t critical_region_count = 0;
    size_t lock_api_count = 0;
    size_t atomic_region_count = 0;
    size_t flush_count = 0;
    size_t cancel_count = 0;
    size_t cancellation_point_count = 0;
    size_t target_region_count = 0;
    size_t target_data_region_count = 0;
    size_t target_nowait_boundary_count = 0;
    size_t doacross_init_count = 0;
    size_t doacross_wait_count = 0;
    size_t doacross_submit_count = 0;
    size_t reduction_nowait_boundary_count = 0;
    size_t detach_completion_count = 0;
    size_t teams_region_count = 0;
    size_t distribute_region_count = 0;
    size_t loop_region_count = 0;
    size_t affinity_region_count = 0;
    size_t scope_region_count = 0;
    size_t nested_parallelism_max_depth = 0;
    size_t nested_parallelism_flat_regions = 0;
    size_t nested_parallelism_nested_regions = 0;
    size_t semantic_entity_count = 0;
    size_t semantic_event_count = 0;
  };

  explicit OpenMPSemantics(llvm::Module &module);

  void analyze();

  const std::vector<std::unique_ptr<Task>> &getTasks() const { return m_tasks; }
  const Task *getTaskForCreate(const llvm::Instruction *inst) const;
  const Task *getTaskForHandle(const llvm::Value *value) const;
  const std::vector<WaitBoundaryInfo> &getWaitBoundaryInfos() const {
    return m_wait_boundary_infos;
  }
  const std::unordered_map<size_t, std::vector<WaitBoundaryRecord>> &
  getWaitBoundaryRecords() const {
    return m_wait_boundaries;
  }
  const std::vector<SemanticEntity> &getSemanticEntities() const {
    return m_entities;
  }
  const std::vector<SemanticEvent> &getSemanticEvents() const {
    return m_events;
  }
  const std::vector<OpenMPTaskEvent> &getTaskEvents() const {
    return m_task_events;
  }
  const std::map<std::pair<const Task *, const Task *>, concurrency::Relation> &
  getRelations() const {
    return m_relations;
  }
  const std::unordered_map<std::string, size_t> &getDeferredReasonCounts() const {
    return m_deferred_reason_counts;
  }
  DependencyConflict
  classifyDependencyConflictForTesting(const Dependency &d1,
                                       const Dependency &d2) const {
    return classifyDependencyConflict(d1, d2);
  }
  size_t getDeferredImpreciseConflictCount() const {
    return m_deferred_imprecise_conflict_count;
  }
  const AnalysisSummary &getSummary() const { return m_summary; }
  size_t getMaxNestedDepth() const { return m_nested_depth; }
  bool isNestedRegion(size_t region_id) const;
  size_t getRegionNestingDepth(size_t region_id) const;
  const std::vector<DataSharingEntry> &
  getDataSharingEntriesForEntity(size_t entity_id) const;

private:
  friend class OpenMPTaskGraph;

  struct TraversalState {
    struct RegionFrame {
      size_t id = 0;
      WaitBoundaryInfo::Kind kind = WaitBoundaryInfo::Kind::Unknown;
      size_t entity_id = 0;
    };

    size_t scheduling_context_id = 0;
    size_t scheduling_context_entity_id = 0;
    size_t next_taskgroup_id = 1;
    size_t next_phase_token = 1;
    size_t next_region_id = 1;
    size_t sequence_index = 0;
    size_t next_event_order = 1;
    const llvm::Instruction *anchor_inst = nullptr;
    std::vector<size_t> taskgroup_stack;
    std::vector<size_t> phase_stack;
    std::vector<RegionFrame> region_stack;
  };

  llvm::Module &m_module;
  std::vector<std::unique_ptr<Task>> m_tasks;
  std::map<const llvm::Instruction *, Task *> m_inst_to_task;
  std::unordered_map<size_t, std::vector<WaitBoundaryRecord>> m_wait_boundaries;
  std::vector<WaitBoundaryInfo> m_wait_boundary_infos;
  std::vector<SemanticEntity> m_entities;
  std::vector<SemanticEvent> m_events;
  std::vector<OpenMPTaskEvent> m_task_events;
  std::map<std::pair<const Task *, const Task *>, concurrency::Relation>
      m_relations;
  AnalysisSummary m_summary;
  size_t m_nested_depth = 0;
  size_t m_next_scheduling_context_id = 1;
  size_t m_next_entity_id = 1;
  std::unordered_map<size_t, size_t> m_region_nesting_depth;
  mutable size_t m_deferred_imprecise_conflict_count = 0;
  mutable std::unordered_map<std::string, size_t> m_deferred_reason_counts;

  void identifySemanticStructure();
  void attachDataSharingFacts();
  void buildTaskRelations();
  void rebuildWaitBoundaryViews();
  void scanSchedulingContext(const llvm::Function *func, TraversalState &state,
                             std::set<const llvm::Function *> &call_stack);
  std::vector<Dependency> extractDependencies(const llvm::CallBase *task_call);
  std::vector<Dependency> extractRuntimeDependencies(const llvm::CallBase *call,
                                                     unsigned ndeps_arg_idx,
                                                     unsigned dep_arg_idx);
  const llvm::CallBase *findTaskAllocCall(const llvm::Value *task_value) const;
  const llvm::Value *canonicalizeTaskHandle(const llvm::Value *task_value) const;
  const llvm::Function *extractTaskFunction(const llvm::CallBase *task_call);
  void applyTaskExecutionHints(Task &task, const llvm::CallBase *task_call);
  size_t addEntity(SemanticEntityKind kind, const llvm::Instruction *anchor_inst,
                   const llvm::Function *func, size_t scheduling_context_id,
                   size_t parent_id, size_t region_id, size_t phase_id,
                   size_t taskgroup_id);
  void addEvent(SemanticEventKind kind, const llvm::Instruction *inst,
                size_t entity_id, size_t scheduling_context_id,
                size_t sequence_index, size_t event_order, size_t region_id,
                size_t phase_id,
                bool is_partial = false);
  void addEvent(SemanticEventKind kind, const llvm::Instruction *inst,
                size_t entity_id, size_t scheduling_context_id,
                size_t sequence_index, size_t region_id, size_t phase_id,
                bool is_partial = false) {
    addEvent(kind, inst, entity_id, scheduling_context_id, sequence_index,
             sequence_index, region_id, phase_id, is_partial);
  }
  void addTaskEvent(OpenMPTaskEvent::Kind kind, const llvm::Instruction *inst,
                    size_t scheduling_context_id, size_t sequence_index,
                    size_t event_order,
                    size_t phase_id, size_t taskgroup_id, size_t region_id,
                    size_t semantic_entity_id, const Task *task = nullptr,
                    WaitBoundaryInfo::Kind boundary_kind =
                        WaitBoundaryInfo::Kind::Unknown,
                    bool is_partial_wait = false,
                    bool is_taskgroup_end = false,
                    std::vector<Dependency> dependencies = {});
  void addTaskEvent(OpenMPTaskEvent::Kind kind, const llvm::Instruction *inst,
                    size_t scheduling_context_id, size_t sequence_index,
                    size_t phase_id, size_t taskgroup_id, size_t region_id,
                    size_t semantic_entity_id, const Task *task = nullptr,
                    WaitBoundaryInfo::Kind boundary_kind =
                        WaitBoundaryInfo::Kind::Unknown,
                    bool is_partial_wait = false,
                    bool is_taskgroup_end = false,
                    std::vector<Dependency> dependencies = {}) {
    addTaskEvent(kind, inst, scheduling_context_id, sequence_index,
                 sequence_index, phase_id, taskgroup_id, region_id,
                 semantic_entity_id, task, boundary_kind, is_partial_wait,
                 is_taskgroup_end, std::move(dependencies));
  }
  DependencyConflict classifyDependencyConflict(const Dependency &d1,
                                               const Dependency &d2) const;
  bool dependenciesConflict(const Dependency &d1, const Dependency &d2) const;
  bool isMutexLikeExclusion(const Dependency &d1, const Dependency &d2) const;
};

} // namespace OpenMP
