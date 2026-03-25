/**
 * @file MPICollectiveAnalysis.h
 * @brief MPI Collective Operation Analysis
 *
 * This file provides analysis for MPI collective operations,
 * checking for mismatches and correctness.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef MPI_COLLECTIVE_ANALYSIS_H
#define MPI_COLLECTIVE_ANALYSIS_H

#include "Analysis/Concurrency/MPI/MPIProcessModel.h"
#include "Analysis/Concurrency/MPI/MPIProtocolAutomaton.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mpi {

class MPIProcessModel;

// MPICollectiveAnalysis owns collective protocol composition. It consumes the
// normalized process-model facts plus function summaries and projects collective
// protocol/frontier results for the public API.
class MPICollectiveAnalysis {
public:
  struct CollectiveCall {
    const llvm::Instruction *inst;
    ThreadAPI::TD_TYPE type;
    CommunicatorID comm;
    MPIBlockingMode blocking_mode = MPIBlockingMode::Unknown;
    MPICollectiveVariant collective_variant = MPICollectiveVariant::Unknown;
    MPICollectiveShape collective_shape = MPICollectiveShape::Unknown;
    size_t communicator_class_id = 0;
    size_t communicator_subgroup_id = 0;
    size_t participant_class_id = 0;
    size_t collective_protocol_class_id = 0;
    const llvm::Function *function;
    size_t sequence_index = 0;
    size_t protocol_sequence_id = 0;
    ProtocolReachability reachability = ProtocolReachability::Unknown;
    concurrency::Relation protocol_relation;
    MPIParticipantSet participants;

    int root_rank = -1;
    int count = -1;
    int recv_count = -1;
    int datatype = -1;
    int recv_datatype = -1;
    int reduction_op = -1;
    bool in_place = false;
  };

  explicit MPICollectiveAnalysis(MPIProcessModel &model)
      : process_model_(model) {}

  void analyzeCollectives();

  std::vector<std::pair<CollectiveCall, CollectiveCall>>
  findMismatchedCollectives() const;

  std::vector<const llvm::Instruction *> findConditionalCollectives() const;
  std::vector<std::pair<CollectiveCall, CollectiveCall>>
  findWrongRootRanks() const;
  const std::unordered_map<std::string, size_t> &
  getProtocolDiagnostics() const {
    return protocol_diagnostics_;
  }
  const std::map<CollectiveStateKey, MPIProtocolInstance> &
  getProtocolAutomata() const {
    return protocol_automaton_.instances();
  }
  const std::vector<CollectiveCall> &getProtocolRelations() const {
    return collective_calls_;
  }
  const std::vector<CollectiveProtocolFrontier> &getProtocolFrontiers() const {
    return protocol_frontiers_;
  }
  size_t getProtocolRelationCount(concurrency::RelationKind kind) const;

private:
  MPIProcessModel &process_model_;
  std::vector<CollectiveCall> collective_calls_;
  std::vector<CollectiveProtocolFrontier> protocol_frontiers_;
  MPIProtocolAutomaton protocol_automaton_;
  mutable std::unordered_map<std::string, size_t> protocol_diagnostics_;

  bool areCollectivesCompatible(const CollectiveCall &c1,
                                const CollectiveCall &c2) const;
  static int getRootArgIndex(ThreadAPI::TD_TYPE type);
};

} // namespace mpi

#endif // MPI_COLLECTIVE_ANALYSIS_H
