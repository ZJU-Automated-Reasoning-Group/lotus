#ifndef CONCURRENCY_BUG_REPORT_H
#define CONCURRENCY_BUG_REPORT_H

#include "Checker/Report/BugTypes.h"

#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>

namespace concurrency {

enum class ConcurrencyBugType {
  DATA_RACE,
  DEADLOCK,
  ATOMICITY_VIOLATION,
  LOCK_MISMATCH,
  COND_VAR_MISUSE,
  OPENMP_TASKGROUP_MISMATCH,
  OPENMP_ATOMIC_MISMATCH,
  OPENMP_PARTIAL_SYNC,
  OPENMP_DETACHED_TASK_LEAK,
  OPENMP_NESTED_SINGLE,
  OPENMP_NOWAIT_MISSING_BARRIER,
  OPENMP_REDUCTION_ERROR,
  OPENMP_INCORRECT_NUMTHREADS,
  OPENMP_MISSING_FLUSH,
  MPI_ORPHANED_REQUEST,
  MPI_DEADLOCK,
  MPI_COLLECTIVE_MISMATCH,
  MPI_CONDITIONAL_COLLECTIVE,
  MPI_UNSYNC_RMA,
  MPI_RMA_RACE,
  MPI_WINDOW_LEAK,
  MPI_DOUBLE_FINALIZE,
  MPI_TAG_MISMATCH,
  MPI_COUNT_DATATYPE_MISMATCH,
  MPI_RANK_OUT_OF_BOUNDS,
  MPI_MISSING_FINALIZE,
  MPI_PERSISTENT_REQUEST_LEAK,
  MPI_WRONG_ROOT_RANK,
  MPI_CANCEL_WITHOUT_WAIT,
  MPI_BUFFER_OVERLAP,
  MPI_WILDCARD_IN_COLLECTIVE,
  OMP_TASKWAIT_MISSING,
  OMP_NESTED_PARALLEL_DISABLED,
  MPI_IN_PLACE_CONFLICT,
  MPI_NULL_HANDLES,
  MPI_ROOT_NEGATIVE,
  OMP_SHARED_PRIVATE_CONFLICT,
  OMP_IF_FALSE_PARALLEL,
  OMP_ORDERED_DEPENDENCY,
  MPI_INVALID_TAG,
  MPI_INVALID_RANK,
  MPI_TYPE_SIZE_MISMATCH,
  OMP_LASTPRIVATE_MISSING,
  OMP_COPYIN_NOT_SHARED,
  OMP_BARRIER_IN_CRITICAL,
  MPI_DESTROY_NULL_COMM,
  MPI_REQUEST_FREE_AFTER_WAIT,
  MPI_IN_PLACE_WRONG_OP,
  MPI_INVALID_RMA_TRANSITION,
  MPI_USE_AFTER_FREE_WINDOW,
  MPI_DOUBLE_WINDOW_FREE,
  OMP_PRIVATE_IN_LOOP,
  OMP_MISSING_SCHEDULE
};

struct ConcurrencyBugStep {
  const llvm::Instruction *instruction;
  std::string description;

  ConcurrencyBugStep(const llvm::Instruction *inst, const std::string &desc)
      : instruction(inst), description(desc) {}
};

/**
 * Data-race-specific annotation for witness/SARIF (Ultimate-style).
 * Access path and read/write flag for each conflicting access.
 */
struct DataRaceInfo {
  std::string accessPath1;
  std::string accessPath2;
  bool write1 = false;
  bool write2 = false;
  std::string sharedLocation; // optional abstract location description
};

struct ConcurrencyBugReport {
  ConcurrencyBugType bugType;
  std::vector<ConcurrencyBugStep> steps;
  std::string description;
  BugDescription::BugImportance importance;
  BugDescription::BugClassification classification;

  std::unique_ptr<DataRaceInfo> dataRaceInfo;

  ConcurrencyBugReport(
      ConcurrencyBugType type, const std::string &desc,
      BugDescription::BugImportance imp = BugDescription::BI_HIGH,
      BugDescription::BugClassification cls = BugDescription::BC_ERROR)
      : bugType(type), description(desc), importance(imp), classification(cls) {
  }

  ConcurrencyBugReport(const ConcurrencyBugReport &other)
      : bugType(other.bugType), steps(other.steps),
        description(other.description), importance(other.importance),
        classification(other.classification) {
    if (other.dataRaceInfo)
      dataRaceInfo = std::make_unique<DataRaceInfo>(*other.dataRaceInfo);
  }
  ConcurrencyBugReport(ConcurrencyBugReport &&) noexcept = default;
  ConcurrencyBugReport &operator=(const ConcurrencyBugReport &other) {
    if (this != &other) {
      bugType = other.bugType;
      steps = other.steps;
      description = other.description;
      importance = other.importance;
      classification = other.classification;
      dataRaceInfo = other.dataRaceInfo
                         ? std::make_unique<DataRaceInfo>(*other.dataRaceInfo)
                         : nullptr;
    }
    return *this;
  }
  ConcurrencyBugReport &operator=(ConcurrencyBugReport &&) noexcept = default;

  void addStep(const llvm::Instruction *inst, const std::string &desc) {
    steps.emplace_back(inst, desc);
  }

  void setDataRaceInfo(std::string ap1, std::string ap2, bool w1, bool w2,
                       std::string shared = "") {
    dataRaceInfo = std::make_unique<DataRaceInfo>();
    dataRaceInfo->accessPath1 = std::move(ap1);
    dataRaceInfo->accessPath2 = std::move(ap2);
    dataRaceInfo->write1 = w1;
    dataRaceInfo->write2 = w2;
    dataRaceInfo->sharedLocation = std::move(shared);
  }
};

} // namespace concurrency

#endif // CONCURRENCY_BUG_REPORT_H
