//===-- Verification/Sifa/RegexDag/BackwardClosedOverlay.h ----------------===//
//
// Backward-closed overlay (ported from Ultimate Library-Sifa).
//
// predecessorsOf() always returns all predecessors included in the overlay.
// The overlay has exactly one source (the DAG source) but can have multiple
// sinks.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_REGEXDAG_BACKWARDCLOSEDOVERLAY_H
#define LOTUS_VERIFICATION_SIFA_REGEXDAG_BACKWARDCLOSEDOVERLAY_H

#include "Verification/Sifa/RegexDag/IDagOverlay.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace lotus {
namespace sifa {

template <typename L>
class BackwardClosedOverlay final : public IDagOverlay<L> {
public:
  using Node = RegexDagNode<L>;
  using Dag = RegexDag<L>;

  void addExclusive(Node *targetNodeExclusive) {
    if (!targetNodeExclusive) {
      return;
    }
    for (Node *pred : targetNodeExclusive->getIncomingNodes()) {
      addInclusive(pred);
    }
  }

  void addInclusive(Node *targetNodeInclusive) {
    if (!targetNodeInclusive) {
      return;
    }

    if (successors_[targetNodeInclusive].empty()) {
      sinks_.insert(targetNodeInclusive);
    }
    addInclusiveIntern(targetNodeInclusive);
  }

  std::vector<Node *> successorsOf(Node *node) const override {
    return lookup(successors_, node);
  }

  std::vector<Node *> predecessorsOf(Node *node) const override {
    return lookup(predecessors_, node);
  }

  std::vector<Node *> sources(const Dag &dag) const override {
    return {dag.getSource()};
  }

  std::vector<Node *> sinks(const Dag &dag) const override {
    (void)dag;
    return std::vector<Node *>(sinks_.begin(), sinks_.end());
  }

private:
  static std::vector<Node *>
  lookup(const std::unordered_map<Node *, std::unordered_set<Node *>> &rel,
         Node *n) {
    auto it = rel.find(n);
    if (it == rel.end()) {
      return {};
    }
    return std::vector<Node *>(it->second.begin(), it->second.end());
  }

  bool addPair(std::unordered_map<Node *, std::unordered_set<Node *>> &rel,
               Node *a, Node *b) {
    return rel[a].insert(b).second;
  }

  void addInclusiveIntern(Node *target) {
    for (Node *pred : target->getIncomingNodes()) {
      if (addPair(successors_, pred, target)) {
        addPair(predecessors_, target, pred);
        sinks_.erase(pred);
        addInclusive(pred);
      }
    }
  }

  std::unordered_map<Node *, std::unordered_set<Node *>> predecessors_;
  std::unordered_map<Node *, std::unordered_set<Node *>> successors_;
  std::unordered_set<Node *> sinks_;
};

} // namespace sifa
} // namespace lotus

#include "Verification/Sifa/Cfg/Transition.h"
extern template class lotus::sifa::BackwardClosedOverlay<
    lotus::sifa::Transition>;

#endif // LOTUS_VERIFICATION_SIFA_REGEXDAG_BACKWARDCLOSEDOVERLAY_H
