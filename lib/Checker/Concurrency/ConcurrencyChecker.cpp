// Author: rainoftime

#include "Checker/Concurrency/ConcurrencyChecker.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Alias/UnificationBased/seadsa/DsaAnalysis.hh"
#include "Alias/UnificationBased/seadsa/InitializePasses.hh"
#include "Checker/Concurrency/ConcurrencyAnalysisDumper.h"
#include "Concurrency/MHP/HappensBeforeAnalysis.h"
#include "Concurrency/Utils/ThreadAPI.h"

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;

namespace concurrency {

ConcurrencyChecker::ConcurrencyChecker(Module &module)
    : m_module(module), m_aliasAnalysis(nullptr),
      m_threadAPI(ThreadAPI::getThreadAPI()), m_stats{} {

  // Register bug types with BugReportMgr (shared pattern)
  BugReportMgr &mgr = BugReportMgr::get_instance();
  m_dataRaceTypeId =
      mgr.register_bug_type("Data Race", BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-362");
  m_deadlockTypeId =
      mgr.register_bug_type("Deadlock", BugDescription::BI_HIGH,
                            BugDescription::BC_ERROR, "Deadlock potential");
  m_atomicityViolationTypeId = mgr.register_bug_type(
      "Atomicity Violation", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR, "Non-atomic operation sequence");
  m_condVarMisuseTypeId = mgr.register_bug_type(
      "Condition Variable Misuse", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Improper condition variable usage");
  m_lockMismatchTypeId = mgr.register_bug_type(
      "Lock Mismatch", BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Lock acquisition/release mismatch");
  m_openMPTaskgroupMismatchTypeId = mgr.register_bug_type(
      "OpenMP Taskgroup Mismatch", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Unbalanced OpenMP taskgroup region");
  m_openMPAtomicMismatchTypeId = mgr.register_bug_type(
      "OpenMP Atomic Region Mismatch", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Unbalanced OpenMP atomic region");
  m_openMPPartialSyncTypeId = mgr.register_bug_type(
      "OpenMP Partial Task Synchronization", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR,
      "Selective task wait may leave sibling tasks unsynchronized");
  m_mpiOrphanedRequestTypeId = mgr.register_bug_type(
      "MPI Orphaned Request", BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Non-blocking MPI request without matching completion");
  m_mpiDeadlockTypeId = mgr.register_bug_type(
      "MPI Deadlock", BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Potential blocking communication deadlock");
  m_mpiCollectiveMismatchTypeId = mgr.register_bug_type(
      "MPI Collective Mismatch", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "Incompatible collective operations across processes");
  m_mpiConditionalCollectiveTypeId = mgr.register_bug_type(
      "MPI Conditional Collective", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "Collective may be executed only by a subset of ranks");
  m_mpiUnsyncRMATypeId =
      mgr.register_bug_type("MPI Unsynchronized RMA", BugDescription::BI_HIGH,
                            BugDescription::BC_ERROR,
                            "RMA operation without recognized synchronization");
  m_mpiRMARaceTypeId = mgr.register_bug_type(
      "MPI RMA Race", BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Conflicting RMA operations without sufficient synchronization");
  m_mpiWindowLeakTypeId = mgr.register_bug_type(
      "MPI Window Leak", BugDescription::BI_MEDIUM, BugDescription::BC_ERROR,
      "RMA window may not be freed");
  m_cudaSharedRaceTypeId = mgr.register_bug_type(
      "CUDA Shared-Memory Race", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "Conflicting shared-memory accesses without sufficient synchronization");
  m_cudaGlobalRaceTypeId =
      mgr.register_bug_type("CUDA Global-Memory Race", BugDescription::BI_HIGH,
                            BugDescription::BC_ERROR,
                            "Conflicting global/device-memory accesses without "
                            "sufficient synchronization");
  m_cudaInterKernelHazardTypeId =
      mgr.register_bug_type("CUDA Inter-Kernel Hazard", BugDescription::BI_HIGH,
                            BugDescription::BC_ERROR,
                            "Potential inter-kernel hazard across ordered or "
                            "unordered CUDA launches");
  m_cudaBarrierMismatchTypeId = mgr.register_bug_type(
      "CUDA Barrier Mismatch", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Not all threads reach the same CUDA barrier");
  m_cudaWarpDivergenceTypeId =
      mgr.register_bug_type("CUDA Warp Divergence", BugDescription::BI_MEDIUM,
                            BugDescription::BC_PERFORMANCE,
                            "Warp executes divergent control-flow paths");
  m_cudaBankConflictTypeId = mgr.register_bug_type(
      "CUDA Bank Conflict", BugDescription::BI_MEDIUM,
      BugDescription::BC_PERFORMANCE,
      "Shared-memory accesses may map multiple lanes to the same bank");
  m_cudaUncoalescedTypeId = mgr.register_bug_type(
      "CUDA Uncoalesced Access", BugDescription::BI_MEDIUM,
      BugDescription::BC_PERFORMANCE,
      "Global-memory accesses may not coalesce");
  m_cudaVolatileMissingTypeId = mgr.register_bug_type(
      "CUDA Missing Volatile", BugDescription::BI_LOW,
      BugDescription::BC_WARNING,
      "Potential missing volatile on inter-thread CUDA memory");
  m_cudaSymbolicConfigTypeId = mgr.register_bug_type(
      "CUDA Symbolic Launch Configuration", BugDescription::BI_LOW,
      BugDescription::BC_WARNING,
      "Kernel launch uses symbolic or unknown thread/block sizing");
  m_cudaMemorySpaceTypeId =
      mgr.register_bug_type("CUDA Memory Space Ambiguity",
                            BugDescription::BI_LOW, BugDescription::BC_WARNING,
                            "Could not classify CUDA memory space precisely");
  m_cudaParametricRaceTypeId = mgr.register_bug_type(
      "CUDA Parametric Race Risk", BugDescription::BI_MEDIUM,
      BugDescription::BC_WARNING,
      "Parametric CUDA thread/block reasoning exposes a potential race");

  m_stats.totalInstructions = 0;
  m_stats.mhpPairs = 0;
  m_stats.locksAnalyzed = 0;
  m_stats.dataRacesFound = 0;
  m_stats.deadlocksFound = 0;
  m_stats.atomicityViolationsFound = 0;
  m_stats.condVarBugsFound = 0;
  m_stats.lockMismatchesFound = 0;
  m_stats.openMPBugsFound = 0;
  m_stats.mpiBugsFound = 0;
  m_stats.cudaBugsFound = 0;
  m_stats.sparseInterferenceEdges = 0;
  m_stats.sparsePointsToFacts = 0;
  m_stats.sparseMemoryRegions = 0;
  m_stats.sparseOriginalNodes = 0;
  m_stats.sparseSlicedNodes = 0;
  m_stats.sparsePreThreads = 0;
  m_stats.sparseMainThreads = 0;
  m_stats.sparseForkMemoryEdges = 0;
  m_stats.sparseJoinMemoryEdges = 0;
  m_stats.sparseHashConsedUniqueSets = 0;
  m_stats.sparseHashConsedUnionCacheHits = 0;
  m_stats.openMPSummary = OpenMP::OpenMPTaskGraph::AnalysisSummary{};
  m_stats.mpiSummary = ConcurrencyFacade::MPISummary{};
  m_stats.cudaSummary = ConcurrencyFacade::CUDASummary{};

  for (Function &func : module) {
    if (!func.isDeclaration()) {
      for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I)
        m_stats.totalInstructions++;
    }
  }
}

void ConcurrencyChecker::runAnalyses() {
  // The previous checker may retain a non-owning pointer to the sparse solver.
  m_dataRaceChecker.reset();
  m_mhpAnalysis = nullptr;
  m_mhpAnalysisStorage.reset();
  m_locksetAnalysis.reset();
  m_locksetAnalysisView = nullptr;
  m_escapeAnalysis.reset();
  m_threadLocalAnalysis.reset();
  m_happensBeforeAnalysis.reset();
  m_staticThreadSharingPM.reset();
  m_staticThreadSharingAnalysis = nullptr;
  m_openMPTaskGraph.reset();
  m_mpiAnalysis.reset();
  m_cudaAnalysis.reset();
  m_sparseRefinement.reset();
  m_stats.mhpPairs = 0;
  m_stats.locksAnalyzed = 0;
  m_stats.cudaBugsFound = 0;
  m_stats.sparseInterferenceEdges = 0;
  m_stats.sparsePointsToFacts = 0;
  m_stats.sparseMemoryRegions = 0;
  m_stats.sparseOriginalNodes = 0;
  m_stats.sparseSlicedNodes = 0;
  m_stats.sparsePreThreads = 0;
  m_stats.sparseMainThreads = 0;
  m_stats.sparseForkMemoryEdges = 0;
  m_stats.sparseJoinMemoryEdges = 0;
  m_stats.sparseHashConsedUniqueSets = 0;
  m_stats.sparseHashConsedUnionCacheHits = 0;
  m_stats.openMPSummary = OpenMP::OpenMPTaskGraph::AnalysisSummary{};
  m_stats.mpiSummary = ConcurrencyFacade::MPISummary{};
  m_stats.cudaSummary = ConcurrencyFacade::CUDASummary{};

  // Config-driven activation: run only analyses required by enabled checks
  // (Goblint-style)
  bool needMHP = m_checkDataRaces || m_checkDeadlocks ||
                 m_checkAtomicityViolations || m_checkCondVars;
  bool needLockSet = m_checkDataRaces || m_checkDeadlocks ||
                     m_checkAtomicityViolations || m_checkCondVars ||
                     m_checkLockMismatches;
  bool needEscape = m_checkDataRaces;
  bool needThreadLocal = m_checkDataRaces;
  bool needStaticSharing = m_checkDataRaces;
  bool needHappensBefore = m_checkDataRaces;
  bool needOpenMP = m_checkOpenMP;
  bool needMPI = m_checkMPI;
  bool needCUDA = m_checkCUDA;

  if (needMHP) {
    if (m_mhpBackend == MHPBackendKind::StaticVectorClock) {
      auto svc = std::make_unique<StaticVectorClockMHP>(m_module);
      svc->analyze();
      m_stats.mhpPairs = svc->getMhpPairCount();
      m_mhpAnalysis = svc.get();
      m_mhpAnalysisStorage = std::move(svc);
    } else {
      auto mhp = std::make_unique<MHPAnalysis>(m_module);
      mhp->enableLockSetAnalysis();
      mhp->analyze();
      m_stats.mhpPairs = mhp->getMhpPairCount();
      m_mhpAnalysis = mhp.get();
      m_mhpAnalysisStorage = std::move(mhp);
    }
  }

  auto *regionMHP = dynamic_cast<MHPAnalysis *>(m_mhpAnalysis);

  // Prefer reusing the lockset computed inside MHPAnalysis (avoids duplicate
  // work).
  m_locksetAnalysisView = nullptr;
  if (needMHP && needLockSet && regionMHP && regionMHP->getLockSetAnalysis()) {
    m_locksetAnalysisView = regionMHP->getLockSetAnalysis();
    m_stats.locksAnalyzed = m_locksetAnalysisView->getStatistics().num_locks;
  } else if (needLockSet) {
    m_locksetAnalysis = std::make_unique<LockSetAnalysis>(m_module);
    if (m_aliasAnalysis)
      m_locksetAnalysis->setAliasAnalysis(m_aliasAnalysis);
    m_locksetAnalysis->analyze();
    m_locksetAnalysisView = m_locksetAnalysis.get();
    m_stats.locksAnalyzed = m_locksetAnalysisView->getStatistics().num_locks;
  }

  if (needEscape) {
    m_escapeAnalysis = std::make_unique<EscapeAnalysis>(m_module);
    m_escapeAnalysis->analyze();
  }

  if (needThreadLocal) {
    m_threadLocalAnalysis =
        std::make_unique<ThreadLocal::ThreadLocalAnalysis>(m_module);
    m_threadLocalAnalysis->analyze();
  }

  if (needStaticSharing) {
    static bool passes_initialized = false;
    if (!passes_initialized) {
      llvm::PassRegistry &registry = *llvm::PassRegistry::getPassRegistry();
      seadsa::initializeAnalysisPasses(registry);
      llvm::initializeDsaAnalysisPass(registry);
      passes_initialized = true;
    }

    m_staticThreadSharingPM = std::make_unique<llvm::legacy::PassManager>();
    m_staticThreadSharingPM->add(new seadsa::DsaAnalysis());
    auto *sharing = new lotus::StaticThreadSharingAnalysis();
    m_staticThreadSharingAnalysis = sharing;
    m_staticThreadSharingPM->add(sharing);
    m_staticThreadSharingPM->run(m_module);
  }

  if (needHappensBefore && regionMHP) {
    m_happensBeforeAnalysis =
        std::make_unique<HappensBeforeAnalysis>(m_module, *regionMHP);
    lotus::AliasAnalysisWrapper *aa = m_aliasAnalysis;
    if (!aa)
      aa = regionMHP->getAliasAnalysis();
    if (aa)
      m_happensBeforeAnalysis->setAliasAnalysis(aa);
    m_happensBeforeAnalysis->analyze();
  }

  if (needOpenMP) {
    m_openMPTaskGraph = std::make_unique<OpenMP::OpenMPTaskGraph>(m_module);
    m_openMPTaskGraph->analyze();
    m_stats.openMPSummary = m_openMPTaskGraph->getSummary();
  }

  if (needMPI) {
    m_mpiAnalysis = std::make_unique<mpi::MPIAnalysis>(m_module);
    m_mpiAnalysis->runAnalysis();
    const auto &mpi_results = m_mpiAnalysis->getResults();
    m_stats.mpiSummary.operation_count =
        m_mpiAnalysis->getProcessModel().getAllOperations().size();
    m_stats.mpiSummary.nonblocking_operation_count =
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::SEND_NONBLOCKING) +
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::RECV_NONBLOCKING) +
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::BARRIER_NONBLOCKING) +
        m_mpiAnalysis->getOperationCount(
            mpi::MPIOpKind::COLLECTIVE_NONBLOCKING);
    m_stats.mpiSummary.collective_operation_count =
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::BARRIER_BLOCKING) +
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::BARRIER_NONBLOCKING) +
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::COLLECTIVE_BLOCKING) +
        m_mpiAnalysis->getOperationCount(
            mpi::MPIOpKind::COLLECTIVE_NONBLOCKING);
    m_stats.mpiSummary.communicator_management_count =
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::COMM_MANAGEMENT);
    m_stats.mpiSummary.request_management_count =
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::REQUEST_MANAGEMENT);
    m_stats.mpiSummary.rma_operation_count =
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::RMA_DATA);
    m_stats.mpiSummary.rma_sync_count =
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::RMA_SYNC);
    m_stats.mpiSummary.orphaned_request_count =
        mpi_results.orphaned_requests.size();
    m_stats.mpiSummary.potential_deadlock_count =
        mpi_results.potential_deadlocks.size();
    m_stats.mpiSummary.mismatched_collective_count =
        mpi_results.mismatched_collectives.size();
    m_stats.mpiSummary.conditional_collective_count =
        mpi_results.conditional_collectives.size();
    m_stats.mpiSummary.unsynchronized_rma_count =
        mpi_results.unsynchronized_rma.size();
    m_stats.mpiSummary.rma_race_count = mpi_results.rma_races.size();
    m_stats.mpiSummary.leaked_window_count = mpi_results.leaked_windows.size();
    m_stats.mpiSummary.collective_slot_count =
        m_mpiAnalysis->getProtocolDiagnosticCount("collective_slots_tracked");
  }

  if (needCUDA) {
    m_cudaAnalysis = std::make_unique<cuda::CUDAAnalysis>(m_module);
    m_cudaAnalysis->runAnalysis();
    m_stats.cudaSummary = ConcurrencyFacade::summarizeCUDA(*m_cudaAnalysis);
  }

  lotus::AliasAnalysisWrapper *aa = m_aliasAnalysis;
  if (!aa && regionMHP)
    aa = regionMHP->getAliasAnalysis();

  if (m_enableSparseFlowSensitiveRefinement && m_checkDataRaces &&
      m_mhpAnalysis) {
    m_sparseRefinement =
        std::make_unique<lotus::analysis::WholeProgramSparseRefinement>();
    lotus::analysis::WholeProgramSparseRefinement::Config sparseConfig;
    sparseConfig.mode =
        m_enableMultiStageSlicing
            ? lotus::analysis::WholeProgramSparseRefinement::Mode::
                  MultiStageSlicing
            : lotus::analysis::WholeProgramSparseRefinement::Mode::WholeProgram;
    sparseConfig.memoryPartition = m_sparseMemoryPartition;
    sparseConfig.threadContextLimit = m_threadContextLimit;
    sparseConfig.pointsToSetBackend = m_sparsePointsToSetBackend;
    const auto &sparseStats = m_sparseRefinement->build(
        m_module, *m_mhpAnalysis, m_locksetAnalysisView, sparseConfig);
    m_stats.sparseInterferenceEdges = sparseStats.overlay.edgesAdded;
    m_stats.sparsePointsToFacts = sparseStats.solver.topLevelFacts;
    m_stats.sparseMemoryRegions = sparseStats.memoryRegions.regions;
    m_stats.sparseOriginalNodes = sparseStats.slicing.originalNodes;
    m_stats.sparseSlicedNodes = sparseStats.slicing.pointsToNodes;
    m_stats.sparsePreThreads = sparseStats.preThreads.nodes;
    m_stats.sparseMainThreads = sparseStats.mainThreads.nodes;
    m_stats.sparseForkMemoryEdges = sparseStats.preOverlay.forkMemoryEdges +
                                    sparseStats.overlay.forkMemoryEdges;
    m_stats.sparseJoinMemoryEdges = sparseStats.preOverlay.joinMemoryEdges +
                                    sparseStats.overlay.joinMemoryEdges;
    m_stats.sparseHashConsedUniqueSets =
        sparseStats.solver.hashConsedUniqueSets;
    m_stats.sparseHashConsedUnionCacheHits =
        sparseStats.solver.hashConsedUnionCacheHits;
  }

  m_dataRaceChecker = std::make_unique<DataRaceChecker>(
      m_module, m_mhpAnalysis, m_locksetAnalysisView, m_escapeAnalysis.get(),
      m_threadLocalAnalysis.get(), m_staticThreadSharingAnalysis, aa,
      m_happensBeforeAnalysis.get());
  m_deadlockChecker = std::make_unique<DeadlockChecker>(
      m_module, m_locksetAnalysisView, m_mhpAnalysis,
      m_happensBeforeAnalysis.get(), m_threadAPI);
  m_atomicityChecker = std::make_unique<AtomicityChecker>(
      m_module, m_mhpAnalysis, m_locksetAnalysisView, m_threadAPI, aa);
  m_condVarChecker = std::make_unique<ConditionVariableChecker>(
      m_module, m_threadAPI, m_locksetAnalysisView);
  m_lockMismatchChecker = std::make_unique<LockMismatchChecker>(
      m_module, m_locksetAnalysisView, m_threadAPI);
  m_openMPChecker = std::make_unique<OpenMPChecker>(
      m_module, m_openMPTaskGraph.get(), m_threadAPI);
  m_mpiChecker = std::make_unique<MPIChecker>(m_module, m_mpiAnalysis.get());
  m_cudaChecker = std::make_unique<CUDAChecker>(m_module, m_cudaAnalysis.get());
}

