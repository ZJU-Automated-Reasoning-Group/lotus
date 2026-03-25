#include "Analysis/Concurrency/OpenMP/OpenMPSemantics.h"

#include "Analysis/Concurrency/OpenMP/OpenMPModel.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <deque>

#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

namespace OpenMP {

namespace {

DependType decodeDependType(uint64_t flags) {
  if ((flags & 0x4ULL) != 0) {
    return DependType::MUTEXINOUTSET;
  }
  switch (flags & 0x3ULL) {
  case 0x1ULL:
    return DependType::IN;
  case 0x2ULL:
    return DependType::OUT;
  default:
    return DependType::INOUT;
  }
}

const Value *stripValue(const Value *value) {
  return value ? value->stripPointerCasts() : nullptr;
}

std::pair<DependencySourceKind, DependencyProof>
classifyDependencyAddressEvidence(const Value *value) {
  if (!value) {
    return {DependencySourceKind::RegionSummary, DependencyProof::Unknown};
  }

  value = value->stripPointerCasts();
  if (isa<GlobalValue>(value) || isa<AllocaInst>(value)) {
    return {DependencySourceKind::DirectAddress, DependencyProof::Definite};
  }

  if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
    if (ce->isCast() || ce->getOpcode() == Instruction::GetElementPtr) {
      const Value *base =
          getUnderlyingObject(ce->getOperand(0)->stripPointerCasts());
      if (isa<GlobalValue>(base) || isa<AllocaInst>(base)) {
        return {DependencySourceKind::DirectAddress, DependencyProof::Definite};
      }
    }
  }

  if (const auto *arg = dyn_cast<Argument>(value)) {
    return {DependencySourceKind::Iterator, arg->getType()->isPointerTy()
                                                ? DependencyProof::Possible
                                                : DependencyProof::Unknown};
  }

  return {DependencySourceKind::RegionSummary, DependencyProof::Possible};
}

const Value *canonicalizeDependencyAddress(const Value *value,
                                           const DataLayout &DL,
                                           int64_t &offset,
                                           bool &has_precise_offset) {
  offset = 0;
  has_precise_offset = false;
  if (!value) {
    return nullptr;
  }

  value = stripValue(value);
  if (const auto *load = dyn_cast<LoadInst>(value)) {
    value = load->getPointerOperand()->stripPointerCasts();
  }

  if (const Value *base_with_offset =
          GetPointerBaseWithConstantOffset(value, offset, DL)) {
    const Value *canonical_base = stripValue(base_with_offset);
    if (!(isa<GEPOperator>(value) && canonical_base == stripValue(value))) {
      has_precise_offset = true;
      return canonical_base;
    }
  }

  if (const Value *base = getUnderlyingObject(value)) {
    return stripValue(base);
  }
  return value;
}

bool decodeConstantDependency(const Constant *elt, Dependency &dep) {
  if (!elt) {
    return false;
  }
  const auto *cs = dyn_cast<ConstantStruct>(elt);
  if (!cs || cs->getNumOperands() < 3) {
    return false;
  }

  dep.address = stripValue(cs->getOperand(0));
  dep.size = 0;
  dep.type = DependType::INOUT;

  if (const auto *len = dyn_cast<ConstantInt>(cs->getOperand(1))) {
    dep.size = len->getZExtValue();
  }
  if (const auto *flags = dyn_cast<ConstantInt>(cs->getOperand(2))) {
    dep.type = decodeDependType(flags->getZExtValue());
  }
  return dep.address != nullptr;
}

const Value *resolveDependencyListValue(const Value *value) {
  if (!value) {
    return nullptr;
  }
  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  const Value *resolved = nullptr;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    current = current->stripPointerCasts();
    if (const auto *load = dyn_cast<LoadInst>(current)) {
      worklist.push_back(load->getPointerOperand());
      continue;
    }
    if (const auto *store = dyn_cast<StoreInst>(current)) {
      worklist.push_back(store->getValueOperand());
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
    if (const auto *gep = dyn_cast<GEPOperator>(current)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }

    if (isa<AllocaInst>(current) || isa<GlobalVariable>(current) ||
        isa<Argument>(current)) {
      bool saw_stored_pointer = false;
      for (const Use &use : current->uses()) {
        if (const auto *store = dyn_cast<StoreInst>(use.getUser())) {
          if (store->getPointerOperand()->stripPointerCasts() == current) {
            worklist.push_back(store->getValueOperand());
            saw_stored_pointer = true;
          }
        }
      }
      if (saw_stored_pointer) {
        continue;
      }
    }

    const Value *underlying = getUnderlyingObject(current);
    if (underlying) {
      current = underlying->stripPointerCasts();
    }
    if (!resolved) {
      resolved = current;
    } else if (resolved != current) {
      return nullptr;
    }
  }

  return resolved ? resolved : value->stripPointerCasts();
}

bool isBeforeInBlock(const Instruction *lhs, const Instruction *rhs) {
  if (!lhs || !rhs || lhs->getParent() != rhs->getParent()) {
    return false;
  }
  for (const Instruction &inst : *lhs->getParent()) {
    if (&inst == lhs) {
      return true;
    }
    if (&inst == rhs) {
      return false;
    }
  }
  return false;
}

bool mustHappenBefore(const Instruction *lhs, const Instruction *rhs) {
  if (!lhs || !rhs || lhs == rhs || lhs->getFunction() != rhs->getFunction()) {
    return false;
  }
  if (lhs->getParent() == rhs->getParent()) {
    return isBeforeInBlock(lhs, rhs);
  }

  const Function *func = lhs->getFunction();
  if (!func || func->isDeclaration()) {
    return false;
  }

  DominatorTree DT(*const_cast<Function *>(func));
  if (!DT.dominates(lhs, rhs)) {
    return false;
  }

  LoopInfo LI(DT);
  return !isPotentiallyReachable(rhs, lhs, nullptr, &DT, &LI);
}

std::pair<const Task *, const Task *> normalizeTaskPair(const Task *lhs,
                                                        const Task *rhs) {
  return lhs < rhs ? std::make_pair(lhs, rhs) : std::make_pair(rhs, lhs);
}

const Instruction *taskOrderingSite(const Task *task) {
  if (!task) {
    return nullptr;
  }
  return task->generating_context ? task->generating_context
                                  : task->task_create;
}

constexpr uint64_t kLibompTaskTiednessMask = 0x1ULL;
constexpr uint64_t kLibompTaskFinalMask = 0x2ULL;
constexpr uint64_t kLibompTaskMergedIf0Mask = 0x4ULL;
constexpr uint64_t kLibompTaskProxyMask = 0x10ULL;
constexpr uint64_t kLibompTaskDetachableMask = 0x40ULL;

} // namespace

OpenMPSemantics::OpenMPSemantics(Module &module) : m_module(module) {}

void OpenMPSemantics::analyze() {
  m_tasks.clear();
  m_inst_to_task.clear();
  m_wait_boundaries.clear();
  m_wait_boundary_infos.clear();
  m_entities.clear();
  m_events.clear();
  m_task_events.clear();
  m_relations.clear();
  m_summary = AnalysisSummary{};
  m_nested_depth = 0;
  m_next_scheduling_context_id = 1;
  m_next_entity_id = 1;
  m_region_nesting_depth.clear();
  m_deferred_imprecise_conflict_count = 0;
  m_deferred_reason_counts.clear();
  identifySemanticStructure();
  rebuildWaitBoundaryViews();
  attachDataSharingFacts();
  buildTaskRelations();
  m_summary.task_count = m_tasks.size();
  m_summary.wait_boundary_count = 0;
  m_summary.partial_wait_boundary_count = 0;
  for (const WaitBoundaryInfo &info : m_wait_boundary_infos) {
    if (info.kind != WaitBoundaryInfo::Kind::Flush) {
      ++m_summary.wait_boundary_count;
    }
    if (info.is_partial_wait) {
      if (info.kind != WaitBoundaryInfo::Kind::Flush) {
        ++m_summary.partial_wait_boundary_count;
      }
    }
  }
  m_summary.semantic_entity_count = m_entities.size();
  m_summary.semantic_event_count = m_events.size();
}

const Task *
OpenMPSemantics::getTaskForCreate(const Instruction *inst) const {
  auto it = m_inst_to_task.find(inst);
  return it != m_inst_to_task.end() ? it->second : nullptr;
}

const Task *OpenMPSemantics::getTaskForHandle(const Value *value) const {
  const Value *handle = canonicalizeTaskHandle(value);
  if (!handle) {
    return nullptr;
  }
  for (const auto &task_uptr : m_tasks) {
    const Task *task = task_uptr.get();
    if (task && task->task_handle == handle) {
      return task;
    }
  }
  return nullptr;
}

bool OpenMPSemantics::isNestedRegion(size_t region_id) const {
  auto it = m_region_nesting_depth.find(region_id);
  if (it == m_region_nesting_depth.end()) {
    return false;
  }
  return it->second > 1;
}

size_t OpenMPSemantics::getRegionNestingDepth(size_t region_id) const {
  auto it = m_region_nesting_depth.find(region_id);
  if (it == m_region_nesting_depth.end()) {
    return 0;
  }
  return it->second;
}

