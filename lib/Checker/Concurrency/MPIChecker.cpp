#include "Checker/Concurrency/MPIChecker.h"

using namespace llvm;

namespace concurrency {

MPIChecker::MPIChecker(Module &module, mpi::MPIAnalysis *analysis)
    : m_module(module), m_analysis(analysis) {}

void MPIChecker::ensureAnalysis() {
  if (m_analysis) {
    return;
  }
  m_ownedAnalysis = std::make_unique<mpi::MPIAnalysis>(m_module);
  m_ownedAnalysis->runAnalysis();
  m_analysis = m_ownedAnalysis.get();
}

std::vector<ConcurrencyBugReport> MPIChecker::checkMPIBugs() {
  ensureAnalysis();

  std::vector<ConcurrencyBugReport> reports;
  if (!m_analysis) {
    return reports;
  }

  const auto &results = m_analysis->getResults();

  for (const auto &req : results.orphaned_requests) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_ORPHANED_REQUEST,
        "MPI non-blocking request may never be completed",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(req.issue_inst, "Non-blocking MPI request is issued here "
                                   "without a matching completion");
    reports.push_back(std::move(report));
  }

  for (const auto &pair : results.potential_deadlocks) {
    ConcurrencyBugReport report(ConcurrencyBugType::MPI_DEADLOCK,
                                "Potential MPI blocking communication deadlock",
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    report.addStep(
        pair.first,
        "Blocking communication participating in a potential deadlock cycle");
    report.addStep(
        pair.second,
        "Another blocking communication may wait cyclically with the first");
    reports.push_back(std::move(report));
  }

  for (const auto &pair : results.mismatched_collectives) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_COLLECTIVE_MISMATCH,
        "Mismatched MPI collective operations across processes",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(pair.first.inst,
                   "Collective call is incompatible with a peer collective");
    report.addStep(pair.second.inst,
                   "Conflicting collective call observed here");
    reports.push_back(std::move(report));
  }

  for (const Instruction *inst : results.conditional_collectives) {
    ConcurrencyBugReport report(ConcurrencyBugType::MPI_CONDITIONAL_COLLECTIVE,
                                "MPI collective may be executed conditionally "
                                "by only a subset of ranks",
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    report.addStep(
        inst,
        "Collective call appears rank-guarded or control-dependent on rank");
    reports.push_back(std::move(report));
  }

  for (const auto &op : results.unsynchronized_rma) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_UNSYNC_RMA,
        "MPI RMA operation may execute without a proven synchronization epoch",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(op.inst,
                   "RMA access occurs here without recognized synchronization");
    reports.push_back(std::move(report));
  }

