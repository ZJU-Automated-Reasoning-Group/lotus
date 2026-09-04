#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace lotus::cfl::cs_index::scs {

class PolicyAutomaton {
public:
  enum class Kind { DFA, NFA };

  PolicyAutomaton(int state_count, int initial_state, Kind kind = Kind::DFA);

  int stateCount() const;
  int initialState() const;
  Kind kind() const;

  void addAcceptingState(int state);
  bool isAccepting(int state) const;
  const std::set<int> &acceptingStates() const;

  void addTransition(int source_state, int event_label, int target_state);
  void addIdentityTransitions(int event_label);
  std::vector<int> successors(int state, int event_label) const;

  /** Validate the automaton for the event alphabet observed in a graph. */
  bool validate(const std::set<int> &observed_events,
                std::string *error = nullptr) const;

private:
  void validateState(int state) const;

  int state_count_;
  int initial_state_;
  Kind kind_;
  std::set<int> accepting_states_;
  std::map<std::pair<int, int>, std::vector<int>> transitions_;
};

} // namespace lotus::cfl::cs_index::scs
