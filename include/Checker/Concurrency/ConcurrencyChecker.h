/** @file ConcurrencyChecker.h @brief Aggregate concurrency checker driver
 * orchestrating multiple analyses. */
#ifndef CONCURRENCY_CHECKER_H
#define CONCURRENCY_CHECKER_H

#include "Checker/Concurrency/AtomicityChecker.h"
#include "Checker/Concurrency/CUDAChecker.h"
#include "Checker/Concurrency/ConcurrencyBugReport.h"
#include "Checker/Concurrency/ConditionVariableChecker.h"
#include "Checker/Concurrency/DataRaceChecker.h"
#include "Checker/Concurrency/DeadlockChecker.h"
#include "Checker/Concurrency/LockMismatchChecker.h"
#include "Checker/Concurrency/MPIChecker.h"
#include "Checker/Concurrency/OpenMPChecker.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"
#include "Concurrency/ConcurrencyFacade.h"
#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "Concurrency/MHP/HappensBeforeAnalysis.h"
#include "Concurrency/MHP/IMHPAnalysis.h"
#include "Concurrency/MHP/MHPAnalysis.h"
#include "Concurrency/MHP/StaticVectorClockMHP.h"
#include "Concurrency/MPI/MPIAnalysis.h"
#include "Concurrency/Memory/EscapeAnalysis.h"
#include "Concurrency/Memory/StaticThreadSharingAnalysis.h"
#include "Concurrency/OpenMP/OpenMPTaskGraph.h"
#include "Concurrency/Utils/ThreadLocalAnalysis.h"
#include "Concurrency/ValueFlow/WholeProgramSparseRefinement.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace concurrency {

/**
 * @brief Static checker for concurrency problems including data races,
 * deadlocks, and atomicity violations
 *
 * This checker uses MHP analysis and lock set analysis to detect:
 * - Data races: concurrent accesses to shared variables without proper
 * synchronization
 * - Deadlocks: potential cycles in lock acquisition order
 * - Atomicity violations: operations that should be atomic but may be
 * interleaved
 */
class ConcurrencyChecker {
public:
  enum class MHPBackendKind { Region, StaticVectorClock };

  explicit ConcurrencyChecker(llvm::Module &module);
  ~ConcurrencyChecker() = default;

  /**
   * @brief Run only the analyses required by the currently enabled checks
   * (Goblint-style config-driven activation). Call after enable*Check() and
   * before runChecks() or dumpAnalysisResults().
   */
  void runAnalyses();

  /**
   * @brief Run all concurrency checks and report bugs to BugReportMgr
   */
  void runChecks();

  /**
   * @brief Check for data races and report to BugReportMgr
   */
  void checkDataRaces();

  /**
   * @brief Check for deadlocks and report to BugReportMgr
   */
  void checkDeadlocks();

  /**
   * @brief Check for atomicity violations and report to BugReportMgr
   */
  void checkAtomicityViolations();

  /**
   * @brief Check for condition variable misuse and report to BugReportMgr
   */
  void checkConditionVariables();

  /**
   * @brief Check for lock mismatches and report to BugReportMgr
   */
  void checkLockMismatches();

  /**
   * @brief Run dedicated OpenMP checks and report to BugReportMgr
   */
  void checkOpenMPBugs();

  /**
   * @brief Run dedicated MPI checks and report to BugReportMgr
   */
  void checkMPIBugs();

  /**
   * @brief Run dedicated CUDA checks and report to BugReportMgr
   */
  void checkCUDABugs();

