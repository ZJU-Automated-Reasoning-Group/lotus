#include "Analysis/Concurrency/MPI/MPIProtocolAutomaton.h"

namespace mpi {

size_t MPIProtocolAutomaton::allocateSlot(const CollectiveStateKey &key,
                                          const llvm::Function *participant) {
  return instances_[key].participant_slots[participant]++;
}

MPIProtocolState &
MPIProtocolAutomaton::getOrCreateState(const CollectiveStateKey &key,
                                       size_t slot_id) {
  MPIProtocolState &state = instances_[key].slots[slot_id];
  state.slot_id = slot_id;
  return state;
}

bool MPIProtocolAutomaton::isCompatible(
    const MPIProtocolState &state, const MPIProtocolTransition &transition) {
  if (!state.has_expected_type) {
    return true;
  }
  bool compatible = state.expected_type == transition.type;
  compatible =
      compatible && (state.expected_root_rank < 0 || transition.root_rank < 0 ||
                     state.expected_root_rank == transition.root_rank);
  compatible =
      compatible && (state.expected_count < 0 || transition.count < 0 ||
                     state.expected_count == transition.count);
  compatible = compatible &&
               (state.expected_recv_count < 0 || transition.recv_count < 0 ||
                state.expected_recv_count == transition.recv_count);
  compatible =
      compatible && (state.expected_datatype < 0 || transition.datatype < 0 ||
                     state.expected_datatype == transition.datatype);
  compatible =
      compatible &&
      (state.expected_recv_datatype < 0 || transition.recv_datatype < 0 ||
       state.expected_recv_datatype == transition.recv_datatype);
  compatible =
      compatible &&
      (state.expected_reduction_op < 0 || transition.reduction_op < 0 ||
       state.expected_reduction_op == transition.reduction_op);
  compatible = compatible && state.expected_in_place == transition.in_place;
  return compatible;
}

} // namespace mpi