const std::vector<DataSharingEntry> &
OpenMPSemantics::getDataSharingEntriesForEntity(size_t entity_id) const {
  static const std::vector<DataSharingEntry> kEmpty;
  for (const SemanticEntity &entity : m_entities) {
    if (entity.id == entity_id) {
      return entity.data_sharing_entries;
    }
  }
  return kEmpty;
}

size_t OpenMPSemantics::addEntity(SemanticEntityKind kind,
                                  const Instruction *anchor_inst,
                                  const Function *func,
                                  size_t scheduling_context_id,
                                  size_t parent_id, size_t region_id,
                                  size_t phase_id, size_t taskgroup_id) {
  SemanticEntity entity;
  entity.id = m_next_entity_id++;
  entity.kind = kind;
  entity.anchor_inst = anchor_inst;
  entity.function = func;
  entity.scheduling_context_id = scheduling_context_id;
  entity.parent_id = parent_id;
  entity.region_id = region_id;
  entity.phase_id = phase_id;
  entity.taskgroup_id = taskgroup_id;
  m_entities.push_back(entity);
  return entity.id;
}

void OpenMPSemantics::addEvent(SemanticEventKind kind, const Instruction *inst,
                               size_t entity_id,
                               size_t scheduling_context_id,
                               size_t sequence_index, size_t event_order,
                               size_t region_id, size_t phase_id,
                               bool is_partial) {
  SemanticEvent event;
  event.entity_id = entity_id;
  event.kind = kind;
  event.inst = inst;
  event.scheduling_context_id = scheduling_context_id;
  event.sequence_index = sequence_index;
  event.event_order = event_order;
  event.region_id = region_id;
  event.phase_id = phase_id;
  event.is_partial = is_partial;
  m_events.push_back(event);
}

void OpenMPSemantics::addTaskEvent(OpenMPTaskEvent::Kind kind,
                                   const Instruction *inst,
                                   size_t scheduling_context_id,
                                   size_t sequence_index, size_t event_order,
                                   size_t phase_id,
                                   size_t taskgroup_id, size_t region_id,
                                   size_t semantic_entity_id,
                                   const Task *task,
                                   WaitBoundaryInfo::Kind boundary_kind,
                                   bool is_partial_wait, bool is_taskgroup_end,
                                   std::vector<Dependency> dependencies) {
  OpenMPTaskEvent event;
  event.kind = kind;
  event.inst = inst;
  event.task = task;
  event.scheduling_context_id = scheduling_context_id;
  event.sequence_index = sequence_index;
  event.event_order = event_order;
  event.phase_id = phase_id;
  event.taskgroup_id = taskgroup_id;
  event.region_id = region_id;
  event.semantic_entity_id = semantic_entity_id;
  event.is_partial_wait = is_partial_wait;
  event.is_taskgroup_end = is_taskgroup_end;
  event.boundary_kind = boundary_kind;
  event.dependencies = std::move(dependencies);
  m_task_events.push_back(std::move(event));
}

void OpenMPSemantics::rebuildWaitBoundaryViews() {
  m_wait_boundaries.clear();
  m_wait_boundary_infos.clear();

  for (const OpenMPTaskEvent &event : m_task_events) {
    switch (event.kind) {
    case OpenMPTaskEvent::Kind::TaskComplete:
    case OpenMPTaskEvent::Kind::Taskwait:
    case OpenMPTaskEvent::Kind::TaskwaitDeps:
    case OpenMPTaskEvent::Kind::TaskgroupEnd:
    case OpenMPTaskEvent::Kind::Barrier:
    case OpenMPTaskEvent::Kind::Flush:
    case OpenMPTaskEvent::Kind::DoacrossWait:
    case OpenMPTaskEvent::Kind::TargetBoundary:
    case OpenMPTaskEvent::Kind::ReductionNowaitBoundary:
      break;
    default:
      continue;
    }

    WaitBoundaryInfo info;
    info.inst = event.inst;
    info.scheduling_context_id = event.scheduling_context_id;
    info.sequence_index = event.sequence_index;
    info.event_order = event.event_order;
    info.phase_id = event.phase_id;
    info.taskgroup_id = event.taskgroup_id;
    info.region_id = event.region_id;
    info.semantic_entity_id = event.semantic_entity_id;
    info.is_taskgroup_end = event.is_taskgroup_end;
    info.is_partial_wait = event.is_partial_wait;
    info.kind = event.boundary_kind;
    m_wait_boundary_infos.push_back(info);

    WaitBoundaryRecord record;
    record.inst = event.inst;
    record.scheduling_context_id = event.scheduling_context_id;
    record.sequence_index = event.sequence_index;
    record.event_order = event.event_order;
    record.sibling_group = event.phase_id;
    record.taskgroup_id = event.taskgroup_id;
    record.semantic_entity_id = event.semantic_entity_id;
    record.is_taskgroup_end = event.is_taskgroup_end;
    record.is_partial_wait = event.is_partial_wait;
    record.kind = event.boundary_kind;
    record.dependencies = event.dependencies;
    m_wait_boundaries[event.scheduling_context_id].push_back(std::move(record));
  }
}

void OpenMPSemantics::identifySemanticStructure() {
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  std::set<const Function *> directly_called;
  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }
    for (const BasicBlock &bb : func) {
      for (const Instruction &inst : bb) {
        const auto *call = dyn_cast<CallBase>(&inst);
        if (!call) {
          continue;
        }
        const Function *callee = call->getCalledFunction();
        if (!callee) {
          if (const Value *called = call->getCalledOperand()) {
            callee = dyn_cast<Function>(called->stripPointerCasts());
          }
        }
        if (callee && !callee->isDeclaration()) {
          directly_called.insert(callee);
        }
        if (api->isForkLike(call)) {
          if (const auto *fork_target =
                  dyn_cast_or_null<Function>(api->getForkedFun(call))) {
            if (!fork_target->isDeclaration()) {
              directly_called.insert(fork_target);
            }
          }
        }
        ThreadAPI::TD_TYPE type = api->getType(call);
        if (type == ThreadAPI::TD_OMP_TASK_WITH_DEPS ||
            type == ThreadAPI::TD_OMP_TASK || type == ThreadAPI::TD_OMP_TASKLOOP) {
          if (const Function *task_function = extractTaskFunction(call)) {
            if (!task_function->isDeclaration()) {
              directly_called.insert(task_function);
            }
          }
        }
      }
    }
  }

  std::vector<const Function *> roots;
  for (const Function &func : m_module) {
    if (!func.isDeclaration() && !directly_called.count(&func)) {
      roots.push_back(&func);
    }
  }
  if (roots.empty()) {
    for (const Function &func : m_module) {
      if (!func.isDeclaration()) {
        roots.push_back(&func);
      }
    }
  }

  for (const Function *root : roots) {
    TraversalState state;
    state.scheduling_context_id = m_next_scheduling_context_id++;
    state.scheduling_context_entity_id =
        addEntity(SemanticEntityKind::SchedulingContext, nullptr, root,
                  state.scheduling_context_id, 0, 0, 0, 0);
    state.phase_stack.push_back(0);
    std::set<const Function *> call_stack;
    scanSchedulingContext(root, state, call_stack);
  }
}

void OpenMPSemantics::attachDataSharingFacts() {
  DataSharingAnalysis data_sharing(m_module);
  data_sharing.analyze();

  auto entriesForFunction = [&](const Function *func) {
    return func ? data_sharing.getEntriesForRegion(func)
                : std::vector<DataSharingEntry>{};
  };

  for (auto &entity : m_entities) {
    entity.data_sharing_entries = entriesForFunction(entity.function);
  }

  for (auto &task_uptr : m_tasks) {
    Task *task = task_uptr.get();
    if (!task) {
      continue;
    }
    task->data_sharing_entries = entriesForFunction(task->task_function);
    if (task->data_sharing_entries.empty()) {
      task->data_sharing_entries = entriesForFunction(task->parent_context);
    }
    for (SemanticEntity &entity : m_entities) {
      if (entity.id == task->semantic_entity_id &&
          entity.data_sharing_entries.empty()) {
        entity.data_sharing_entries = task->data_sharing_entries;
        break;
      }
    }
  }
}