  /**
   * @brief Set alias analysis wrapper for better precision
   */
  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) {
    m_aliasAnalysis = aa;
    if (m_happensBeforeAnalysis)
      m_happensBeforeAnalysis->setAliasAnalysis(aa);
    if (m_locksetAnalysis)
      m_locksetAnalysis->setAliasAnalysis(aa);
    if (m_locksetAnalysisView &&
        m_locksetAnalysisView != m_locksetAnalysis.get())
      m_locksetAnalysisView->setAliasAnalysis(aa);
  }

  /**
   * @brief Enable/disable specific checks
   */
  void enableDataRaceCheck(bool enable) { m_checkDataRaces = enable; }
  void enableDeadlockCheck(bool enable) { m_checkDeadlocks = enable; }
  void enableAtomicityCheck(bool enable) {
    m_checkAtomicityViolations = enable;
  }
  void enableCondVarCheck(bool enable) { m_checkCondVars = enable; }
  void enableLockMismatchCheck(bool enable) { m_checkLockMismatches = enable; }
  void enableOpenMPCheck(bool enable) { m_checkOpenMP = enable; }
  void enableMPICheck(bool enable) { m_checkMPI = enable; }
  void enableCUDACheck(bool enable) { m_checkCUDA = enable; }
  void setMHPBackend(MHPBackendKind backend) { m_mhpBackend = backend; }
  void enableSparseFlowSensitiveRefinement(bool enable) {
    m_enableSparseFlowSensitiveRefinement = enable;
  }
  void enableMultiStageSlicing(bool enable) {
    m_enableMultiStageSlicing = enable;
    m_enableSparseFlowSensitiveRefinement |= enable;
  }
  void setSparseMemoryPartition(
      lotus::analysis::MemoryRegionPartitionStrategy strategy) {
    m_sparseMemoryPartition = strategy;
  }
  void setThreadContextLimit(std::size_t limit) {
    m_threadContextLimit = limit;
  }
  void
  setSparsePointsToSetBackend(lotus::alias::PointsToSetBackend backend) {
    m_sparsePointsToSetBackend = backend;
  }

  /**
   * @brief Get statistics about the analysis
   */
  struct Statistics {
    size_t totalInstructions;
    size_t mhpPairs;
    size_t locksAnalyzed;
    size_t dataRacesFound;
    size_t deadlocksFound;
    size_t atomicityViolationsFound;
    size_t condVarBugsFound;
    size_t lockMismatchesFound;
    size_t openMPBugsFound;
    size_t mpiBugsFound;
    size_t cudaBugsFound;
    size_t sparseInterferenceEdges;
    size_t sparsePointsToFacts;
    size_t sparseMemoryRegions;
    size_t sparseOriginalNodes;
    size_t sparseSlicedNodes;
    size_t sparsePreThreads;
    size_t sparseMainThreads;
    size_t sparseForkMemoryEdges;
    size_t sparseJoinMemoryEdges;
    size_t sparseHashConsedUniqueSets;
    size_t sparseHashConsedUnionCacheHits;
    OpenMP::OpenMPTaskGraph::AnalysisSummary openMPSummary;
    ConcurrencyFacade::MPISummary mpiSummary;
    ConcurrencyFacade::CUDASummary cudaSummary;
  };

  Statistics getStatistics() const { return m_stats; }

  /**
   * @brief Dump analysis results with source-level debug information
   *
   * This method dumps the results of fundamental concurrency analyses
   * (MHP, LockSet, Escape) at the source-code level using debug information.
   * Used in analysis-only mode to report facts without performing bug checking.
   *
   * @param os Output stream to write results to
   * @param jsonFormat If true, output in JSON format; otherwise, human-readable
   * text
   */
  void dumpAnalysisResults(llvm::raw_ostream &os,
                           bool jsonFormat = false) const;