void ConcurrencyChecker::runChecks() {
  if (m_checkDataRaces) {
    checkDataRaces();
  }

  if (m_checkDeadlocks) {
    checkDeadlocks();
  }

  if (m_checkAtomicityViolations) {
    checkAtomicityViolations();
  }

  if (m_checkCondVars) {
    checkConditionVariables();
  }

  if (m_checkLockMismatches) {
    checkLockMismatches();
  }

  if (m_checkOpenMP) {
    checkOpenMPBugs();
  }

  if (m_checkMPI) {
    checkMPIBugs();
  }

  if (m_checkCUDA) {
    checkCUDABugs();
  }
}

void ConcurrencyChecker::checkDataRaces() {
  if (m_dataRaceChecker && m_mhpAnalysis) {
    auto reports = m_dataRaceChecker->checkDataRaces();
    m_stats.dataRacesFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report, m_dataRaceTypeId);
    }
  }
}

void ConcurrencyChecker::checkDeadlocks() {
  if (m_deadlockChecker && m_mhpAnalysis && m_locksetAnalysisView) {
    auto reports = m_deadlockChecker->checkDeadlocks();
    m_stats.deadlocksFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report, m_deadlockTypeId);
    }
  }
}

void ConcurrencyChecker::checkAtomicityViolations() {
  if (m_atomicityChecker && m_mhpAnalysis && m_locksetAnalysisView) {
    auto reports = m_atomicityChecker->checkAtomicityViolations();
    m_stats.atomicityViolationsFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report, m_atomicityViolationTypeId);
    }
  }
}