void OpenMPSemantics::buildTaskRelations() {
  auto recordRelation =
      [&](const Task *lhs, const Task *rhs, concurrency::RelationKind kind,
          concurrency::ProofStrength proof, StringRef reason) {
        if (!lhs || !rhs || lhs == rhs) {
          return;
        }
        auto key = normalizeTaskPair(lhs, rhs);
        concurrency::Relation relation;
        relation.kind = kind;
        relation.proof = proof;
        relation.reason = reason.str();

        auto it = m_relations.find(key);
        if (it == m_relations.end() ||
            concurrency::relationPriority(kind) >
                concurrency::relationPriority(it->second.kind)) {
          m_relations[key] = std::move(relation);
        }
      };

  auto deferredPartialWaitReason = [&](const Task *lhs,
                                       const Task *rhs) -> StringRef {
    if (!lhs || !rhs ||
        lhs->scheduling_context_id != rhs->scheduling_context_id) {
      return "";
    }
    const size_t earlier = std::min(lhs->event_order, rhs->event_order);
    const size_t later = std::max(lhs->event_order, rhs->event_order);
    for (const OpenMPTaskEvent &event : m_task_events) {
      if (event.scheduling_context_id != lhs->scheduling_context_id ||
          !event.is_partial_wait || !event.dependencies.empty()) {
        continue;
      }
      if (earlier < event.event_order && event.event_order <= later) {
        switch (event.boundary_kind) {
        case WaitBoundaryInfo::Kind::Flush:
          return "omp_flush_witness_required";
        case WaitBoundaryInfo::Kind::DoacrossWait:
          return "omp_doacross_partial";
        case WaitBoundaryInfo::Kind::TargetNowait:
          return "omp_target_nowait_partial";
        case WaitBoundaryInfo::Kind::TargetDataNowait:
          return "omp_target_data_nowait_partial";
        case WaitBoundaryInfo::Kind::ReduceNowait:
          return "omp_reduction_nowait_partial";
        default:
          return "omp_taskwait_deps_partial";
        }
      }
    }
    return "";
  };

  auto hasUniqueMatchingDoacrossSubmit = [&](const OpenMPTaskEvent &boundary) {
    if (boundary.boundary_kind != WaitBoundaryInfo::Kind::DoacrossWait ||
        boundary.dependencies.empty()) {
      return true;
    }

    const OpenMPTaskEvent *match = nullptr;
    for (const OpenMPTaskEvent &event : m_task_events) {
      if (event.kind != OpenMPTaskEvent::Kind::DoacrossSubmit ||
          event.scheduling_context_id != boundary.scheduling_context_id ||
          !event.inst || !boundary.inst ||
          !mustHappenBefore(event.inst, boundary.inst)) {
        continue;
      }

      bool conflicts = false;
      for (const Dependency &submit_dep : event.dependencies) {
        for (const Dependency &wait_dep : boundary.dependencies) {
          if (classifyDependencyConflict(submit_dep, wait_dep) ==
              DependencyConflict::MustConflict) {
            conflicts = true;
            break;
          }
        }
        if (conflicts) {
          break;
        }
      }

      if (!conflicts) {
        continue;
      }
      if (match) {
        ++m_deferred_reason_counts["omp_doacross_submit_ambiguous"];
        return false;
      }
      match = &event;
    }

    if (!match) {
      ++m_deferred_reason_counts["omp_doacross_submit_missing"];
      return false;
    }
    return true;
  };

  for (size_t i = 0; i < m_tasks.size(); ++i) {
    Task *task_i = m_tasks[i].get();
    for (size_t j = i + 1; j < m_tasks.size(); ++j) {
      Task *task_j = m_tasks[j].get();
      if (task_i->scheduling_context_id != task_j->scheduling_context_id) {
        continue;
      }
      StringRef deferred_reason = deferredPartialWaitReason(task_i, task_j);
      if (!deferred_reason.empty()) {
        ++m_deferred_reason_counts[deferred_reason.str()];
        recordRelation(
            task_i, task_j, concurrency::RelationKind::UnknownDueToModelGap,
            concurrency::ProofStrength::Unknown, deferred_reason);
        continue;
      }

      bool saw_conflict = false;
      bool saw_mutex_exclusion = false;
      bool saw_unknown_conflict = false;
      for (const Dependency &dep_i : task_i->dependencies) {
        for (const Dependency &dep_j : task_j->dependencies) {
          DependencyConflict conflict =
              classifyDependencyConflict(dep_i, dep_j);
          if (conflict == DependencyConflict::MustConflict) {
            saw_conflict = true;
            saw_mutex_exclusion =
                saw_mutex_exclusion || isMutexLikeExclusion(dep_i, dep_j);
          } else if (conflict == DependencyConflict::MayConflict ||
                     conflict == DependencyConflict::Unknown) {
            saw_unknown_conflict = true;
          }
        }
      }

      if (!saw_conflict) {
        if (saw_unknown_conflict) {
          recordRelation(
              task_i, task_j, concurrency::RelationKind::UnknownDueToModelGap,
              concurrency::ProofStrength::May, "omp_depend_may_conflict");
        }
        continue;
      }

      if (saw_mutex_exclusion) {
        task_i->exclusions.insert(task_j);
        task_j->exclusions.insert(task_i);
        recordRelation(task_i, task_j,
                       concurrency::RelationKind::MutuallyExclusive,
                       concurrency::ProofStrength::Must, "omp_mutexinoutset");
        continue;
      }

      if (mustHappenBefore(taskOrderingSite(task_i), taskOrderingSite(task_j))) {
        task_i->successors.insert(task_j);
        task_j->predecessors.insert(task_i);
        recordRelation(task_i, task_j,
                       concurrency::RelationKind::MustHappenBefore,
                       concurrency::ProofStrength::Must, "omp_depend_ordered");
      } else if (mustHappenBefore(taskOrderingSite(task_j),
                                  taskOrderingSite(task_i))) {
        task_j->successors.insert(task_i);
        task_i->predecessors.insert(task_j);
        recordRelation(task_i, task_j,
                       concurrency::RelationKind::MustHappenBefore,
                       concurrency::ProofStrength::Must, "omp_depend_ordered");
      } else {
        recordRelation(
            task_i, task_j, concurrency::RelationKind::UnknownDueToModelGap,
            concurrency::ProofStrength::Unknown, "omp_nonlexical_task_order");
      }
    }
  }

  auto selectiveReasonForBoundary = [](WaitBoundaryInfo::Kind kind) {
    switch (kind) {
    case WaitBoundaryInfo::Kind::Flush:
      return std::make_pair(StringRef("omp_flush_selective"),
                            StringRef("omp_flush_witness_required"));
    case WaitBoundaryInfo::Kind::DoacrossWait:
      return std::make_pair(StringRef("omp_doacross_selective"),
                            StringRef("omp_doacross_partial"));
    case WaitBoundaryInfo::Kind::TargetNowait:
      return std::make_pair(StringRef("omp_target_nowait_selective"),
                            StringRef("omp_target_nowait_partial"));
    case WaitBoundaryInfo::Kind::TargetDataNowait:
      return std::make_pair(StringRef("omp_target_data_nowait_selective"),
                            StringRef("omp_target_data_nowait_partial"));
    case WaitBoundaryInfo::Kind::ReduceNowait:
      return std::make_pair(StringRef("omp_reduction_nowait_selective"),
                            StringRef("omp_reduction_nowait_partial"));
    default:
      return std::make_pair(StringRef("omp_taskwait_deps_selective"),
                            StringRef("omp_taskwait_deps_partial"));
    }
  };

  for (const OpenMPTaskEvent &boundary : m_task_events) {
    switch (boundary.kind) {
    case OpenMPTaskEvent::Kind::Taskwait:
    case OpenMPTaskEvent::Kind::TaskwaitDeps:
    case OpenMPTaskEvent::Kind::TaskgroupEnd:
    case OpenMPTaskEvent::Kind::Barrier:
    case OpenMPTaskEvent::Kind::Flush:
    case OpenMPTaskEvent::Kind::DoacrossWait:
    case OpenMPTaskEvent::Kind::TargetBoundary:
    case OpenMPTaskEvent::Kind::ReductionNowaitBoundary:
      break;
    default:
      continue;
    }

      for (const auto &lhs : m_tasks) {
        if (lhs->scheduling_context_id != boundary.scheduling_context_id ||
            lhs->event_order >= boundary.event_order) {
          continue;
        }
        if (boundary.is_taskgroup_end &&
            (boundary.taskgroup_id == 0 ||
             lhs->taskgroup_id != boundary.taskgroup_id)) {
          continue;
        }
        if (!boundary.is_taskgroup_end && !boundary.is_partial_wait &&
            lhs->phase_id != boundary.phase_id) {
          continue;
        }
        for (const auto &rhs : m_tasks) {
          if (rhs->scheduling_context_id != boundary.scheduling_context_id ||
              rhs.get() == lhs.get() ||
              rhs->event_order < boundary.event_order) {
            continue;
          }
          if (!boundary.is_taskgroup_end && !boundary.is_partial_wait &&
              rhs->phase_id <= boundary.phase_id) {
            continue;
          }
      if (boundary.is_partial_wait) {
            if (boundary.boundary_kind == WaitBoundaryInfo::Kind::DoacrossWait &&
                !hasUniqueMatchingDoacrossSubmit(boundary)) {
              recordRelation(lhs.get(), rhs.get(),
                             concurrency::RelationKind::UnknownDueToModelGap,
                             concurrency::ProofStrength::Unknown,
                             "omp_doacross_submit_missing");
              continue;
            }
            bool lhs_selected = false;
            bool rhs_selected = false;
            for (const Dependency &wait_dep : boundary.dependencies) {
              for (const Dependency &lhs_dep : lhs->dependencies) {
                if (classifyDependencyConflict(lhs_dep, wait_dep) ==
                    DependencyConflict::MustConflict) {
                  lhs_selected = true;
                  break;
                }
              }
              for (const Dependency &rhs_dep : rhs->dependencies) {
                if (classifyDependencyConflict(rhs_dep, wait_dep) ==
                    DependencyConflict::MustConflict) {
                  rhs_selected = true;
                  break;
                }
              }
              if (lhs_selected && rhs_selected) {
                break;
              }
            }
            StringRef selective_reason = "omp_taskwait_deps_selective";
            StringRef deferred_reason = "omp_taskwait_deps_partial";
            std::tie(selective_reason, deferred_reason) =
                selectiveReasonForBoundary(boundary.boundary_kind);
            if (!boundary.dependencies.empty() && lhs_selected &&
                rhs_selected &&
                mustHappenBefore(taskOrderingSite(lhs.get()), boundary.inst) &&
                mustHappenBefore(boundary.inst, taskOrderingSite(rhs.get()))) {
              lhs->successors.insert(rhs.get());
              rhs->predecessors.insert(lhs.get());
              recordRelation(lhs.get(), rhs.get(),
                             concurrency::RelationKind::SelectiveHappenBefore,
                             concurrency::ProofStrength::Must,
                             selective_reason);
            } else {
              ++m_deferred_reason_counts[deferred_reason.str()];
              recordRelation(lhs.get(), rhs.get(),
                             concurrency::RelationKind::UnknownDueToModelGap,
                             concurrency::ProofStrength::Unknown,
                             deferred_reason);
            }
            continue;
          }
          if (mustHappenBefore(taskOrderingSite(lhs.get()), boundary.inst) &&
              mustHappenBefore(boundary.inst, taskOrderingSite(rhs.get()))) {
            lhs->successors.insert(rhs.get());
            rhs->predecessors.insert(lhs.get());
            recordRelation(lhs.get(), rhs.get(),
                           concurrency::RelationKind::MustHappenBefore,
                           concurrency::ProofStrength::Must,
                           "omp_wait_boundary");
          } else {
            recordRelation(lhs.get(), rhs.get(),
                           concurrency::RelationKind::UnknownDueToModelGap,
                           concurrency::ProofStrength::Unknown,
                           "omp_conditional_wait_boundary");
          }
        }
      }
  }

  for (const OpenMPTaskEvent &event : m_task_events) {
    if (event.kind != OpenMPTaskEvent::Kind::TaskComplete || !event.task ||
        !event.inst) {
      continue;
    }
    Task *completed_task = const_cast<Task *>(event.task);
    if (!completed_task) {
      continue;
    }
    for (const auto &rhs : m_tasks) {
      if (!rhs || rhs.get() == event.task ||
          rhs->scheduling_context_id != event.scheduling_context_id ||
          rhs->event_order < event.event_order) {
        continue;
      }
      if (!mustHappenBefore(event.inst, taskOrderingSite(rhs.get()))) {
        continue;
      }
      completed_task->successors.insert(rhs.get());
      rhs->predecessors.insert(completed_task);
      recordRelation(completed_task, rhs.get(),
                     concurrency::RelationKind::MustHappenBefore,
                     concurrency::ProofStrength::Must,
                     "omp_detached_task_completion");
    }
  }
}

