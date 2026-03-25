/**
 * @file MPIRMAAnalysis.h
 * @brief MPI RMA (Remote Memory Access) Analysis
 *
 * This file provides analysis for MPI RMA operations,
 * checking for data races and synchronization errors.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef MPI_RMA_ANALYSIS_H
#define MPI_RMA_ANALYSIS_H

#include "Analysis/Concurrency/ConcurrencyRelation.h"
#include "Analysis/Concurrency/MPI/MPIProcessModel.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace mpi {

class MPIProcessModel;

class MPIRMAAnalysis {
public:
  enum class EpochCompletion { None, LocalOnly, RemoteGuaranteed };
  enum class EpochProof { Must, May, Unknown };

  enum class SyncModel { FENCE, LOCK_UNLOCK, PSCW, NONE };

  enum class EpochState {
    Idle,
    FenceOpen,
    LockOpen,
    LockAllOpen,
    PSCWAccessOpen,
    PSCWExposureOpen
  };

  struct RMAWindow {
    WindowID window;
    const llvm::Instruction *create_inst;
    const llvm::Instruction *free_inst = nullptr;

    std::set<const llvm::Instruction *> put_ops;
    std::set<const llvm::Instruction *> get_ops;
    std::set<const llvm::Instruction *> accumulate_ops;

    std::set<const llvm::Instruction *> fence_ops;
    std::set<const llvm::Instruction *> lock_ops;
    std::set<const llvm::Instruction *> unlock_ops;
    std::set<const llvm::Instruction *> flush_ops;
  };

  struct RMAOperation {
    const llvm::Instruction *inst;
    const llvm::Function *function = nullptr;
    WindowID window;
    GroupID group = nullptr;
    int target_rank = -1;
    int target_rank_min = -1;
    int target_rank_max = -1;
    int64_t target_disp = -1;
    int64_t byte_length = -1;
    RMAEpochKind rma_epoch_kind = RMAEpochKind::None;
    concurrency::ProofStrength synchronization_proof =
        concurrency::ProofStrength::Unknown;
    concurrency::Relation relation;
    SyncModel sync_model = SyncModel::NONE;
    size_t epoch_id = 0;
    bool lock_all = false;
    bool flush_completed = false;
    bool local_completion_only = false;
    bool exposure_epoch_observed = false;
    bool pscw_group_unresolved = false;
    EpochCompletion epoch_completion = EpochCompletion::None;
    EpochProof epoch_proof = EpochProof::Unknown;

    const llvm::Instruction *sync_start = nullptr;
    const llvm::Instruction *sync_end = nullptr;
  };

  MPIRMAAnalysis(MPIProcessModel &model, ThreadAPI *api)
      : process_model_(model), thread_api_(api) {}

  void analyzeRMA();

  std::vector<RMAOperation> findUnsynchronizedRMAOps() const;

  std::vector<const llvm::Instruction *> findInvalidEpochTransitions() const;

  std::vector<const llvm::Instruction *> findUseAfterFreeWindows() const;

  std::vector<const llvm::Instruction *> findDoubleWindowFree() const;

  std::vector<std::pair<RMAOperation, RMAOperation>> findRMARaces() const;

  std::vector<WindowID> findLeakedWindows() const;
  size_t getTrackedWindowCount() const { return windows_.size(); }
  const std::vector<RMAOperation> &getSynchronizationRelations() const {
    return rma_operations_;
  }
  const std::vector<RMASynchronizationFact> &getSynchronizationFacts() const {
    return synchronization_facts_;
  }
  const std::vector<MPIModelGap> &getModelGaps() const { return model_gaps_; }

private:
  struct EpochMachine {
    EpochState state = EpochState::Idle;
    SyncModel model = SyncModel::NONE;
    const llvm::Instruction *start = nullptr;
    size_t epoch_id = 0;
    bool local_completion_only = false;
    bool remote_completion_observed = false;
    bool exposure_epoch_observed = false;
    std::vector<size_t> op_indices;
  };

  MPIProcessModel &process_model_;
  ThreadAPI *thread_api_;

  std::map<WindowID, RMAWindow> windows_;
  std::vector<RMAOperation> rma_operations_;
  std::vector<RMASynchronizationFact> synchronization_facts_;
  std::vector<MPIModelGap> model_gaps_;
  std::vector<const llvm::Instruction *> invalid_epoch_transitions_;
  std::vector<const llvm::Instruction *> use_after_free_windows_;
  std::vector<const llvm::Instruction *> double_window_free_;

  bool transitionEpochMachine(EpochMachine &machine, const MPIOperation &op,
                              size_t next_epoch_id) const;
  void annotateOperationsInMachine(EpochMachine &machine,
                                   const llvm::Instruction *end_inst,
                                   concurrency::ProofStrength proof,
                                   llvm::StringRef reason,
                                   bool close_epoch) const;
  SyncModel determineSyncModel(const RMAOperation &op) const;
  bool areRMAOpsConflicting(const RMAOperation &op1,
                            const RMAOperation &op2) const;
};

} // namespace mpi

#endif // MPI_RMA_ANALYSIS_H