void ConcurrencyChecker::checkConditionVariables() {
  if (m_condVarChecker && m_locksetAnalysisView) {
    auto reports = m_condVarChecker->checkConditionVariables();
    m_stats.condVarBugsFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report, m_condVarMisuseTypeId);
    }
  }
}

void ConcurrencyChecker::checkLockMismatches() {
  if (m_lockMismatchChecker && m_locksetAnalysisView) {
    auto reports = m_lockMismatchChecker->checkLockMisuse();
    m_stats.lockMismatchesFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report, m_lockMismatchTypeId);
    }
  }
}

void ConcurrencyChecker::checkOpenMPBugs() {
  if (!m_openMPChecker) {
    return;
  }

  auto reports = m_openMPChecker->checkOpenMPBugs();
  m_stats.openMPBugsFound = reports.size();
  for (const auto &report : reports) {
    switch (report.bugType) {
    case ConcurrencyBugType::OPENMP_TASKGROUP_MISMATCH:
      reportBug(report, m_openMPTaskgroupMismatchTypeId);
      break;
    case ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH:
      reportBug(report, m_openMPAtomicMismatchTypeId);
      break;
    case ConcurrencyBugType::OPENMP_PARTIAL_SYNC:
      reportBug(report, m_openMPPartialSyncTypeId);
      break;
    default:
      break;
    }
  }
}