void OpenMPSemantics::scanSchedulingContext(const Function *func,
                                            TraversalState &state,
                                            std::set<const Function *> &call_stack) {
  if (!func || func->isDeclaration() || !call_stack.insert(func).second) {
    return;
  }

  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto currentPhaseToken = [&state]() -> size_t {
    return state.phase_stack.empty() ? 0 : state.phase_stack.back();
  };
  auto currentRegionId = [&state]() -> size_t {
    return state.region_stack.empty() ? 0 : state.region_stack.back().id;
  };
  auto currentRegionEntityId = [&state]() -> size_t {
    return state.region_stack.empty() ? state.scheduling_context_entity_id
                                      : state.region_stack.back().entity_id;
  };
  auto nextEventOrder = [&state]() -> size_t { return state.next_event_order++; };
  auto advanceCurrentPhase = [&state]() {
    if (state.phase_stack.empty()) {
      state.phase_stack.push_back(state.next_phase_token++);
    } else {
      state.phase_stack.back() = state.next_phase_token++;
    }
  };
  auto pushRegion = [&](WaitBoundaryInfo::Kind kind, SemanticEntityKind entity_kind,
                        const Instruction *anchor = nullptr) {
    TraversalState::RegionFrame frame;
    frame.id = state.next_region_id++;
    frame.kind = kind;
    frame.entity_id =
        addEntity(entity_kind, anchor, func, state.scheduling_context_id,
                  currentRegionEntityId(), frame.id, currentPhaseToken(),
                  state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back());
    state.region_stack.push_back(frame);
    const size_t event_order = nextEventOrder();
    addEvent(SemanticEventKind::RegionBegin, anchor, frame.entity_id,
             state.scheduling_context_id, state.sequence_index, event_order,
             frame.id,
             currentPhaseToken());
    return frame;
  };
  auto popRegion = [&](WaitBoundaryInfo::Kind kind, const Instruction *anchor) {
    if (state.region_stack.empty()) {
      ++m_deferred_reason_counts["omp_region_end_unmatched"];
      return TraversalState::RegionFrame{};
    }
    if (state.region_stack.back().kind == kind) {
      TraversalState::RegionFrame frame = state.region_stack.back();
      state.region_stack.pop_back();
      const size_t event_order = nextEventOrder();
      addEvent(SemanticEventKind::RegionEnd, anchor, frame.entity_id,
               state.scheduling_context_id, state.sequence_index, event_order,
               frame.id,
               currentPhaseToken());
      return frame;
    }
    for (size_t idx = state.region_stack.size(); idx > 0; --idx) {
      if (state.region_stack[idx - 1].kind != kind) {
        continue;
      }
      ++m_deferred_reason_counts["omp_region_mismatched_end"];
      const size_t stale_frames = state.region_stack.size() - idx;
      if (stale_frames != 0) {
        m_deferred_reason_counts["omp_region_stale_frames_dropped"] +=
            stale_frames;
      }
      for (size_t stale_idx = state.region_stack.size(); stale_idx > idx - 1;
           --stale_idx) {
        const TraversalState::RegionFrame &frame = state.region_stack[stale_idx - 1];
        const size_t event_order = nextEventOrder();
        addEvent(SemanticEventKind::RegionEnd, anchor, frame.entity_id,
                 state.scheduling_context_id, state.sequence_index, event_order,
                 frame.id,
                 currentPhaseToken());
      }
      TraversalState::RegionFrame frame = state.region_stack[idx - 1];
      state.region_stack.erase(state.region_stack.begin() + (idx - 1),
                               state.region_stack.end());
      return frame;
    }
    ++m_deferred_reason_counts["omp_region_end_unmatched"];
    return TraversalState::RegionFrame{};
  };
  auto recordBoundary = [&](const CallBase *call, WaitBoundaryInfo::Kind kind,
                            bool partial_wait = false,
                            bool taskgroup_end = false) {
    const size_t entity_id =
        addEntity(SemanticEntityKind::WaitBoundary, call, func,
                  state.scheduling_context_id, currentRegionEntityId(),
                  currentRegionId(), currentPhaseToken(),
                  state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back());
    WaitBoundaryInfo info;
    info.inst = call;
    info.scheduling_context_id = state.scheduling_context_id;
    info.sequence_index = state.sequence_index;
    info.event_order = nextEventOrder();
    info.phase_id = currentPhaseToken();
    info.region_id = currentRegionId();
    info.semantic_entity_id = entity_id;
    info.kind = kind;
    info.is_partial_wait = partial_wait;
    info.is_taskgroup_end = taskgroup_end;
    m_wait_boundary_infos.push_back(info);
    addEvent(kind == WaitBoundaryInfo::Kind::Barrier ? SemanticEventKind::Barrier
                                                     : kind == WaitBoundaryInfo::Kind::Flush
                                                           ? SemanticEventKind::Flush
                                                           : kind == WaitBoundaryInfo::Kind::Target ||
                                                                     kind == WaitBoundaryInfo::Kind::TargetNowait ||
                                                                     kind == WaitBoundaryInfo::Kind::TargetData ||
                                                                     kind == WaitBoundaryInfo::Kind::TargetDataNowait
                                                                 ? SemanticEventKind::TargetLaunch
                                                                 : SemanticEventKind::Boundary,
             call, entity_id, state.scheduling_context_id, state.sequence_index,
             info.event_order, currentRegionId(), currentPhaseToken(),
             partial_wait);
    return info;
  };

  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = api->getCallee(call);
      ThreadAPI::TD_TYPE type = api->getType(callee);
      ThreadAPI::RuntimeLibrary library = api->getRuntimeLibrary(callee);
      bool is_nowait_variant = callee && callee->getName().contains("nowait");

      if (library == ThreadAPI::RuntimeLibrary::OpenMP) {
        if (type == ThreadAPI::TD_FORK) {
          ++m_summary.parallel_region_count;
          size_t current_depth = state.region_stack.size() + 1;
          m_nested_depth = std::max(m_nested_depth, current_depth);
          m_summary.nested_parallelism_max_depth =
              std::max(m_summary.nested_parallelism_max_depth, current_depth);
          if (current_depth > 1) {
            ++m_summary.nested_parallelism_nested_regions;
          } else {
            ++m_summary.nested_parallelism_flat_regions;
          }
          const bool explicit_parallel_end =
              callee && api->hasTrait(callee, "parallel-explicit-end");
          TraversalState::RegionFrame frame =
              pushRegion(WaitBoundaryInfo::Kind::Unknown,
                         SemanticEntityKind::ParallelRegion, call);
          m_region_nesting_depth[frame.id] = current_depth;
          if (const auto *fork_target =
                  dyn_cast_or_null<Function>(api->getForkedFun(call))) {
            TraversalState fork_state;
            fork_state.scheduling_context_id = m_next_scheduling_context_id++;
            fork_state.scheduling_context_entity_id =
                addEntity(SemanticEntityKind::SchedulingContext, call,
                          fork_target, fork_state.scheduling_context_id,
                          frame.entity_id, frame.id, currentPhaseToken(), 0);
            fork_state.phase_stack.push_back(0);
            fork_state.anchor_inst = call;
            std::set<const Function *> nested_call_stack;
            scanSchedulingContext(fork_target, fork_state, nested_call_stack);
          }
          if (!explicit_parallel_end) {
            popRegion(WaitBoundaryInfo::Kind::Unknown, call);
          }
          continue;
        }

        if (type == ThreadAPI::TD_BAR_WAIT) {
          ++m_summary.barrier_count;
          WaitBoundaryInfo boundary =
              recordBoundary(call, WaitBoundaryInfo::Kind::Barrier);
          addTaskEvent(OpenMPTaskEvent::Kind::Barrier, call,
                       state.scheduling_context_id, state.sequence_index,
                       boundary.event_order,
                       currentPhaseToken(),
                       state.taskgroup_stack.empty() ? 0
                                                     : state.taskgroup_stack.back(),
                       currentRegionId(), boundary.semantic_entity_id, nullptr,
                       WaitBoundaryInfo::Kind::Barrier);
          if (callee && api->hasTrait(callee, "parallel-end")) {
            popRegion(WaitBoundaryInfo::Kind::Unknown, call);
          }
          advanceCurrentPhase();
          continue;
        }

        if (type == ThreadAPI::TD_OMP_TASKYIELD) {
          ++m_summary.taskyield_count;
          continue;
        }
        if (type == ThreadAPI::TD_ACQUIRE || type == ThreadAPI::TD_RELEASE ||
            type == ThreadAPI::TD_TRY_ACQUIRE) {
          if (api->semanticTagStartsWith(callee, "critical")) {
            if (type == ThreadAPI::TD_ACQUIRE) {
              ++m_summary.critical_region_count;
              pushRegion(WaitBoundaryInfo::Kind::CriticalEnd,
                         SemanticEntityKind::CriticalRegion, call);
            } else if (type == ThreadAPI::TD_RELEASE) {
              popRegion(WaitBoundaryInfo::Kind::CriticalEnd, call);
            }
          } else {
            ++m_summary.lock_api_count;
          }
          continue;
        }
        if (type == ThreadAPI::TD_OMP_CANCEL) {
          if (api->hasSemanticTag(callee, "cancellation-point")) {
            ++m_summary.cancellation_point_count;
            ++m_deferred_reason_counts["omp_cancellation_point_runtime_unmodeled"];
          } else {
            ++m_summary.cancel_count;
            ++m_deferred_reason_counts["omp_cancel_runtime_unmodeled"];
          }
          continue;
        }
      }

      if (type == ThreadAPI::TD_OMP_TASKWAIT_DEPS) {
        std::vector<Dependency> dependencies =
            extractRuntimeDependencies(call, 2, 3);
        WaitBoundaryInfo boundary =
            recordBoundary(call, WaitBoundaryInfo::Kind::TaskwaitDeps,
                           dependencies.empty());
        addTaskEvent(OpenMPTaskEvent::Kind::TaskwaitDeps, call,
                     state.scheduling_context_id, state.sequence_index,
                     boundary.event_order,
                     currentPhaseToken(),
                     state.taskgroup_stack.empty() ? 0
                                                   : state.taskgroup_stack.back(),
                     currentRegionId(), boundary.semantic_entity_id, nullptr,
                     WaitBoundaryInfo::Kind::TaskwaitDeps,
                     dependencies.empty(), false, std::move(dependencies));
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASKWAIT) {
        WaitBoundaryInfo boundary =
            recordBoundary(call, WaitBoundaryInfo::Kind::Taskwait);
        addTaskEvent(OpenMPTaskEvent::Kind::Taskwait, call,
                     state.scheduling_context_id, state.sequence_index,
                     boundary.event_order,
                     currentPhaseToken(),
                     state.taskgroup_stack.empty() ? 0
                                                   : state.taskgroup_stack.back(),
                     currentRegionId(), boundary.semantic_entity_id, nullptr,
                     WaitBoundaryInfo::Kind::Taskwait);
        advanceCurrentPhase();
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASKGROUP_START) {
        size_t taskgroup_id = state.next_taskgroup_id++;
        state.taskgroup_stack.push_back(taskgroup_id);
        state.phase_stack.push_back(state.next_phase_token++);
        TraversalState::RegionFrame frame =
            pushRegion(WaitBoundaryInfo::Kind::TaskgroupEnd,
                       SemanticEntityKind::Taskgroup, call);
        const size_t event_order = nextEventOrder();
        addTaskEvent(OpenMPTaskEvent::Kind::TaskgroupBegin, call,
                     state.scheduling_context_id, state.sequence_index,
                     event_order,
                     currentPhaseToken(), taskgroup_id, frame.id,
                     frame.entity_id);
        ++m_summary.taskgroup_region_count;
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASKGROUP_END) {
        WaitBoundaryInfo info =
            recordBoundary(call, WaitBoundaryInfo::Kind::TaskgroupEnd, false, true);
        size_t taskgroup_id = 0;
        if (!state.taskgroup_stack.empty()) {
          taskgroup_id = state.taskgroup_stack.back();
          info.taskgroup_id = taskgroup_id;
          state.taskgroup_stack.pop_back();
        }
        TraversalState::RegionFrame frame =
            popRegion(WaitBoundaryInfo::Kind::TaskgroupEnd, call);
        info.region_id = frame.id;
        addTaskEvent(OpenMPTaskEvent::Kind::TaskgroupEnd, call,
                     state.scheduling_context_id, state.sequence_index,
                     info.event_order,
                     currentPhaseToken(), taskgroup_id, frame.id,
                     info.semantic_entity_id, nullptr,
                     WaitBoundaryInfo::Kind::TaskgroupEnd, false, true);
        if (!state.phase_stack.empty()) {
          state.phase_stack.pop_back();
        }
        advanceCurrentPhase();
        continue;
      }

      if (type == ThreadAPI::TD_OMP_SINGLE_START) {
        pushRegion(WaitBoundaryInfo::Kind::SingleEnd,
                   SemanticEntityKind::SingleRegion, call);
        ++m_summary.single_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_MASTER_START) {
        pushRegion(WaitBoundaryInfo::Kind::MasterEnd,
                   SemanticEntityKind::MasterRegion, call);
        ++m_summary.master_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_ORDERED_START) {
        pushRegion(WaitBoundaryInfo::Kind::OrderedEnd,
                   SemanticEntityKind::OrderedRegion, call);
        ++m_summary.ordered_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_FOR_STATIC_INIT ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_INIT) {
        pushRegion(type == ThreadAPI::TD_OMP_FOR_STATIC_INIT
                       ? WaitBoundaryInfo::Kind::ForFini
                       : WaitBoundaryInfo::Kind::DispatchFini,
                   SemanticEntityKind::WorksharingLoop, call);
        ++m_summary.worksharing_loop_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_REDUCE_NOWAIT_START) {
        pushRegion(WaitBoundaryInfo::Kind::Unknown,
                   SemanticEntityKind::ReductionRegion, call);
        ++m_summary.reduction_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_SECTIONS_INIT) {
        ++m_summary.sections_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_ATOMIC_START) {
        ++m_summary.atomic_region_count;
        ++m_deferred_reason_counts["omp_atomic_runtime_unmodeled"];
        continue;
      }
      if (type == ThreadAPI::TD_OMP_TARGET) {
        ++m_summary.target_region_count;
        WaitBoundaryInfo::Kind kind = is_nowait_variant
                                          ? WaitBoundaryInfo::Kind::TargetNowait
                                          : WaitBoundaryInfo::Kind::Target;
        WaitBoundaryInfo boundary =
            recordBoundary(call, kind, is_nowait_variant);
        addTaskEvent(OpenMPTaskEvent::Kind::TargetBoundary, call,
                     state.scheduling_context_id, state.sequence_index,
                     boundary.event_order,
                     currentPhaseToken(),
                     state.taskgroup_stack.empty() ? 0
                                                   : state.taskgroup_stack.back(),
                     currentRegionId(), boundary.semantic_entity_id, nullptr,
                     kind, is_nowait_variant);
        if (is_nowait_variant) {
          ++m_summary.target_nowait_boundary_count;
        }
        if (!is_nowait_variant) {
          advanceCurrentPhase();
        }
        continue;
      }
      if (type == ThreadAPI::TD_OMP_TARGET_DATA_BEGIN ||
          type == ThreadAPI::TD_OMP_TARGET_DATA_END ||
          type == ThreadAPI::TD_OMP_TARGET_DATA_UPDATE) {
        ++m_summary.target_data_region_count;
        if (type == ThreadAPI::TD_OMP_TARGET_DATA_END) {
          WaitBoundaryInfo::Kind kind = is_nowait_variant
                                            ? WaitBoundaryInfo::Kind::TargetDataNowait
                                            : WaitBoundaryInfo::Kind::TargetData;
          WaitBoundaryInfo boundary =
              recordBoundary(call, kind, is_nowait_variant);
          addTaskEvent(OpenMPTaskEvent::Kind::TargetBoundary, call,
                       state.scheduling_context_id, state.sequence_index,
                       boundary.event_order,
                       currentPhaseToken(),
                       state.taskgroup_stack.empty() ? 0
                                                     : state.taskgroup_stack.back(),
                       currentRegionId(), boundary.semantic_entity_id, nullptr,
                       kind, is_nowait_variant);
          if (is_nowait_variant) {
            ++m_summary.target_nowait_boundary_count;
          }
          if (!is_nowait_variant) {
            advanceCurrentPhase();
          }
        }
        continue;
      }
      if (type == ThreadAPI::TD_OMP_DOACROSS_INIT) {
        ++m_summary.doacross_init_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_DOACROSS_WAIT) {
        ++m_summary.doacross_wait_count;
        std::vector<Dependency> dependencies;
        if (call->arg_size() >= 3) {
          const Value *witness = stripValue(call->getArgOperand(2));
          if (witness && !isa<ConstantPointerNull>(witness)) {
            Dependency dep;
            dep.address = witness;
            dep.type = DependType::INOUT;
            dep.size = 0;
            dep.source_kind = DependencySourceKind::Iterator;
            dep.proof = DependencyProof::Possible;
            int64_t offset = 0;
            bool precise = false;
            dep.canonical_base = canonicalizeDependencyAddress(
                witness, m_module.getDataLayout(), offset, precise);
            dep.offset = offset;
            dep.has_precise_offset = precise;
            dependencies.push_back(dep);
          }
        }
        WaitBoundaryInfo boundary =
            recordBoundary(call, WaitBoundaryInfo::Kind::DoacrossWait, true);
        addTaskEvent(OpenMPTaskEvent::Kind::DoacrossWait, call,
                     state.scheduling_context_id, state.sequence_index,
                     boundary.event_order,
                     currentPhaseToken(),
                     state.taskgroup_stack.empty() ? 0
                                                   : state.taskgroup_stack.back(),
                     currentRegionId(), boundary.semantic_entity_id, nullptr,
                     WaitBoundaryInfo::Kind::DoacrossWait, true, false,
                     std::move(dependencies));
        continue;
      }
      if (type == ThreadAPI::TD_OMP_DOACROSS_SUBMIT) {
        ++m_summary.doacross_submit_count;
        std::vector<Dependency> dependencies;
        if (call->arg_size() >= 3) {
          const Value *witness = stripValue(call->getArgOperand(2));
          if (witness && !isa<ConstantPointerNull>(witness)) {
            Dependency dep;
            dep.address = witness;
            dep.type = DependType::INOUT;
            dep.size = 0;
            dep.source_kind = DependencySourceKind::Iterator;
            dep.proof = DependencyProof::Possible;
            int64_t offset = 0;
            bool precise = false;
            dep.canonical_base = canonicalizeDependencyAddress(
                witness, m_module.getDataLayout(), offset, precise);
            dep.offset = offset;
            dep.has_precise_offset = precise;
            dependencies.push_back(dep);
          }
        }
        const size_t event_order = nextEventOrder();
        addTaskEvent(OpenMPTaskEvent::Kind::DoacrossSubmit, call,
                     state.scheduling_context_id, state.sequence_index,
                     event_order,
                     currentPhaseToken(),
                     state.taskgroup_stack.empty() ? 0
                                                   : state.taskgroup_stack.back(),
                     currentRegionId(), currentRegionEntityId(), nullptr,
                     WaitBoundaryInfo::Kind::Unknown, false, false,
                     std::move(dependencies));
        continue;
      }
      if (type == ThreadAPI::TD_OMP_TASK_COMPLETE) {
        ++m_summary.detach_completion_count;
        const Task *completed_task =
            call->arg_size() >= 3 ? getTaskForHandle(call->getArgOperand(2)) : nullptr;
        if (!completed_task) {
          ++m_deferred_reason_counts["omp_detached_task_completion_unresolved"];
        }
        const size_t event_order = nextEventOrder();
        addTaskEvent(OpenMPTaskEvent::Kind::TaskComplete, call,
                     state.scheduling_context_id, state.sequence_index,
                     event_order,
                     currentPhaseToken(),
                     state.taskgroup_stack.empty() ? 0
                                                   : state.taskgroup_stack.back(),
                     currentRegionId(), currentRegionEntityId(), completed_task);
        continue;
      }
      if (type == ThreadAPI::TD_OMP_TEAMS ||
          type == ThreadAPI::TD_OMP_TEAMS_HOST ||
          type == ThreadAPI::TD_OMP_TEAMS_DISTRIBUTE) {
        ++m_summary.teams_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_DISTRIBUTE ||
          type == ThreadAPI::TD_OMP_DISTRIBUTE_STATIC ||
          type == ThreadAPI::TD_OMP_DISTRIBUTE_DYNAMIC ||
          type == ThreadAPI::TD_OMP_DISTRIBUTE_GUIDANCE) {
        ++m_summary.distribute_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_LOOP_STATIC_INIT ||
          type == ThreadAPI::TD_OMP_LOOP_DYNAMIC_INIT ||
          type == ThreadAPI::TD_OMP_LOOP_GUIDANCE_INIT) {
        ++m_summary.loop_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_AFFINITY) {
        ++m_summary.affinity_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_SCOPE_START ||
          type == ThreadAPI::TD_OMP_SCOPE_END) {
        ++m_summary.scope_region_count;
        continue;
      }

      if (type == ThreadAPI::TD_OMP_MASTER_END ||
          type == ThreadAPI::TD_OMP_ORDERED_END) {
        popRegion(type == ThreadAPI::TD_OMP_MASTER_END
                      ? WaitBoundaryInfo::Kind::MasterEnd
                      : WaitBoundaryInfo::Kind::OrderedEnd,
                  call);
        continue;
      }

      if (type == ThreadAPI::TD_OMP_SINGLE_END ||
          type == ThreadAPI::TD_OMP_SECTIONS_END ||
          type == ThreadAPI::TD_OMP_FOR_STATIC_FINI ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_FINI ||
          type == ThreadAPI::TD_OMP_REDUCE_START) {
        WaitBoundaryInfo::Kind kind = WaitBoundaryInfo::Kind::Unknown;
        if (type == ThreadAPI::TD_OMP_SINGLE_END) {
          kind = WaitBoundaryInfo::Kind::SingleEnd;
        } else if (type == ThreadAPI::TD_OMP_SECTIONS_END) {
          kind = WaitBoundaryInfo::Kind::SectionsEnd;
        } else if (type == ThreadAPI::TD_OMP_FOR_STATIC_FINI) {
          kind = WaitBoundaryInfo::Kind::ForFini;
        } else if (type == ThreadAPI::TD_OMP_FOR_DISPATCH_FINI) {
          kind = WaitBoundaryInfo::Kind::DispatchFini;
        } else if (type == ThreadAPI::TD_OMP_REDUCE_START) {
          kind = WaitBoundaryInfo::Kind::Reduce;
        }
        WaitBoundaryInfo info = recordBoundary(call, kind);
        TraversalState::RegionFrame frame;
        const bool should_pop_region =
            type == ThreadAPI::TD_OMP_SINGLE_END ||
            type == ThreadAPI::TD_OMP_FOR_STATIC_FINI ||
            type == ThreadAPI::TD_OMP_FOR_DISPATCH_FINI;
        if (should_pop_region) {
          frame = popRegion(kind, call);
        } else {
          frame.id = currentRegionId();
          frame.entity_id = currentRegionEntityId();
        }
        addTaskEvent(kind == WaitBoundaryInfo::Kind::Barrier
                         ? OpenMPTaskEvent::Kind::Barrier
                         : OpenMPTaskEvent::Kind::Taskwait,
                     call, state.scheduling_context_id, state.sequence_index,
                     info.event_order,
                     currentPhaseToken(),
                     state.taskgroup_stack.empty() ? 0
                                                   : state.taskgroup_stack.back(),
                     frame.id, info.semantic_entity_id, nullptr, kind);
        advanceCurrentPhase();
        continue;
      }

      if (type == ThreadAPI::TD_OMP_REDUCE_END ||
          type == ThreadAPI::TD_OMP_REDUCE_NOWAIT_END ||
          type == ThreadAPI::TD_OMP_FLUSH) {
        if (type == ThreadAPI::TD_OMP_FLUSH) {
          ++m_summary.flush_count;
          std::vector<Dependency> dependencies;
          if (call->arg_size() >= 1) {
            const Value *flush_obj = stripValue(call->getArgOperand(0));
            if (flush_obj) {
              Dependency dep;
              dep.address = flush_obj;
              dep.type = DependType::INOUT;
              dep.size = 0;
              dep.source_kind = DependencySourceKind::DirectAddress;
              dep.proof = DependencyProof::Possible;
              int64_t offset = 0;
              bool precise = false;
              dep.canonical_base = canonicalizeDependencyAddress(
                  flush_obj, m_module.getDataLayout(), offset, precise);
              dep.offset = offset;
              dep.has_precise_offset = precise;
              dependencies.push_back(dep);
            }
          }
          WaitBoundaryInfo boundary =
              recordBoundary(call, WaitBoundaryInfo::Kind::Flush, true);
          addTaskEvent(OpenMPTaskEvent::Kind::Flush, call,
                       state.scheduling_context_id, state.sequence_index,
                       boundary.event_order,
                       currentPhaseToken(),
                       state.taskgroup_stack.empty() ? 0
                                                     : state.taskgroup_stack.back(),
                       currentRegionId(), boundary.semantic_entity_id, nullptr,
                       WaitBoundaryInfo::Kind::Flush, true, false,
                       std::move(dependencies));
        } else if (type == ThreadAPI::TD_OMP_REDUCE_NOWAIT_END) {
          popRegion(WaitBoundaryInfo::Kind::Unknown, call);
          WaitBoundaryInfo boundary =
              recordBoundary(call, WaitBoundaryInfo::Kind::ReduceNowait, true);
          addTaskEvent(OpenMPTaskEvent::Kind::ReductionNowaitBoundary, call,
                       state.scheduling_context_id, state.sequence_index,
                       boundary.event_order,
                       currentPhaseToken(),
                       state.taskgroup_stack.empty() ? 0
                                                     : state.taskgroup_stack.back(),
                       currentRegionId(), boundary.semantic_entity_id, nullptr,
                       WaitBoundaryInfo::Kind::ReduceNowait, true);
          ++m_summary.reduction_nowait_boundary_count;
        }
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASK_WITH_DEPS ||
          type == ThreadAPI::TD_OMP_TASK || type == ThreadAPI::TD_OMP_TASKLOOP) {
        auto task = std::make_unique<Task>();
        task->task_create = call;
        task->task_function = extractTaskFunction(call);
        task->task_handle = canonicalizeTaskHandle(call->getArgOperand(2));
        applyTaskExecutionHints(*task, call);
        task->parent_context = func;
        task->generating_context = state.anchor_inst ? state.anchor_inst : call;
        task->scheduling_context_id = state.scheduling_context_id;
        task->taskgroup_id =
            state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back();
        task->phase_id = currentPhaseToken();
        task->sibling_group = currentPhaseToken();
        task->sequence_index = state.sequence_index++;
        task->event_order = nextEventOrder();
        task->region_id = currentRegionId();
        task->semantic_entity_id =
            addEntity(SemanticEntityKind::ExplicitTask, call,
                      task->task_function ? task->task_function : func,
                      state.scheduling_context_id, currentRegionEntityId(),
                      currentRegionId(), currentPhaseToken(),
                      task->taskgroup_id);
        addEvent(SemanticEventKind::TaskCreate, call, task->semantic_entity_id,
                 state.scheduling_context_id, task->sequence_index,
                 task->event_order,
                 currentRegionId(), currentPhaseToken());
        addTaskEvent(OpenMPTaskEvent::Kind::TaskCreate, call,
                     state.scheduling_context_id, task->sequence_index,
                     task->event_order,
                     currentPhaseToken(), task->taskgroup_id,
                     currentRegionId(), task->semantic_entity_id, task.get());

        if (type == ThreadAPI::TD_OMP_TASK_WITH_DEPS) {
          task->dependencies = extractDependencies(call);
          ++m_summary.task_with_dependencies_count;
          for (const Dependency &dep : task->dependencies) {
            if (dep.canonical_base) {
              task->synchronization_objects.insert(dep.canonical_base);
            }
          }
        }
        if (type == ThreadAPI::TD_OMP_TASKLOOP) {
          ++m_summary.taskloop_count;
        }
        if (task->execution_mode == TaskExecutionMode::Included) {
          ++m_summary.included_task_count;
        }
        if (task->is_final) {
          ++m_summary.final_task_count;
        }
        if (task->is_untied) {
          ++m_summary.untied_task_count;
        }
        if (task->is_detached) {
          ++m_summary.detached_task_count;
        }

        m_inst_to_task[call] = task.get();
        if (task->task_function && !task->task_function->isDeclaration()) {
          TraversalState task_state;
          task_state.scheduling_context_id = m_next_scheduling_context_id++;
          task_state.scheduling_context_entity_id =
              addEntity(SemanticEntityKind::SchedulingContext, call,
                        task->task_function, task_state.scheduling_context_id,
                        task->semantic_entity_id, currentRegionId(),
                        currentPhaseToken(), task->taskgroup_id);
          task_state.phase_stack.push_back(0);
          task_state.anchor_inst = call;
          std::set<const Function *> nested_call_stack = call_stack;
          scanSchedulingContext(task->task_function, task_state,
                                nested_call_stack);
        }
        m_tasks.push_back(std::move(task));
        continue;
      }

      if (callee && !callee->isDeclaration() && type == ThreadAPI::TD_DUMMY &&
          !OpenMPModel::isOpenMP(callee->getName())) {
        const Instruction *saved_anchor = state.anchor_inst;
        state.anchor_inst = call;
        scanSchedulingContext(callee, state, call_stack);
        state.anchor_inst = saved_anchor;
      }
    }
  }

  call_stack.erase(func);
}

