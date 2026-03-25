//===-- Verification/Sifa/RegexDag/RegexDagNode.h -------------------------===//
//
// Regex DAG node (ported from Ultimate Library-Sifa).
//
// A RegexDag represents a regex where:
//  - concatenation is represented by edges (a linear chain),
//  - union is represented by forks/joins (epsilon nodes),
//  - stars are treated like literals and stored as a single node content.
//
// Node content is typically one of:
//  - Literal
//  - Star
//  - Epsilon
//  - EmptySet
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXDAGNODE_H
#define LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXDAGNODE_H

#include "Utils/Algorithms/PathExpressions/Regex.h"

#include <algorithm>
#include <vector>

namespace lotus {
namespace sifa {

template <typename L> class RegexDagNode final {
public:
  using Label = L;
  using RegexRef = lotus::pathexpressions::RegexRef<L>;

  explicit RegexDagNode(RegexRef content) : content_(std::move(content)) {}

  const RegexRef &getContent() const { return content_; }

  bool isEpsilon() const { return content_ && content_->isEpsilon(); }

  const std::vector<RegexDagNode<L> *> &getOutgoingNodes() const {
    return outgoing_;
  }
  const std::vector<RegexDagNode<L> *> &getIncomingNodes() const {
    return incoming_;
  }

  void connectOutgoing(RegexDagNode<L> *succ) {
    if (!succ || succ == this) {
      return;
    }
    if (std::find(outgoing_.begin(), outgoing_.end(), succ) !=
        outgoing_.end()) {
      return;
    }
    outgoing_.push_back(succ);
    succ->connectIncomingInternal(this);
  }

  void connectIncoming(RegexDagNode<L> *pred) {
    if (!pred || pred == this) {
      return;
    }
    if (std::find(incoming_.begin(), incoming_.end(), pred) !=
        incoming_.end()) {
      return;
    }
    incoming_.push_back(pred);
    pred->connectOutgoingInternal(this);
  }

  void removeOutgoing(RegexDagNode<L> *succ) {
    outgoing_.erase(std::remove(outgoing_.begin(), outgoing_.end(), succ),
                    outgoing_.end());
  }

  void removeIncoming(RegexDagNode<L> *pred) {
    incoming_.erase(std::remove(incoming_.begin(), incoming_.end(), pred),
                    incoming_.end());
  }

private:
  void connectOutgoingInternal(RegexDagNode<L> *succ) {
    if (std::find(outgoing_.begin(), outgoing_.end(), succ) ==
        outgoing_.end()) {
      outgoing_.push_back(succ);
    }
  }

  void connectIncomingInternal(RegexDagNode<L> *pred) {
    if (std::find(incoming_.begin(), incoming_.end(), pred) ==
        incoming_.end()) {
      incoming_.push_back(pred);
    }
  }

  RegexRef content_;
  std::vector<RegexDagNode<L> *> outgoing_;
  std::vector<RegexDagNode<L> *> incoming_;
};

} // namespace sifa
} // namespace lotus

#include "Verification/Sifa/Cfg/Transition.h"
extern template class lotus::sifa::RegexDagNode<lotus::sifa::Transition>;

#endif // LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXDAGNODE_H