void ConcurrencyChecker::checkMPIBugs() {
  if (!m_mpiChecker) {
    return;
  }

  auto reports = m_mpiChecker->checkMPIBugs();
  m_stats.mpiBugsFound = reports.size();
  for (const auto &report : reports) {
    switch (report.bugType) {
    case ConcurrencyBugType::MPI_ORPHANED_REQUEST:
      reportBug(report, m_mpiOrphanedRequestTypeId);
      break;
    case ConcurrencyBugType::MPI_DEADLOCK:
      reportBug(report, m_mpiDeadlockTypeId);
      break;
    case ConcurrencyBugType::MPI_COLLECTIVE_MISMATCH:
      reportBug(report, m_mpiCollectiveMismatchTypeId);
      break;
    case ConcurrencyBugType::MPI_CONDITIONAL_COLLECTIVE:
      reportBug(report, m_mpiConditionalCollectiveTypeId);
      break;
    case ConcurrencyBugType::MPI_UNSYNC_RMA:
      reportBug(report, m_mpiUnsyncRMATypeId);
      break;
    case ConcurrencyBugType::MPI_RMA_RACE:
      reportBug(report, m_mpiRMARaceTypeId);
      break;
    case ConcurrencyBugType::MPI_WINDOW_LEAK:
      reportBug(report, m_mpiWindowLeakTypeId);
      break;
    default:
      break;
    }
  }
}

