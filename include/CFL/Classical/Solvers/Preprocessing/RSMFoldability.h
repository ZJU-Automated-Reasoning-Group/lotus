#pragma once

#include "CFL/Classical/Core/RecursiveStateMachine.h"

#include <array>
#include <set>
#include <string>
#include <utility>

namespace lotus::cfl::classical {

class NodePairPattern {
public:
  enum LabelSet : std::size_t {
    IncomingToX = 0,
    IncomingToY,
    XToY,
    YToX,
    OutgoingFromX,
    OutgoingFromY,
    LabelSetCount,
  };

  NodePairPattern() = default;
  NodePairPattern(bool x_is_source, bool y_is_source,
                  std::array<std::set<RsmLabel>, LabelSetCount> labels)
      : x_is_source_(x_is_source), y_is_source_(y_is_source),
        labels_(std::move(labels)) {}

  static NodePairPattern parse(const std::string &text,
                               const RecursiveStateMachine &rsm);

  bool xIsSource() const { return x_is_source_; }
  bool yIsSource() const { return y_is_source_; }
  const std::set<RsmLabel> &labels(LabelSet set) const {
    return labels_.at(set);
  }

private:
  bool x_is_source_ = false;
  bool y_is_source_ = false;
  std::array<std::set<RsmLabel>, LabelSetCount> labels_;
};

class FoldabilityChecker {
public:
  explicit FoldabilityChecker(const RecursiveStateMachine &rsm) : rsm_(rsm) {}

  bool isFoldable(const NodePairPattern &pattern) const;

private:
  bool check(const std::array<std::set<RsmLabel>, 3> &x_labels,
             const std::array<std::set<RsmLabel>, 3> &y_labels,
             bool x_is_source) const;
  bool subsumes(const RsmGlobalState &first, const RsmGlobalState &second,
                const std::set<RsmLabel> &labels) const;
  std::set<RsmGlobalState> targetStates(const std::set<RsmLabel> &labels) const;

  const RecursiveStateMachine &rsm_;
};

} // namespace lotus::cfl::classical
