#include "CFL/Classical/Solvers/Preprocessing/RSMFoldability.h"

#include <stdexcept>
#include <vector>

namespace lotus::cfl::classical {
namespace {

std::string trim(const std::string &text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::vector<std::string> splitPreservingEmpty(const std::string &text,
                                              char delimiter) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (true) {
    const auto next = text.find(delimiter, start);
    result.push_back(trim(text.substr(start, next - start)));
    if (next == std::string::npos) {
      return result;
    }
    start = next + 1;
  }
}

} // namespace

NodePairPattern NodePairPattern::parse(const std::string &text,
                                       const RecursiveStateMachine &rsm) {
  const auto fields = splitPreservingEmpty(text, ';');
  if (fields.size() != 8) {
    throw std::invalid_argument(
        "A foldability pattern must contain eight semicolon fields");
  }
  if ((fields[0] != "0" && fields[0] != "1") ||
      (fields[1] != "0" && fields[1] != "1")) {
    throw std::invalid_argument("Pattern source fields must be 0 or 1");
  }

  std::array<std::set<RsmLabel>, LabelSetCount> labels;
  for (std::size_t index = 0; index < LabelSetCount; ++index) {
    if (fields[index + 2].empty()) {
      continue;
    }
    for (const std::string &label :
         splitPreservingEmpty(fields[index + 2], ',')) {
      if (!label.empty()) {
        labels[index].insert(rsm.parseLabel(label));
      }
    }
  }
  return {fields[0] == "1", fields[1] == "1", std::move(labels)};
}

bool FoldabilityChecker::subsumes(const RsmGlobalState &first,
                                  const RsmGlobalState &second,
                                  const std::set<RsmLabel> &labels) const {
  if (!first.valid()) {
    return true;
  }
  if (rsm_.isAccepting(first) && !rsm_.isAccepting(second)) {
    return false;
  }
  for (RsmLabel label : labels) {
    const RsmGlobalState first_target = rsm_.transition(first, label);
    if (first_target.valid() &&
        !(first_target == rsm_.transition(second, label))) {
      return false;
    }
  }
  return true;
}

std::set<RsmGlobalState>
FoldabilityChecker::targetStates(const std::set<RsmLabel> &labels) const {
  std::set<RsmGlobalState> result;
  for (RsmLabel label : labels) {
    for (const auto &[local_source, transitions] : rsm_.transitions()) {
      if (transitions.count(label.kind) == 0) {
        continue;
      }
      RsmGlobalState source;
      source.local_state = local_source.local_state;
      if (!local_source.boxes.empty()) {
        source.boxes.push_back({local_source.boxes.back(), label.index});
      }
      const RsmGlobalState target = rsm_.transition(source, label);
      if (target.valid()) {
        result.insert(target);
      }
    }
  }
  return result;
}

bool FoldabilityChecker::check(
    const std::array<std::set<RsmLabel>, 3> &x_labels,
    const std::array<std::set<RsmLabel>, 3> &y_labels, bool x_is_source) const {
  const auto &incoming_to_x = x_labels[0];
  const auto &x_to_y = x_labels[1];
  const auto &outgoing_from_x = x_labels[2];
  const auto &incoming_to_y = y_labels[0];
  const auto &y_to_x = y_labels[1];
  const auto &outgoing_from_y = y_labels[2];
  (void)incoming_to_y;

  std::set<RsmGlobalState> incoming_targets = targetStates(incoming_to_x);
  std::set<RsmGlobalState> reverse_targets = targetStates(y_to_x);
  if (x_is_source) {
    incoming_targets.insert(rsm_.initialState());
    reverse_targets.insert(rsm_.initialState());
  }

  std::set<RsmGlobalState> x_states = incoming_targets;
  std::set<RsmGlobalState> non_reverse_x_states = incoming_targets;
  x_states.insert(reverse_targets.begin(), reverse_targets.end());

  std::vector<RsmGlobalState> seeds(x_states.begin(), x_states.end());
  for (const RsmGlobalState &seed : seeds) {
    if (seed.boxes.empty()) {
      continue;
    }
    for (const auto &[first_box, _] : rsm_.boxes()) {
      for (const auto &[second_box, unused] : rsm_.boxes()) {
        (void)unused;
        RsmGlobalState nested = seed;
        nested.boxes.insert(nested.boxes.begin(), {rsm_.boxId(first_box), 0});
        nested.boxes.insert(nested.boxes.begin(), {rsm_.boxId(second_box), 0});
        x_states.insert(nested);
        if (incoming_targets.count(seed) != 0) {
          non_reverse_x_states.insert(nested);
        }
      }
    }
  }

  for (RsmGlobalState x_state : x_states) {
    for (RsmLabel xy_label : x_to_y) {
      if (!x_state.boxes.empty()) {
        x_state.boxes.back().index = xy_label.index;
      }
      RsmGlobalState y_state = rsm_.transition(x_state, xy_label);
      if (non_reverse_x_states.count(x_state) != 0 &&
          (!subsumes(x_state, y_state, outgoing_from_y) ||
           !subsumes(y_state, x_state, outgoing_from_y))) {
        return false;
      }
      if (!y_state.valid()) {
        continue;
      }
      for (RsmLabel yx_label : y_to_x) {
        if (!y_state.boxes.empty() &&
            y_state.boxes.size() <= x_state.boxes.size()) {
          y_state.boxes.back().index = yx_label.index;
        }
        const RsmGlobalState next_x = rsm_.transition(y_state, yx_label);
        if (next_x.valid() && !subsumes(next_x, x_state, outgoing_from_x)) {
          return false;
        }
      }
    }
  }
  return true;
}

bool FoldabilityChecker::isFoldable(const NodePairPattern &pattern) const {
  if (pattern.labels(NodePairPattern::YToX).empty() &&
      (pattern.yIsSource() ||
       !pattern.labels(NodePairPattern::IncomingToY).empty())) {
    return false;
  }

  const std::array<std::set<RsmLabel>, 3> x_labels{
      pattern.labels(NodePairPattern::IncomingToX),
      pattern.labels(NodePairPattern::XToY),
      pattern.labels(NodePairPattern::OutgoingFromX)};
  const std::array<std::set<RsmLabel>, 3> y_labels{
      pattern.labels(NodePairPattern::IncomingToY),
      pattern.labels(NodePairPattern::YToX),
      pattern.labels(NodePairPattern::OutgoingFromY)};
  return check(x_labels, y_labels, pattern.xIsSource()) &&
         check(y_labels, x_labels, pattern.yIsSource());
}

} // namespace lotus::cfl::classical
