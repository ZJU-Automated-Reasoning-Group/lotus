#include "CFL/CSIndex/SCS/PolicyAutomaton.h"

#include <algorithm>
#include <stdexcept>

namespace lotus::cfl::cs_index::scs {

PolicyAutomaton::PolicyAutomaton(int state_count, int initial_state, Kind kind)
    : state_count_(state_count), initial_state_(initial_state), kind_(kind) {
  if (state_count <= 0)
    throw std::invalid_argument("Policy automata require at least one state");
  validateState(initial_state);
}

int PolicyAutomaton::stateCount() const { return state_count_; }

int PolicyAutomaton::initialState() const { return initial_state_; }

PolicyAutomaton::Kind PolicyAutomaton::kind() const { return kind_; }

void PolicyAutomaton::addAcceptingState(int state) {
  validateState(state);
  accepting_states_.insert(state);
}

bool PolicyAutomaton::isAccepting(int state) const {
  validateState(state);
  return accepting_states_.count(state);
}

const std::set<int> &PolicyAutomaton::acceptingStates() const {
  return accepting_states_;
}

void PolicyAutomaton::addTransition(int source_state, int event_label,
                                    int target_state) {
  validateState(source_state);
  validateState(target_state);
  if (event_label == 0) {
    throw std::invalid_argument(
        "Event label zero is reserved for the identity transition");
  }

  std::vector<int> &targets = transitions_[{source_state, event_label}];
  if (kind_ == Kind::DFA && !targets.empty() &&
      targets.front() != target_state) {
    throw std::invalid_argument(
        "A DFA state/event pair cannot have multiple targets");
  }
  if (std::find(targets.begin(), targets.end(), target_state) == targets.end())
    targets.push_back(target_state);
}

void PolicyAutomaton::addIdentityTransitions(int event_label) {
  if (event_label == 0)
    return;
  for (int state = 0; state < state_count_; ++state)
    addTransition(state, event_label, state);
}

std::vector<int> PolicyAutomaton::successors(int state, int event_label) const {
  validateState(state);
  if (event_label == 0)
    return {state};

  const auto transition_it = transitions_.find({state, event_label});
  if (transition_it == transitions_.end())
    return {};
  return transition_it->second;
}

bool PolicyAutomaton::validate(const std::set<int> &observed_events,
                               std::string *error) const {
  if (accepting_states_.empty()) {
    if (error)
      *error = "The policy has no accepting state";
    return false;
  }

  if (kind_ == Kind::NFA)
    return true;

  for (int state = 0; state < state_count_; ++state) {
    for (int event : observed_events) {
      const auto transition_it = transitions_.find({state, event});
      if (transition_it == transitions_.end() ||
          transition_it->second.size() != 1) {
        if (error) {
          *error = "The DFA is not total for state " + std::to_string(state) +
                   " and event " + std::to_string(event);
        }
        return false;
      }
    }
  }
  return true;
}

void PolicyAutomaton::validateState(int state) const {
  if (state < 0 || state >= state_count_)
    throw std::out_of_range("Policy state is out of range");
}

} // namespace lotus::cfl::cs_index::scs