private:
  llvm::Module &m_module;
  std::unique_ptr<mhp::IMHPAnalysis> m_mhpAnalysisStorage;
  mhp::IMHPAnalysis *m_mhpAnalysis = nullptr;
  std::unique_ptr<mhp::LockSetAnalysis> m_locksetAnalysis;
  mhp::LockSetAnalysis *m_locksetAnalysisView = nullptr;
  std::unique_ptr<lotus::EscapeAnalysis> m_escapeAnalysis;
  std::unique_ptr<ThreadLocal::ThreadLocalAnalysis> m_threadLocalAnalysis;
  std::unique_ptr<lotus::HappensBeforeAnalysis> m_happensBeforeAnalysis;
  std::unique_ptr<llvm::legacy::PassManager> m_staticThreadSharingPM;
  lotus::StaticThreadSharingAnalysis *m_staticThreadSharingAnalysis = nullptr;
  std::unique_ptr<OpenMP::OpenMPTaskGraph> m_openMPTaskGraph;
  std::unique_ptr<mpi::MPIAnalysis> m_mpiAnalysis;
  std::unique_ptr<cuda::CUDAAnalysis> m_cudaAnalysis;
  std::unique_ptr<lotus::analysis::WholeProgramSparseRefinement>
      m_sparseRefinement;
  lotus::AliasAnalysisWrapper *m_aliasAnalysis;
  ThreadAPI *m_threadAPI;

  // Specialized checker components
  std::unique_ptr<DataRaceChecker> m_dataRaceChecker;
  std::unique_ptr<DeadlockChecker> m_deadlockChecker;
  std::unique_ptr<AtomicityChecker> m_atomicityChecker;
  std::unique_ptr<ConditionVariableChecker> m_condVarChecker;
  std::unique_ptr<LockMismatchChecker> m_lockMismatchChecker;
  std::unique_ptr<OpenMPChecker> m_openMPChecker;
  std::unique_ptr<MPIChecker> m_mpiChecker;
  std::unique_ptr<CUDAChecker> m_cudaChecker;

  // Configuration
  bool m_checkDataRaces = true;
  bool m_checkDeadlocks = true;
  bool m_checkAtomicityViolations = true;
  bool m_checkCondVars = true;
  bool m_checkLockMismatches = true;
  bool m_checkOpenMP = true;
  bool m_checkMPI = true;
  bool m_checkCUDA = true;
  bool m_enableSparseFlowSensitiveRefinement = false;
  bool m_enableMultiStageSlicing = false;
  lotus::analysis::MemoryRegionPartitionStrategy m_sparseMemoryPartition =
      lotus::analysis::MemoryRegionPartitionStrategy::InterDisjoint;
  std::size_t m_threadContextLimit = 2;
  lotus::alias::PointsToSetBackend m_sparsePointsToSetBackend =
      lotus::alias::PointsToSetBackend::Mutable;
  MHPBackendKind m_mhpBackend = MHPBackendKind::Region;

  // Bug type IDs (registered with BugReportMgr)
  int m_dataRaceTypeId;
  int m_deadlockTypeId;
  int m_atomicityViolationTypeId;
  int m_condVarMisuseTypeId;
  int m_lockMismatchTypeId;
  int m_openMPTaskgroupMismatchTypeId;
  int m_openMPAtomicMismatchTypeId;
  int m_openMPPartialSyncTypeId;
  int m_mpiOrphanedRequestTypeId;
  int m_mpiDeadlockTypeId;
  int m_mpiCollectiveMismatchTypeId;
  int m_mpiConditionalCollectiveTypeId;
  int m_mpiUnsyncRMATypeId;
  int m_mpiRMARaceTypeId;
  int m_mpiWindowLeakTypeId;
  int m_cudaSharedRaceTypeId;
  int m_cudaGlobalRaceTypeId;
  int m_cudaInterKernelHazardTypeId;
  int m_cudaBarrierMismatchTypeId;
  int m_cudaWarpDivergenceTypeId;
  int m_cudaBankConflictTypeId;
  int m_cudaUncoalescedTypeId;
  int m_cudaVolatileMissingTypeId;
  int m_cudaSymbolicConfigTypeId;
  int m_cudaMemorySpaceTypeId;
  int m_cudaParametricRaceTypeId;

  // Results tracking
  Statistics m_stats;

  // Helper to convert ConcurrencyBugReport to BugReport
  void reportBug(const ConcurrencyBugReport &bug_report, int bug_type_id);
};

} // namespace concurrency

#endif // CONCURRENCY_CHECKER_H