  for (const auto &pair : results.rma_races) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_RMA_RACE, "Potential MPI RMA data race",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(pair.first.inst,
                   "First conflicting RMA operation occurs here");
    report.addStep(pair.second.inst,
                   "Second conflicting RMA operation occurs here");
    reports.push_back(std::move(report));
  }

  for (const auto *window : results.leaked_windows) {
    (void)window;
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_WINDOW_LEAK, "MPI RMA window may be leaked",
        BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.double_finalize) {
    ConcurrencyBugReport report(ConcurrencyBugType::MPI_DOUBLE_FINALIZE,
                                "Multiple MPI_Finalize calls detected",
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    report.addStep(inst, "Second MPI_Finalize call detected");
    reports.push_back(std::move(report));
  }

  if (results.missing_finalize) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_MISSING_FINALIZE,
        "MPI_Finalize may not be called before program exits",
        BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
    report.addStep(nullptr,
                   "MPI_Init was called but MPI_Finalize was not found");
    reports.push_back(std::move(report));
  }

  for (const auto &pair : results.tag_mismatches) {
    ConcurrencyBugReport report(ConcurrencyBugType::MPI_TAG_MISMATCH,
                                "MPI send and receive have mismatched tags",
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    report.addStep(pair.first, "Send operation with tag value");
    report.addStep(pair.second, "Receive operation with different tag");
    reports.push_back(std::move(report));
  }

  for (const auto &pair : results.count_datatype_mismatches) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_COUNT_DATATYPE_MISMATCH,
        "MPI send and receive have mismatched count or datatype",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(pair.first, "Send operation");
    report.addStep(pair.second,
                   "Receive operation with incompatible parameters");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.rank_out_of_bounds) {
    ConcurrencyBugReport report(ConcurrencyBugType::MPI_RANK_OUT_OF_BOUNDS,
                                "MPI operation may use invalid rank (negative)",
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    report.addStep(inst, "Rank value is negative or invalid");
    reports.push_back(std::move(report));
  }

  for (const auto &req : results.persistent_request_leaks) {
    (void)req;
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_PERSISTENT_REQUEST_LEAK,
        "MPI persistent request may not be properly completed",
        BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
    report.addStep(nullptr,
                   "Persistent request template created but never started");
    reports.push_back(std::move(report));
  }

  for (const auto &pair : results.wrong_root_ranks) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_WRONG_ROOT_RANK,
        "MPI collective operations use inconsistent root ranks",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(pair.first.inst, "Collective call with one root rank");
    report.addStep(pair.second.inst,
                   "Conflicting collective call with different root");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.cancel_without_wait) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_CANCEL_WITHOUT_WAIT,
        "MPI_Cancel may be called without subsequent wait/test",
        BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
    report.addStep(inst, "MPI_Cancel called without guaranteed completion");
    reports.push_back(std::move(report));
  }

  for (const auto &pair : results.buffer_overlaps) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_BUFFER_OVERLAP,
        "MPI_Sendrecv may use overlapping send and receive buffers",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(pair.first,
                   "Sendrecv operation with same buffer for send and receive");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.wildcard_in_collective) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_WILDCARD_IN_COLLECTIVE,
        "MPI collective operation may use wildcard source rank",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(inst, "Collective with wildcard source rank");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.in_place_conflicts) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_IN_PLACE_CONFLICT,
        "MPI_IN_PLACE may be used incorrectly in collective operation",
        BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
    report.addStep(inst, "Potential incorrect MPI_IN_PLACE usage");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.null_handles) {
    ConcurrencyBugReport report(ConcurrencyBugType::MPI_NULL_HANDLES,
                                "MPI operation may use NULL handle incorrectly",
                                BugDescription::BI_MEDIUM,
                                BugDescription::BC_WARNING);
    report.addStep(inst, "NULL handle used in MPI operation");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.negative_root) {
    ConcurrencyBugReport report(ConcurrencyBugType::MPI_ROOT_NEGATIVE,
                                "MPI collective has negative root rank",
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    report.addStep(inst, "Negative root rank in bcast/gather/reduce");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.invalid_tags) {
    ConcurrencyBugReport report(ConcurrencyBugType::MPI_INVALID_TAG,
                                "MPI operation uses an invalid tag value",
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    report.addStep(inst, "Tag value is negative and not MPI_ANY_TAG");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.invalid_ranks) {
    ConcurrencyBugReport report(ConcurrencyBugType::MPI_INVALID_RANK,
                                "MPI operation uses an invalid rank value",
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    report.addStep(inst,
                   "Rank value is invalid and not an allowed MPI wildcard");
    reports.push_back(std::move(report));
  }

  for (const auto &pair : results.type_size_mismatches) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_TYPE_SIZE_MISMATCH,
        "MPI send and receive payload sizes may be incompatible",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(pair.first, "Send operation payload size");
    report.addStep(pair.second, "Receive operation payload size");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.destroy_null_comm) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_DESTROY_NULL_COMM,
        "MPI_Comm_free may be called with a null communicator",
        BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
    report.addStep(inst, "Communicator free on null-like handle");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.request_free_after_wait) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_REQUEST_FREE_AFTER_WAIT,
        "MPI_Request_free may be called after request completion",
        BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
    report.addStep(inst,
                   "Request free observed after wait/test completion state");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.in_place_wrong_op) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_IN_PLACE_WRONG_OP,
        "MPI_IN_PLACE may be used on an unsupported collective operand",
        BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
    report.addStep(inst,
                   "Collective send buffer uses MPI_IN_PLACE where disallowed");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.invalid_rma_transitions) {
    ConcurrencyBugReport report(ConcurrencyBugType::MPI_INVALID_RMA_TRANSITION,
                                "MPI RMA epoch transition is invalid",
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    report.addStep(inst, "RMA sync call violates epoch-state transition rules");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.use_after_free_windows) {
    ConcurrencyBugReport report(ConcurrencyBugType::MPI_USE_AFTER_FREE_WINDOW,
                                "MPI RMA operation may use a freed window",
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    report.addStep(inst, "Window handle used after MPI_Win_free");
    reports.push_back(std::move(report));
  }

  for (const auto *inst : results.double_window_free) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_DOUBLE_WINDOW_FREE,
        "MPI_Win_free may be called multiple times on the same window",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(inst, "Repeated window free detected");
    reports.push_back(std::move(report));
  }

  return reports;
}

} // namespace concurrency