std::vector<Dependency>
OpenMPSemantics::extractDependencies(const CallBase *task_call) {
  return extractRuntimeDependencies(task_call, 3, 4);
}

std::vector<Dependency> OpenMPSemantics::extractRuntimeDependencies(
    const CallBase *task_call, unsigned ndeps_arg_idx, unsigned dep_arg_idx) {
  std::vector<Dependency> deps;
  const DataLayout &DL = m_module.getDataLayout();

  if (!task_call ||
      task_call->arg_size() <= std::max(ndeps_arg_idx, dep_arg_idx)) {
    return deps;
  }

  const Value *ndeps_val = task_call->getArgOperand(ndeps_arg_idx);
  const ConstantInt *CI = dyn_cast<ConstantInt>(ndeps_val);
  if (!CI) {
    return deps;
  }
  uint64_t ndeps = CI->getZExtValue();

  const Value *dep_list = task_call->getArgOperand(dep_arg_idx);
  const Value *dep_root = resolveDependencyListValue(dep_list);
  const Value *dep_base =
      dep_root ? getUnderlyingObject(dep_root->stripPointerCasts()) : nullptr;
  if (!dep_base && dep_root) {
    dep_base = dep_root->stripPointerCasts();
  }

  if (const auto *gv = dyn_cast_or_null<GlobalVariable>(dep_base)) {
    if (const auto *init = gv->getInitializer()) {
      if (const auto *array = dyn_cast<ConstantArray>(init)) {
        for (unsigned i = 0; i < array->getNumOperands() && deps.size() < ndeps;
             ++i) {
          Dependency dep;
          if (decodeConstantDependency(dyn_cast<Constant>(array->getOperand(i)),
                                       dep)) {
            dep.source_kind = gv->isConstant()
                                  ? DependencySourceKind::DirectAddress
                                  : DependencySourceKind::RegionSummary;
            dep.proof = gv->isConstant() ? DependencyProof::Definite
                                         : DependencyProof::Possible;
            dep.canonical_base = canonicalizeDependencyAddress(
                dep.address, DL, dep.offset, dep.has_precise_offset);
            deps.push_back(dep);
          }
        }
      }
    }
  } else if (const auto *alloca = dyn_cast_or_null<AllocaInst>(dep_base)) {
    struct PartialDependency {
      const Value *address = nullptr;
      uint64_t size = 0;
      uint64_t flags = 0;
      DependencySourceKind source_kind = DependencySourceKind::RegionSummary;
      DependencyProof proof = DependencyProof::Unknown;
      bool has_address = false;
      bool has_size = false;
      bool has_flags = false;
    };

    std::map<uint64_t, PartialDependency> partials;
    const Function *parent = alloca->getFunction();
    if (!parent) {
      return deps;
    }
    for (const Instruction &inst : instructions(parent)) {
      const auto *store = dyn_cast<StoreInst>(&inst);
      if (!store) {
        continue;
      }
      const auto *gep = dyn_cast<GEPOperator>(store->getPointerOperand());
      if (!gep || getUnderlyingObject(gep->getPointerOperand()) != alloca) {
        continue;
      }
      if (gep->getNumIndices() < 2) {
        continue;
      }

      SmallVector<unsigned, 4> indices;
      bool all_constant = true;
      for (unsigned i = 0; i < gep->getNumIndices(); ++i) {
        const auto *ci = dyn_cast<ConstantInt>(gep->getOperand(i + 1));
        if (!ci) {
          all_constant = false;
          break;
        }
        indices.push_back(ci->getZExtValue());
      }
      if (!all_constant) {
        continue;
      }

      uint64_t dep_idx = indices[indices.size() - 2];
      unsigned field_idx = indices.back();
      if (dep_idx >= ndeps) {
        continue;
      }

      PartialDependency &partial = partials[dep_idx];
      const Value *stored = store->getValueOperand();
      if (field_idx == 0) {
        partial.address = stripValue(stored);
        partial.has_address = partial.address != nullptr;
        auto evidence = classifyDependencyAddressEvidence(stored);
        partial.source_kind = evidence.first;
        partial.proof = evidence.second;
      } else if (field_idx == 1) {
        if (const auto *len = dyn_cast<ConstantInt>(stored)) {
          partial.size = len->getZExtValue();
          partial.has_size = true;
        }
      } else if (field_idx == 2) {
        if (const auto *flags = dyn_cast<ConstantInt>(stored)) {
          partial.flags = flags->getZExtValue();
          partial.has_flags = true;
        }
      }
    }

    for (uint64_t i = 0; i < ndeps; ++i) {
      auto it = partials.find(i);
      if (it == partials.end() || !it->second.has_address) {
        continue;
      }
      Dependency dep;
      dep.address = it->second.address;
      dep.size = it->second.has_size ? it->second.size : 0;
      dep.type =
          decodeDependType(it->second.has_flags ? it->second.flags : 0x3);
      dep.source_kind = it->second.source_kind;
      dep.proof =
          it->second.has_address ? it->second.proof : DependencyProof::Unknown;
      dep.canonical_base = canonicalizeDependencyAddress(
          dep.address, DL, dep.offset, dep.has_precise_offset);
      deps.push_back(dep);
    }
  }

  return deps;
}

