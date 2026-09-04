#include "CFL/Classical/Core/RecursiveStateMachine.h"

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

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

std::vector<std::string> split(const std::string &text, char delimiter) {
  std::vector<std::string> result;
  std::istringstream stream(text);
  for (std::string item; std::getline(stream, item, delimiter);) {
    result.push_back(trim(item));
  }
  if (!text.empty() && text.back() == delimiter) {
    result.emplace_back();
  }
  return result;
}

std::pair<std::string, std::uint32_t>
parseIndexedName(const std::string &text) {
  const auto separator = text.find_last_of('_');
  if (separator == std::string::npos || separator + 1 == text.size()) {
    return {text, 0};
  }
  const std::string suffix = text.substr(separator + 1);
  if (suffix.find_first_not_of("0123456789") != std::string::npos) {
    return {text, 0};
  }
  const std::uint64_t attribute = std::stoull(suffix);
  if (attribute > std::numeric_limits<std::uint32_t>::max()) {
    throw std::out_of_range("RSM attribute exceeds uint32_t range: " + suffix);
  }
  return {text.substr(0, separator), static_cast<std::uint32_t>(attribute)};
}

} // namespace

RecursiveStateMachine
RecursiveStateMachine::parseFromFile(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Failed to open RSM file: " + path);
  }
  std::ostringstream text;
  text << input.rdbuf();
  return parseFromText(text.str());
}

RecursiveStateMachine
RecursiveStateMachine::parseFromText(const std::string &text) {
  RecursiveStateMachine rsm;
  rsm.loadFromText(text);
  return rsm;
}

void RecursiveStateMachine::loadFromText(const std::string &text) {
  std::vector<std::pair<std::string, std::string>> declarations;
  std::istringstream input(text);
  for (std::string line; std::getline(input, line);) {
    line = trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const auto fields = split(line, '\t');
    if (fields.size() >= 3) {
      const RsmLocalState source = parseLocalState(fields[0]);
      const std::uint32_t label = internLabel(fields[1]);
      const RsmLocalState target = parseLocalState(fields[2]);
      auto &by_label = transitions_[source];
      if (!by_label.emplace(label, target).second) {
        throw std::invalid_argument(
            "RSM is not deterministic for transition: " + line);
      }
      continue;
    }
    if (fields.size() != 2) {
      throw std::invalid_argument("Malformed RSM line: " + line);
    }
    declarations.emplace_back(fields[0], fields[1]);
  }

  for (const auto &[kind, states] : declarations) {
    if (kind == "init:") {
      initial_state_ = parseGlobalState(states);
    } else if (kind == "acpt:") {
      for (const std::string &state : split(states, ';')) {
        if (!state.empty()) {
          accepting_states_.insert(parseGlobalState(state));
        }
      }
    } else {
      throw std::invalid_argument("Unknown RSM declaration: " + kind);
    }
  }
  if (!initial_state_.valid()) {
    throw std::invalid_argument("RSM has no valid initial state");
  }
}

RsmLocalState RecursiveStateMachine::parseLocalState(const std::string &text) {
  const auto parts = split(text, ',');
  if (parts.empty() || parts.back().empty()) {
    throw std::invalid_argument("Malformed RSM local state: " + text);
  }
  RsmLocalState result;
  result.local_state = internState(parts.back());
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    const auto [box, unused] = parseIndexedName(parts[index]);
    (void)unused;
    result.boxes.push_back(internBox(box));
  }
  return result;
}

RsmGlobalState
RecursiveStateMachine::parseGlobalState(const std::string &text) const {
  const auto parts = split(text, ',');
  if (parts.empty() || parts.back().empty()) {
    throw std::invalid_argument("Malformed RSM global state: " + text);
  }
  RsmGlobalState result;
  result.local_state = stateId(parts.back());
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    const auto [box, attribute] = parseIndexedName(parts[index]);
    result.boxes.push_back({boxId(box), attribute});
  }
  return result;
}

RsmLabel RecursiveStateMachine::parseLabel(const std::string &text) const {
  const auto [label, attribute] = parseIndexedName(trim(text));
  return {labelId(label), attribute};
}

RsmGlobalState RecursiveStateMachine::transition(const RsmGlobalState &source,
                                                 RsmLabel label) const {
  if (!source.valid() || !label) {
    return {};
  }

  RsmGlobalState result = source;
  RsmLocalState local_source{{}, source.local_state};
  auto transition = transitions_.find(local_source);
  if (transition != transitions_.end()) {
    const auto target = transition->second.find(label.kind);
    if (target != transition->second.end()) {
      if (target->second.boxes.empty()) {
        result.local_state = target->second.local_state;
      } else {
        result.boxes.push_back({target->second.boxes.back(), label.index});
        result.local_state = target->second.local_state;
      }
      return result;
    }
  }

  if (source.boxes.empty()) {
    return {};
  }
  local_source.boxes.push_back(source.boxes.back().kind);
  transition = transitions_.find(local_source);
  if (transition == transitions_.end()) {
    return {};
  }
  const auto target = transition->second.find(label.kind);
  if (target == transition->second.end() ||
      source.boxes.back().index != label.index) {
    return {};
  }

  result.boxes.pop_back();
  if (!target->second.boxes.empty()) {
    result.boxes.push_back({target->second.boxes.back(), label.index});
  }
  result.local_state = target->second.local_state;
  return result;
}

std::uint32_t RecursiveStateMachine::internState(const std::string &name) {
  const auto [it, inserted] =
      state_ids_.emplace(name, static_cast<std::uint32_t>(state_names_.size()));
  if (inserted) {
    state_names_.push_back(name);
  }
  return it->second;
}

std::uint32_t RecursiveStateMachine::internLabel(const std::string &name) {
  const auto [it, inserted] =
      label_ids_.emplace(name, static_cast<std::uint32_t>(label_names_.size()));
  if (inserted) {
    label_names_.push_back(name);
  }
  return it->second;
}

std::uint32_t RecursiveStateMachine::internBox(const std::string &name) {
  const auto [it, inserted] =
      box_ids_.emplace(name, static_cast<std::uint32_t>(box_names_.size()));
  if (inserted) {
    box_names_.push_back(name);
  }
  return it->second;
}

std::uint32_t RecursiveStateMachine::stateId(const std::string &name) const {
  const auto it = state_ids_.find(name);
  if (it == state_ids_.end()) {
    throw std::out_of_range("Unknown RSM state: " + name);
  }
  return it->second;
}

std::uint32_t RecursiveStateMachine::labelId(const std::string &name) const {
  const auto it = label_ids_.find(name);
  if (it == label_ids_.end()) {
    throw std::out_of_range("Unknown RSM label: " + name);
  }
  return it->second;
}

std::uint32_t RecursiveStateMachine::boxId(const std::string &name) const {
  const auto it = box_ids_.find(name);
  if (it == box_ids_.end()) {
    throw std::out_of_range("Unknown RSM box: " + name);
  }
  return it->second;
}

const std::string &RecursiveStateMachine::stateName(std::uint32_t id) const {
  return state_names_.at(id);
}

const std::string &RecursiveStateMachine::labelName(std::uint32_t id) const {
  return label_names_.at(id);
}

const std::string &RecursiveStateMachine::boxName(std::uint32_t id) const {
  return box_names_.at(id);
}

} // namespace lotus::cfl::classical
