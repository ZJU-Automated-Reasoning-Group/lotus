#include "Checker/Concurrency/OpenMPChecker.h"

#include <llvm/IR/InstIterator.h>

using namespace llvm;

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

bool isSharedLike(OpenMP::DataSharingAttribute attribute) {
  switch (attribute) {
  case OpenMP::DataSharingAttribute::Shared:
  case OpenMP::DataSharingAttribute::SharedNoModify:
  case OpenMP::DataSharingAttribute::Copyin:
  case OpenMP::DataSharingAttribute::Copyout:
  case OpenMP::DataSharingAttribute::Reduction:
    return true;
  default:
    return false;
  }
}

} // namespace

OpenMPChecker::OpenMPChecker(Module &module,
                             OpenMP::OpenMPTaskGraph *task_graph,
                             ThreadAPI *thread_api)
    : m_module(module), m_taskGraph(task_graph), m_threadAPI(thread_api) {}

void OpenMPChecker::ensureTaskGraph() {
  if (m_taskGraph) {
    return;
  }
  m_ownedTaskGraph = std::make_unique<OpenMP::OpenMPTaskGraph>(m_module);
  m_ownedTaskGraph->analyze();
  m_taskGraph = m_ownedTaskGraph.get();
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkPartialTaskSynchronization() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_taskGraph) {
    return reports;
  }

  for (const auto &boundary : m_taskGraph->getWaitBoundaries()) {
    if (!boundary.is_partial_wait) {
      continue;
    }

    ConcurrencyBugReport report(ConcurrencyBugType::OPENMP_PARTIAL_SYNC,
                                "OpenMP task wait with dependencies may leave "
                                "sibling tasks unsynchronized",
                                BugDescription::BI_MEDIUM,
                                BugDescription::BC_ERROR);
    report.addStep(boundary.inst,
                   "Selective __kmpc_omp_wait_deps boundary encountered here");
    reports.push_back(std::move(report));
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkTaskgroupStructure() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    int depth = 0;
    const Instruction *last_start = nullptr;
    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      auto type = m_threadAPI->getType(call);
      if (type == ThreadAPI::TD_OMP_TASKGROUP_START) {
        ++depth;
        last_start = &inst;
      } else if (type == ThreadAPI::TD_OMP_TASKGROUP_END) {
        if (depth == 0) {
          ConcurrencyBugReport report(
              ConcurrencyBugType::OPENMP_TASKGROUP_MISMATCH,
              "OpenMP taskgroup end has no matching start",
              BugDescription::BI_HIGH, BugDescription::BC_ERROR);
          report.addStep(
              &inst,
              "Encountered __kmpc_end_taskgroup without active taskgroup");
          reports.push_back(std::move(report));
        } else {
          --depth;
        }
      }
    }

    if (depth > 0 && last_start) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OPENMP_TASKGROUP_MISMATCH,
          "OpenMP taskgroup start may not be properly closed",
          BugDescription::BI_HIGH, BugDescription::BC_ERROR);
      report.addStep(last_start, "Taskgroup starts here but no matching "
                                 "__kmpc_end_taskgroup was found");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkAtomicRegionStructure() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    int depth = 0;
    const Instruction *last_start = nullptr;
    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      auto type = m_threadAPI->getType(call);
      if (type == ThreadAPI::TD_OMP_ATOMIC_START) {
        ++depth;
        last_start = &inst;
      } else if (type == ThreadAPI::TD_OMP_ATOMIC_END) {
        if (depth == 0) {
          ConcurrencyBugReport report(
              ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH,
              "OpenMP atomic end has no matching start",
              BugDescription::BI_HIGH, BugDescription::BC_ERROR);
          report.addStep(
              &inst,
              "Encountered __kmpc_atomic_end without active atomic region");
          reports.push_back(std::move(report));
        } else {
          --depth;
        }
      }
    }

    if (depth > 0 && last_start) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH,
          "OpenMP atomic region may not be properly closed",
          BugDescription::BI_HIGH, BugDescription::BC_ERROR);
      report.addStep(last_start, "Atomic region starts here but no matching "
                                 "__kmpc_atomic_end was found");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkDetachedTaskLeak() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_taskGraph) {
    return reports;
  }

  const auto &summary = m_taskGraph->getSummary();
  if (summary.detached_task_count != 0 &&
      summary.detached_task_count > summary.detach_completion_count) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::OPENMP_DETACHED_TASK_LEAK,
        "OpenMP detached task may not be properly completed",
        BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
    report.addStep(
        nullptr,
        "Detached tasks: " + std::to_string(summary.detached_task_count) +
            ", completions: " +
            std::to_string(summary.detach_completion_count));
    reports.push_back(std::move(report));
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkNestedSingle() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    int single_depth = 0;
    const Instruction *outer_single = nullptr;
    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      auto type = m_threadAPI->getType(call);
      if (type == ThreadAPI::TD_OMP_SINGLE_START) {
        if (single_depth > 0) {
          ConcurrencyBugReport report(ConcurrencyBugType::OPENMP_NESTED_SINGLE,
                                      "Nested OpenMP single regions detected "
                                      "without proper synchronization",
                                      BugDescription::BI_MEDIUM,
                                      BugDescription::BC_ERROR);
          report.addStep(outer_single, "Outer single region");
          report.addStep(
              &inst, "Inner single region - may cause implicit barrier issues");
          reports.push_back(std::move(report));
        }
        ++single_depth;
        if (!outer_single) {
          outer_single = &inst;
        }
      } else if (type == ThreadAPI::TD_OMP_SINGLE_END) {
        if (single_depth > 0) {
          --single_depth;
          if (single_depth == 0) {
            outer_single = nullptr;
          }
        }
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkNowaitMissingBarrier() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    const Instruction *last_workshare = nullptr;
    bool found_flush_after_workshare = false;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }
      StringRef name = callee->getName();

      if (name.contains("single_start") || name.contains("for_static_fini") ||
          name.contains("sections_next")) {
        last_workshare = &inst;
        found_flush_after_workshare = false;
        continue;
      }

      if (name.contains("flush")) {
        if (last_workshare) {
          found_flush_after_workshare = true;
        }
        continue;
      }

      if (name.contains("barrier")) {
        if (found_flush_after_workshare && last_workshare) {
          ConcurrencyBugReport report(
              ConcurrencyBugType::OPENMP_NOWAIT_MISSING_BARRIER,
              "Potential race between workshare region and barrier",
              BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
          report.addStep(last_workshare, "Workshare region found");
          report.addStep(&inst, "Subsequent flush/barrier found");
          reports.push_back(std::move(report));
        }
        last_workshare = nullptr;
        found_flush_after_workshare = false;
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkMissingFlush() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    bool has_critical_section = false;
    bool has_flush = false;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }
      StringRef name = callee->getName();

      if (name.contains("__kmpc_critical") ||
          name.contains("__kmpc_end_critical") ||
          name.contains("omp_set_lock") || name.contains("omp_unset_lock")) {
        has_critical_section = true;
      }

      if (name.contains("__kmpc_flush") ||
          name.contains("omp_get_thread_num")) {
        has_flush = true;
      }
    }

    if (has_critical_section && !has_flush) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OPENMP_MISSING_FLUSH,
          "OpenMP critical section without flush may cause stale data",
          BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
      report.addStep(nullptr, "Critical section or lock used without flush");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkIncorrectNumThreads() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }
      StringRef name = callee->getName();

      if (name.contains("__kmpc_fork_call")) {
        if (call->arg_size() >= 3) {
          const Value *num_threads_arg = call->getArgOperand(2);
          if (const auto *call_inst = dyn_cast<CallInst>(num_threads_arg)) {
            const Function *inner_callee = call_inst->getCalledFunction();
            if (inner_callee &&
                (inner_callee->getName().contains("omp_get_num_threads") ||
                 inner_callee->getName().contains("omp_get_max_threads"))) {
              ConcurrencyBugReport report(
                  ConcurrencyBugType::OPENMP_INCORRECT_NUMTHREADS,
                  "Incorrect use of omp_get_num_threads in num_threads clause",
                  BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
              report.addStep(
                  &inst, "omp_get_num_threads called in num_threads clause");
              reports.push_back(std::move(report));
            }
          }
        }
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkReductionError() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    std::set<const Instruction *> reduce_starts;
    std::set<const Instruction *> reduce_ends;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }
      StringRef name = callee->getName();

      if (name.contains("__kmpc_reduce") && !name.contains("end")) {
        reduce_starts.insert(&inst);
      }
      if (name.contains("__kmpc_end_reduce")) {
        reduce_ends.insert(&inst);
      }
    }

    if (reduce_starts.size() != reduce_ends.size()) {
      for (const auto *start : reduce_starts) {
        ConcurrencyBugReport report(
            ConcurrencyBugType::OPENMP_REDUCTION_ERROR,
            "OpenMP reduction region may be improperly nested",
            BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
        report.addStep(start, "Reduction start without matching end");
        reports.push_back(std::move(report));
        break;
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkTaskwaitMissing() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    bool has_task_create = false;
    const Instruction *task_create_inst = nullptr;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }
      StringRef name = callee->getName();

      if (name.contains("__kmpc_omp_task") && !name.contains("wait")) {
        has_task_create = true;
        task_create_inst = &inst;
      }

      if (name.contains("__kmpc_omp_taskwait") ||
          name.contains("__kmpc_omp_wait_deps")) {
        has_task_create = false;
        task_create_inst = nullptr;
      }

      if (isa<ReturnInst>(&inst) && has_task_create && task_create_inst) {
        ConcurrencyBugReport report(
            ConcurrencyBugType::OMP_TASKWAIT_MISSING,
            "OpenMP task created without taskwait before function returns",
            BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
        report.addStep(task_create_inst,
                       "Task created here without taskwait before return");
        reports.push_back(std::move(report));
        break;
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkNestedParallelDisabled() const {
  std::vector<ConcurrencyBugReport> reports;

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }
      StringRef name = callee->getName();

      if (name.contains("__kmpc_fork_call")) {
        for (const User *U : func.users()) {
          if (const auto *call_inst = dyn_cast<CallBase>(U)) {
            const Function *inner_callee = call_inst->getCalledFunction();
            if (inner_callee &&
                inner_callee->getName().contains("__kmpc_fork_call")) {
              ConcurrencyBugReport report(
                  ConcurrencyBugType::OMP_NESTED_PARALLEL_DISABLED,
                  "Nested parallel region may not execute - "
                  "OMP_MAX_ACTIVE_LEVELS might be 1",
                  BugDescription::BI_LOW, BugDescription::BC_WARNING);
              report.addStep(&inst, "Nested parallel region detected");
              reports.push_back(std::move(report));
              break;
            }
          }
        }
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkSharedPrivateConflict() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    const Instruction *copyprivate_inst = nullptr;
    const Instruction *task_inst = nullptr;
    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }

      const ThreadAPI::TD_TYPE type = m_threadAPI->getType(callee);
      if (type == ThreadAPI::TD_OMP_TASK ||
          type == ThreadAPI::TD_OMP_TASK_WITH_DEPS ||
          type == ThreadAPI::TD_OMP_TASKLOOP) {
        task_inst = &inst;
      }
      if (callee->getName().contains("copyprivate")) {
        copyprivate_inst = &inst;
      }
    }

    if (copyprivate_inst && task_inst) {
      bool semantic_conflict = true;
      if (m_taskGraph) {
        if (const OpenMP::Task *task = m_taskGraph->getTaskForCreate(task_inst)) {
          bool saw_private_like = false;
          bool saw_shared_like = false;
          for (const OpenMP::DataSharingEntry &entry : task->data_sharing_entries) {
            saw_private_like = saw_private_like || isPrivateLike(entry.attribute);
            saw_shared_like = saw_shared_like || isSharedLike(entry.attribute);
          }
          if (!task->data_sharing_entries.empty()) {
            semantic_conflict = saw_private_like && saw_shared_like;
          }
        }
      }
      if (!semantic_conflict) {
        continue;
      }
      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_SHARED_PRIVATE_CONFLICT,
          "OpenMP copyprivate state crosses tasking boundary; shared/private "
          "intent may be inconsistent",
          BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
      report.addStep(copyprivate_inst, "copyprivate-style operation here");
      report.addStep(task_inst, "tasking operation in same region");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkIfFalseParallel() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    const Instruction *set_num_threads_inst = nullptr;
    const Instruction *fork_inst = nullptr;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }

      if (callee->getName().contains("omp_set_num_threads") &&
          call->arg_size() >= 1) {
        if (const auto *ci = dyn_cast<ConstantInt>(call->getArgOperand(0))) {
          if (ci->getSExtValue() <= 1) {
            set_num_threads_inst = &inst;
          }
        }
      }

      if (m_threadAPI->getType(callee) == ThreadAPI::TD_FORK) {
        fork_inst = &inst;
      }
    }

    if (set_num_threads_inst && fork_inst) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_IF_FALSE_PARALLEL,
          "Parallel region may be serialized because thread count is forced to "
          "1",
          BugDescription::BI_LOW, BugDescription::BC_WARNING);
      report.addStep(set_num_threads_inst,
                     "omp_set_num_threads forces a single worker");
      report.addStep(fork_inst, "parallel region launch occurs here");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkOrderedDependency() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    size_t ordered_count = 0;
    size_t doacross_wait_count = 0;
    size_t doacross_submit_count = 0;
    const Instruction *first_ordered = nullptr;
    const Instruction *first_wait = nullptr;
    const Instruction *first_submit = nullptr;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }

      const auto type = m_threadAPI->getType(callee);
      if (type == ThreadAPI::TD_OMP_ORDERED_START) {
        ++ordered_count;
        if (!first_ordered) {
          first_ordered = &inst;
        }
      } else if (type == ThreadAPI::TD_OMP_DOACROSS_WAIT) {
        ++doacross_wait_count;
        if (!first_wait) {
          first_wait = &inst;
        }
      } else if (type == ThreadAPI::TD_OMP_DOACROSS_SUBMIT) {
        ++doacross_submit_count;
        if (!first_submit) {
          first_submit = &inst;
        }
      }
    }

    const bool missing_doacross = ordered_count > 0 &&
                                  doacross_wait_count == 0 &&
                                  doacross_submit_count == 0;
    const bool imbalance = doacross_wait_count != doacross_submit_count;
    if (missing_doacross || imbalance) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_ORDERED_DEPENDENCY,
          "OpenMP ordered dependency protocol appears incomplete",
          BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
      if (first_ordered) {
        report.addStep(first_ordered, "ordered region starts here");
      }
      if (missing_doacross) {
        report.addStep(nullptr,
                       "no doacross wait/submit operations were observed");
      }
      if (imbalance) {
        if (first_wait) {
          report.addStep(first_wait, "doacross wait observed");
        }
        if (first_submit) {
          report.addStep(first_submit, "doacross submit observed");
        }
      }
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkLastprivateMissing() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    bool saw_loop_init = false;
    bool saw_loop_fini = false;
    const Instruction *loop_init_inst = nullptr;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }

      const auto type = m_threadAPI->getType(callee);
      if (type == ThreadAPI::TD_OMP_FOR_STATIC_INIT ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_INIT) {
        saw_loop_init = true;
        if (!loop_init_inst) {
          loop_init_inst = &inst;
        }
      }
      if (type == ThreadAPI::TD_OMP_FOR_STATIC_FINI ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_FINI) {
        saw_loop_fini = true;
      }
    }

    if (saw_loop_init && !saw_loop_fini && loop_init_inst) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_LASTPRIVATE_MISSING,
          "OpenMP worksharing loop init has no matching finalize; trailing "
          "lastprivate update may be skipped",
          BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
      report.addStep(loop_init_inst,
                     "loop worksharing initialization appears unmatched");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkCopyinNotShared() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    const Instruction *copyprivate_inst = nullptr;
    bool saw_single_region = false;
    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }

      if (m_threadAPI->getType(callee) == ThreadAPI::TD_OMP_SINGLE_START ||
          m_threadAPI->getType(callee) == ThreadAPI::TD_OMP_SINGLE_END) {
        saw_single_region = true;
      }
      if (callee->getName().contains("copyprivate")) {
        copyprivate_inst = &inst;
      }
    }

    if (copyprivate_inst && !saw_single_region) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_COPYIN_NOT_SHARED,
          "copyprivate/copyin-style operation appears outside single region",
          BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
      report.addStep(copyprivate_inst,
                     "copyprivate-like operation observed without single");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkBarrierInCritical() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    int critical_depth = 0;
    const Instruction *critical_entry = nullptr;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }

      const auto type = m_threadAPI->getType(callee);
      if (type == ThreadAPI::TD_ACQUIRE &&
          m_threadAPI->semanticTagStartsWith(callee, "critical")) {
        ++critical_depth;
        if (!critical_entry) {
          critical_entry = &inst;
        }
        continue;
      }

      if (type == ThreadAPI::TD_RELEASE &&
          m_threadAPI->semanticTagStartsWith(callee, "critical")) {
        if (critical_depth > 0) {
          --critical_depth;
          if (critical_depth == 0) {
            critical_entry = nullptr;
          }
        }
        continue;
      }

      if (type == ThreadAPI::TD_BAR_WAIT && critical_depth > 0) {
        ConcurrencyBugReport report(
            ConcurrencyBugType::OMP_BARRIER_IN_CRITICAL,
            "Barrier encountered in active critical region",
            BugDescription::BI_HIGH, BugDescription::BC_ERROR);
        if (critical_entry) {
          report.addStep(critical_entry, "critical region begins here");
        }
        report.addStep(&inst, "barrier inside critical may deadlock");
        reports.push_back(std::move(report));
        break;
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkPrivateInLoop() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    bool in_loop_region = false;
    const Instruction *loop_enter = nullptr;
    const Instruction *task_call_in_loop = nullptr;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }

      const auto type = m_threadAPI->getType(callee);
      if (type == ThreadAPI::TD_OMP_FOR_STATIC_INIT ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_INIT ||
          type == ThreadAPI::TD_OMP_LOOP_STATIC_INIT ||
          type == ThreadAPI::TD_OMP_LOOP_DYNAMIC_INIT ||
          type == ThreadAPI::TD_OMP_LOOP_GUIDANCE_INIT) {
        in_loop_region = true;
        if (!loop_enter) {
          loop_enter = &inst;
        }
        continue;
      }

      if (in_loop_region && (type == ThreadAPI::TD_OMP_TASK ||
                             type == ThreadAPI::TD_OMP_TASK_WITH_DEPS ||
                             type == ThreadAPI::TD_OMP_TASKLOOP)) {
        task_call_in_loop = &inst;
      }

      if (type == ThreadAPI::TD_OMP_FOR_STATIC_FINI ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_FINI) {
        in_loop_region = false;
      }
    }

    if (loop_enter && task_call_in_loop) {
      bool captures_loop_private = true;
      if (m_taskGraph) {
        if (const OpenMP::Task *task =
                m_taskGraph->getTaskForCreate(task_call_in_loop)) {
          if (!task->data_sharing_entries.empty()) {
            captures_loop_private = false;
            for (const OpenMP::DataSharingEntry &entry :
                 task->data_sharing_entries) {
              if (isPrivateLike(entry.attribute)) {
                captures_loop_private = true;
                break;
              }
            }
          }
        }
      }
      if (!captures_loop_private) {
        continue;
      }
      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_PRIVATE_IN_LOOP,
          "Tasking inside OpenMP loop may capture loop-private values "
          "unsafely",
          BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
      report.addStep(loop_enter, "OpenMP loop region starts here");
      report.addStep(task_call_in_loop, "tasking operation within loop region");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkMissingSchedule() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    bool has_static_loop = false;
    bool has_dynamic_loop = false;
    const Instruction *loop_inst = nullptr;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }

      const auto type = m_threadAPI->getType(callee);
      if (type == ThreadAPI::TD_OMP_FOR_STATIC_INIT) {
        has_static_loop = true;
        if (!loop_inst) {
          loop_inst = &inst;
        }
      }
      if (type == ThreadAPI::TD_OMP_FOR_DISPATCH_INIT) {
        has_dynamic_loop = true;
      }
    }

    if (has_static_loop && !has_dynamic_loop && loop_inst) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_MISSING_SCHEDULE,
          "OpenMP loop appears to rely on implicit default scheduling",
          BugDescription::BI_LOW, BugDescription::BC_WARNING);
      report.addStep(loop_inst,
                     "static loop init observed without explicit dispatch "
                     "runtime path");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkOpenMPBugs() {
  ensureTaskGraph();

  std::vector<ConcurrencyBugReport> reports;

  auto append = [&](std::vector<ConcurrencyBugReport> more) {
    reports.insert(reports.end(), std::make_move_iterator(more.begin()),
                   std::make_move_iterator(more.end()));
  };

  append(checkPartialTaskSynchronization());
  append(checkTaskgroupStructure());
  append(checkAtomicRegionStructure());
  append(checkDetachedTaskLeak());
  append(checkNestedSingle());
  append(checkNowaitMissingBarrier());
  append(checkMissingFlush());
  append(checkIncorrectNumThreads());
  append(checkReductionError());
  append(checkTaskwaitMissing());
  append(checkNestedParallelDisabled());
  append(checkSharedPrivateConflict());
  append(checkIfFalseParallel());
  append(checkOrderedDependency());
  append(checkLastprivateMissing());
  append(checkCopyinNotShared());
  append(checkBarrierInCritical());
  append(checkPrivateInLoop());
  append(checkMissingSchedule());

  return reports;
}

} // namespace concurrency