const CallBase *OpenMPSemantics::findTaskAllocCall(const Value *task_value) const {
  if (!task_value) {
    return nullptr;
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  worklist.push_back(task_value);

  auto isTaskAllocCall = [](const Value *value) -> const CallBase * {
    const auto *cb = dyn_cast<CallBase>(value);
    if (!cb) {
      return nullptr;
    }
    const Function *callee = cb->getCalledFunction();
    return callee && OpenMPModel::isTaskAlloc(callee->getName()) ? cb : nullptr;
  };

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    current = current->stripPointerCasts();
    if (const CallBase *alloc = isTaskAllocCall(current)) {
      return alloc;
    }
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
    if (const auto *gep = dyn_cast<GEPOperator>(current)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }

    for (const User *user : current->users()) {
      if (const auto *store = dyn_cast<StoreInst>(user)) {
        if (store->getPointerOperand()->stripPointerCasts() == current) {
          worklist.push_back(store->getValueOperand());
        }
      }
    }
  }

  return nullptr;
}

const Value *OpenMPSemantics::canonicalizeTaskHandle(const Value *task_value) const {
  if (!task_value) {
    return nullptr;
  }

  task_value = task_value->stripPointerCasts();
  if (const CallBase *task_alloc = findTaskAllocCall(task_value)) {
    return task_alloc;
  }

  if (const Value *underlying = getUnderlyingObject(task_value)) {
    return underlying->stripPointerCasts();
  }
  return task_value;
}