void ConcurrencyChecker::checkCUDABugs() {
  if (!m_cudaChecker) {
    return;
  }

  auto reports = m_cudaChecker->checkCUDABugs();
  m_stats.cudaBugsFound = reports.size();
  for (const auto &report : reports) {
    switch (report.bugType) {
    case ConcurrencyBugType::CUDA_SHARED_MEMORY_RACE:
      reportBug(report, m_cudaSharedRaceTypeId);
      break;
    case ConcurrencyBugType::CUDA_GLOBAL_MEMORY_RACE:
      reportBug(report, m_cudaGlobalRaceTypeId);
      break;
    case ConcurrencyBugType::CUDA_INTER_KERNEL_HAZARD:
      reportBug(report, m_cudaInterKernelHazardTypeId);
      break;
    case ConcurrencyBugType::CUDA_BARRIER_MISMATCH:
      reportBug(report, m_cudaBarrierMismatchTypeId);
      break;
    case ConcurrencyBugType::CUDA_WARP_DIVERGENCE:
      reportBug(report, m_cudaWarpDivergenceTypeId);
      break;
    case ConcurrencyBugType::CUDA_BANK_CONFLICT:
      reportBug(report, m_cudaBankConflictTypeId);
      break;
    case ConcurrencyBugType::CUDA_UNCOALESCED_ACCESS:
      reportBug(report, m_cudaUncoalescedTypeId);
      break;
    case ConcurrencyBugType::CUDA_VOLATILE_MISSING:
      reportBug(report, m_cudaVolatileMissingTypeId);
      break;
    case ConcurrencyBugType::CUDA_SYMBOLIC_CONFIG_RISK:
      reportBug(report, m_cudaSymbolicConfigTypeId);
      break;
    case ConcurrencyBugType::CUDA_SHARED_GLOBAL_SPACE_MISMATCH:
      reportBug(report, m_cudaMemorySpaceTypeId);
      break;
    case ConcurrencyBugType::CUDA_PARAMETRIC_RACE_RISK:
      reportBug(report, m_cudaParametricRaceTypeId);
      break;
    default:
      break;
    }
  }
}

