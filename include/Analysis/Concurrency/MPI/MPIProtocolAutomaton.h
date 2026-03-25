#pragma once

#include "Analysis/Concurrency/MPI/MPIOperation.h"

#include <map>

namespace mpi {

struct CollectiveStateKey {
  size_t communicator_class_id = 0;
  size_t communicator_subgroup_id = 0;
  size_t collective_protocol_class_id = 0;

  bool operator<(const CollectiveStateKey &other) const {
    if (communicator_class_id != other.communicator_class_id) {
      return communicator_class_id < other.communicator_class_id;
    }
    if (communicator_subgroup_id != other.communicator_subgroup_id) {
      return communicator_subgroup_id < other.communicator_subgroup_id;
    }
    return collective_protocol_class_id < other.collective_protocol_class_id;
  }
};

struct MPIProtocolState {
  size_t slot_id = 0;
  ThreadAPI::TD_TYPE expected_type = ThreadAPI::TD_DUMMY;
  bool has_expected_type = false;
  int expected_root_rank = -1;
  int expected_count = -1;
  int expected_recv_count = -1;
  int expected_datatype = -1;
  int expected_recv_datatype = -1;
  int expected_reduction_op = -1;
  bool expected_in_place = false;
};

struct MPIProtocolInstance {
  std::map<const llvm::Function *, size_t> participant_slots;
  std::map<size_t, MPIProtocolState> slots;
};

struct MPIProtocolTransition {
  size_t slot_id = 0;
  ThreadAPI::TD_TYPE type = ThreadAPI::TD_DUMMY;
  int root_rank = -1;
  int count = -1;
  int recv_count = -1;
  int datatype = -1;
  int recv_datatype = -1;
  int reduction_op = -1;
  bool in_place = false;
};

class MPIProtocolAutomaton {
public:
  size_t allocateSlot(const CollectiveStateKey &key,
                      const llvm::Function *participant);

  MPIProtocolState &getOrCreateState(const CollectiveStateKey &key,
                                     size_t slot_id);

  const std::map<CollectiveStateKey, MPIProtocolInstance> &instances() const {
    return instances_;
  }

  static bool isCompatible(const MPIProtocolState &state,
                           const MPIProtocolTransition &transition);

private:
  std::map<CollectiveStateKey, MPIProtocolInstance> instances_;
};

} // namespace mpi