const Function *
OpenMPSemantics::extractTaskFunction(const CallBase *task_call) {
  if (!task_call || task_call->arg_size() < 3) {
    return nullptr;
  }

  if (const Function *callee = task_call->getCalledFunction()) {
    if (callee->hasName() && callee->getName().equals("GOMP_task") &&
        task_call->arg_size() >= 1) {
      if (const auto *direct =
              dyn_cast<Function>(task_call->getArgOperand(0)->stripPointerCasts())) {
        return direct;
      }
    }
  }

  const Value *task_arg = task_call->getArgOperand(2)->stripPointerCasts();
  if (const auto *direct = dyn_cast<Function>(task_arg)) {
    return direct;
  }

  if (const CallBase *task_alloc = findTaskAllocCall(task_arg)) {
    if (task_alloc->arg_size() >= 6) {
      const Value *entry = task_alloc->getArgOperand(5)->stripPointerCasts();
      if (const auto *direct = dyn_cast<Function>(entry)) {
        return direct;
      }
    }
  }

  const Value *task_base = getUnderlyingObject(task_arg);
  if (!task_base) {
    task_base = task_arg;
  }

  auto tryStoredFunction = [&](const Value *value) -> const Function * {
    if (!value) {
      return nullptr;
    }
    value = value->stripPointerCasts();
    if (const auto *func = dyn_cast<Function>(value)) {
      return func;
    }
    if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
      if (ce->isCast()) {
        return dyn_cast<Function>(ce->getOperand(0)->stripPointerCasts());
      }
    }
    return nullptr;
  };

  auto pointerTargetsTask = [&](const Value *ptr) {
    if (!ptr) {
      return false;
    }
    ptr = ptr->stripPointerCasts();
    if (ptr == task_arg || ptr == task_base) {
      return true;
    }
    if (const Value *underlying = getUnderlyingObject(ptr)) {
      return underlying->stripPointerCasts() == task_base;
    }
    if (const auto *gep = dyn_cast<GEPOperator>(ptr)) {
      return getUnderlyingObject(gep->getPointerOperand()) == task_base;
    }
    return false;
  };

  const Function *parent = task_call->getFunction();
  if (!parent) {
    return nullptr;
  }
  for (const Instruction &inst : instructions(parent)) {
    const auto *store = dyn_cast<StoreInst>(&inst);
    if (!store || !pointerTargetsTask(store->getPointerOperand())) {
      continue;
    }
    if (const Function *stored = tryStoredFunction(store->getValueOperand())) {
      return stored;
    }
  }
  return nullptr;
}