void ConcurrencyChecker::reportBug(const ConcurrencyBugReport &bug_report,
                                   int bug_type_id) {
  // Create a new BugReport following the shared reporting pattern
  BugReport *report = new BugReport(bug_type_id);

  // Add diagnostic steps showing the concurrency bug trace
  // Enhanced with Infer-inspired features: trace levels, node tags, and access
  // info
  int trace_level = 0;
  for (size_t i = 0; i < bug_report.steps.size(); ++i) {
    const auto &step = bug_report.steps[i];
    if (step.instruction) {
      std::vector<NodeTag> tags;

      // Infer node tags based on instruction type
      if (isa<CallInst>(step.instruction)) {
        tags.push_back(NodeTag::CALL_SITE);
      }

      // Determine access type
      std::string access = "step";
      if (isa<LoadInst>(step.instruction)) {
        access = "load";
      } else if (isa<StoreInst>(step.instruction)) {
        access = "store";
      } else if (isa<CallInst>(step.instruction)) {
        access = "call";
      }

      // Use enhanced append_step with trace level, tags, and access info
      report->append_step(const_cast<Instruction *>(step.instruction),
                          step.description, trace_level, tags, access);

      // Increment trace level for nested calls
      if (isa<CallInst>(step.instruction)) {
        trace_level++;
      }
    }
  }

  // Set confidence score based on importance
  report->set_conf_score(bug_report.importance == BugDescription::BI_HIGH ? 90
                                                                          : 70);

  // Add metadata (Infer-inspired feature)
  report->add_metadata("checker", "ConcurrencyChecker");
  if (bug_report.bugType == ConcurrencyBugType::OPENMP_TASKGROUP_MISMATCH ||
      bug_report.bugType == ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH ||
      bug_report.bugType == ConcurrencyBugType::OPENMP_PARTIAL_SYNC) {
    report->add_metadata("checker", "OpenMPChecker");
    report->add_metadata("openmp_task_count",
                         std::to_string(m_stats.openMPSummary.task_count));
    report->add_metadata(
        "openmp_task_with_dependencies_count",
        std::to_string(m_stats.openMPSummary.task_with_dependencies_count));
    report->add_metadata(
        "openmp_wait_boundary_count",
        std::to_string(m_stats.openMPSummary.wait_boundary_count));
    report->add_metadata(
        "openmp_partial_wait_boundary_count",
        std::to_string(m_stats.openMPSummary.partial_wait_boundary_count));
  } else if (bug_report.bugType == ConcurrencyBugType::MPI_ORPHANED_REQUEST ||
             bug_report.bugType == ConcurrencyBugType::MPI_DEADLOCK ||
             bug_report.bugType ==
                 ConcurrencyBugType::MPI_COLLECTIVE_MISMATCH ||
             bug_report.bugType ==
                 ConcurrencyBugType::MPI_CONDITIONAL_COLLECTIVE ||
             bug_report.bugType == ConcurrencyBugType::MPI_UNSYNC_RMA ||
             bug_report.bugType == ConcurrencyBugType::MPI_RMA_RACE ||
             bug_report.bugType == ConcurrencyBugType::MPI_WINDOW_LEAK) {
    report->add_metadata("checker", "MPIChecker");
    report->add_metadata("mpi_operation_count",
                         std::to_string(m_stats.mpiSummary.operation_count));
    report->add_metadata(
        "mpi_nonblocking_operation_count",
        std::to_string(m_stats.mpiSummary.nonblocking_operation_count));
    report->add_metadata(
        "mpi_collective_operation_count",
        std::to_string(m_stats.mpiSummary.collective_operation_count));
    report->add_metadata(
        "mpi_collective_slot_count",
        std::to_string(m_stats.mpiSummary.collective_slot_count));
    report->add_metadata(
        "mpi_rma_operation_count",
        std::to_string(m_stats.mpiSummary.rma_operation_count));
    report->add_metadata(
        "mpi_leaked_window_count",
        std::to_string(m_stats.mpiSummary.leaked_window_count));
  } else if (bug_report.bugType == ConcurrencyBugType::CUDA_WARP_DIVERGENCE ||
             bug_report.bugType ==
                 ConcurrencyBugType::CUDA_SHARED_MEMORY_RACE ||
             bug_report.bugType ==
                 ConcurrencyBugType::CUDA_GLOBAL_MEMORY_RACE ||
             bug_report.bugType ==
                 ConcurrencyBugType::CUDA_INTER_KERNEL_HAZARD ||
             bug_report.bugType == ConcurrencyBugType::CUDA_BARRIER_MISMATCH ||
             bug_report.bugType == ConcurrencyBugType::CUDA_BANK_CONFLICT ||
             bug_report.bugType ==
                 ConcurrencyBugType::CUDA_UNCOALESCED_ACCESS ||
             bug_report.bugType == ConcurrencyBugType::CUDA_VOLATILE_MISSING ||
             bug_report.bugType ==
                 ConcurrencyBugType::CUDA_SYMBOLIC_CONFIG_RISK ||
             bug_report.bugType ==
                 ConcurrencyBugType::CUDA_SHARED_GLOBAL_SPACE_MISMATCH ||
             bug_report.bugType ==
                 ConcurrencyBugType::CUDA_PARAMETRIC_RACE_RISK) {
    report->add_metadata("checker", "CUDAChecker");
    report->add_metadata(
        "cuda_kernel_launch_count",
        std::to_string(m_stats.cudaSummary.kernel_launch_count));
    report->add_metadata(
        "cuda_inter_kernel_hazard_count",
        std::to_string(m_stats.cudaSummary.inter_kernel_hazard_count));
    report->add_metadata("cuda_transfer_count",
                         std::to_string(m_stats.cudaSummary.transfer_count));
    report->add_metadata(
        "cuda_unified_memory_count",
        std::to_string(m_stats.cudaSummary.unified_memory_count));
  }
  report->add_metadata(
      "importance",
      bug_report.importance == BugDescription::BI_HIGH ? "HIGH" : "MEDIUM");

  // Report to the manager with deduplication enabled
  BugReportMgr::get_instance().insert_report(bug_type_id, report, true);
}

void ConcurrencyChecker::dumpAnalysisResults(raw_ostream &os,
                                             bool jsonFormat) const {
  if (!m_mhpAnalysis || !m_locksetAnalysisView) {
    os << "No analysis results (runAnalyses() not run or no analyses "
          "enabled).\n";
    return;
  }
  ConcurrencyAnalysisDumper dumper(m_module, m_mhpAnalysis,
                                   m_locksetAnalysisView,
                                   m_escapeAnalysis.get(), m_threadAPI);
  dumper.dump(os, jsonFormat);
}

} // namespace concurrency
