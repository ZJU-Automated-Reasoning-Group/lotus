#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus::cfl::classical {

struct RsmLabel {
  std::uint32_t kind = 0;
  std::uint32_t index = 0;

  bool operator==(const RsmLabel &other) const {
    return kind == other.kind && index == other.index;
  }
  bool operator<(const RsmLabel &other) const {
    return std::tie(kind, index) < std::tie(other.kind, other.index);
  }
  explicit operator bool() const { return kind != 0; }
};

struct IndexedRsmBox {
  std::uint32_t kind = 0;
  std::uint32_t index = 0;

  bool operator==(const IndexedRsmBox &other) const {
    return kind == other.kind && index == other.index;
  }
  bool operator<(const IndexedRsmBox &other) const {
    return std::tie(kind, index) < std::tie(other.kind, other.index);
  }
};

struct RsmGlobalState {
  std::vector<IndexedRsmBox> boxes;
  std::uint32_t local_state = 0;

  bool valid() const { return local_state != 0; }
  bool operator==(const RsmGlobalState &other) const {
    return boxes == other.boxes && local_state == other.local_state;
  }
  bool operator<(const RsmGlobalState &other) const {
    return std::tie(boxes, local_state) <
           std::tie(other.boxes, other.local_state);
  }
};

struct RsmLocalState {
  std::vector<std::uint32_t> boxes;
  std::uint32_t local_state = 0;

  bool operator<(const RsmLocalState &other) const {
    return std::tie(boxes, local_state) <
           std::tie(other.boxes, other.local_state);
  }
};

/// Deterministic recursive state machine used by POCR's graph-folding proof.
class RecursiveStateMachine {
public:
  using TransitionMap =
      std::map<RsmLocalState, std::map<std::uint32_t, RsmLocalState>>;

  static RecursiveStateMachine parseFromFile(const std::string &path);
  static RecursiveStateMachine parseFromText(const std::string &text);

  RsmGlobalState transition(const RsmGlobalState &source, RsmLabel label) const;
  RsmGlobalState parseGlobalState(const std::string &text) const;
  RsmLabel parseLabel(const std::string &text) const;

  bool isAccepting(const RsmGlobalState &state) const {
    return accepting_states_.count(state) != 0;
  }
  const RsmGlobalState &initialState() const { return initial_state_; }
  const std::set<RsmGlobalState> &acceptingStates() const {
    return accepting_states_;
  }
  const TransitionMap &transitions() const { return transitions_; }
  const std::unordered_map<std::string, std::uint32_t> &labels() const {
    return label_ids_;
  }
  const std::unordered_map<std::string, std::uint32_t> &boxes() const {
    return box_ids_;
  }

  std::uint32_t stateId(const std::string &name) const;
  std::uint32_t labelId(const std::string &name) const;
  std::uint32_t boxId(const std::string &name) const;
  const std::string &stateName(std::uint32_t id) const;
  const std::string &labelName(std::uint32_t id) const;
  const std::string &boxName(std::uint32_t id) const;

private:
  void loadFromText(const std::string &text);
  std::uint32_t internState(const std::string &name);
  std::uint32_t internLabel(const std::string &name);
  std::uint32_t internBox(const std::string &name);
  RsmLocalState parseLocalState(const std::string &text);

  std::unordered_map<std::string, std::uint32_t> state_ids_;
  std::unordered_map<std::string, std::uint32_t> label_ids_;
  std::unordered_map<std::string, std::uint32_t> box_ids_;
  std::vector<std::string> state_names_{""};
  std::vector<std::string> label_names_{""};
  std::vector<std::string> box_names_{""};
  TransitionMap transitions_;
  RsmGlobalState initial_state_;
  std::set<RsmGlobalState> accepting_states_;
};

} // namespace lotus::cfl::classical