void OpenMPSemantics::applyTaskExecutionHints(Task &task,
                                              const CallBase *task_call) {
  if (!task_call) {
    return;
  }

  const Function *callee = task_call->getCalledFunction();
  if (callee && callee->hasName() &&
      callee->getName().equals("__kmpc_omp_task_begin_if0")) {
    task.execution_mode = TaskExecutionMode::Included;
  }

  const CallBase *task_alloc = findTaskAllocCall(task_call->getArgOperand(2));
  if (!task_alloc || task_alloc->arg_size() < 3) {
    return;
  }

  const auto *flags = dyn_cast<ConstantInt>(task_alloc->getArgOperand(2));
  if (!flags) {
    ++m_deferred_reason_counts["omp_task_flags_unresolved"];
    return;
  }

  const uint64_t value = flags->getZExtValue();
  if ((value & kLibompTaskMergedIf0Mask) != 0) {
    task.execution_mode = TaskExecutionMode::Included;
  }
  task.is_final = (value & kLibompTaskFinalMask) != 0;
  task.is_detached = (value & kLibompTaskDetachableMask) != 0 ||
                     (value & kLibompTaskProxyMask) != 0;
  task.is_untied = (value & kLibompTaskTiednessMask) == 0;

  if (task.is_detached) {
    task.execution_mode = TaskExecutionMode::Detached;
  } else if (task.is_final) {
    task.execution_mode = TaskExecutionMode::Final;
  } else if (task.is_untied) {
    task.execution_mode = TaskExecutionMode::Untied;
  }
}

bool OpenMPSemantics::dependenciesConflict(const Dependency &d1,
                                           const Dependency &d2) const {
  return classifyDependencyConflict(d1, d2) == DependencyConflict::MustConflict;
}

DependencyConflict
OpenMPSemantics::classifyDependencyConflict(const Dependency &d1,
                                            const Dependency &d2) const {
  const DataLayout &DL = m_module.getDataLayout();
  int64_t offset1 = d1.offset;
  int64_t offset2 = d2.offset;
  bool precise1 = d1.has_precise_offset;
  bool precise2 = d2.has_precise_offset;
  const Value *base1 =
      d1.canonical_base
          ? d1.canonical_base
          : canonicalizeDependencyAddress(d1.address, DL, offset1, precise1);
  const Value *base2 =
      d2.canonical_base
          ? d2.canonical_base
          : canonicalizeDependencyAddress(d2.address, DL, offset2, precise2);

  if (!base1 || !base2) {
    return DependencyConflict::Unknown;
  }

  bool is_write1 =
      (d1.type == DependType::OUT || d1.type == DependType::INOUT ||
       d1.type == DependType::MUTEXINOUTSET);
  bool is_write2 =
      (d2.type == DependType::OUT || d2.type == DependType::INOUT ||
       d2.type == DependType::MUTEXINOUTSET);

  if (base1 != base2) {
    if ((is_write1 || is_write2) &&
        (d1.proof != DependencyProof::Definite ||
         d2.proof != DependencyProof::Definite ||
         d1.source_kind != DependencySourceKind::DirectAddress ||
         d2.source_kind != DependencySourceKind::DirectAddress)) {
      ++m_deferred_imprecise_conflict_count;
      ++m_deferred_reason_counts["omp_depend_distinct_base_may_alias"];
      return DependencyConflict::MayConflict;
    }
    return DependencyConflict::NoConflict;
  }

  if (!(is_write1 || is_write2)) {
    return DependencyConflict::NoConflict;
  }

  if (precise1 && precise2 && d1.size != 0 && d2.size != 0) {
    uint64_t begin1 = static_cast<uint64_t>(offset1);
    uint64_t begin2 = static_cast<uint64_t>(offset2);
    uint64_t end1 = begin1 + d1.size;
    uint64_t end2 = begin2 + d2.size;
    return begin1 < end2 && begin2 < end1 ? DependencyConflict::MustConflict
                                          : DependencyConflict::NoConflict;
  }

  if (stripValue(d1.address) &&
      stripValue(d1.address) == stripValue(d2.address)) {
    return DependencyConflict::MustConflict;
  }

  ++m_deferred_imprecise_conflict_count;
  ++m_deferred_reason_counts["omp_depend_may_conflict"];
  return DependencyConflict::MayConflict;
}

bool OpenMPSemantics::isMutexLikeExclusion(const Dependency &d1,
                                           const Dependency &d2) const {
  return d1.type == DependType::MUTEXINOUTSET ||
         d2.type == DependType::MUTEXINOUTSET;
}

} // namespace OpenMP
