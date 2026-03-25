//===-- Verification/Sifa/RegexDag/FullOverlay.h --------------------------===//
//
// Overlay exposing all edges of a RegexDag (ported from Ultimate Library-Sifa).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_REGEXDAG_FULLOVERLAY_H
#define LOTUS_VERIFICATION_SIFA_REGEXDAG_FULLOVERLAY_H

#include "Verification/Sifa/RegexDag/IDagOverlay.h"

namespace lotus {
namespace sifa {

template <typename L> class FullOverlay final : public IDagOverlay<L> {
public:
  using Node = RegexDagNode<L>;
  using Dag = RegexDag<L>;

  std::vector<Node *> successorsOf(Node *node) const override {
    return node ? std::vector<Node *>(node->getOutgoingNodes().begin(),
                                      node->getOutgoingNodes().end())
                : std::vector<Node *>{};
  }

  std::vector<Node *> predecessorsOf(Node *node) const override {
    return node ? std::vector<Node *>(node->getIncomingNodes().begin(),
                                      node->getIncomingNodes().end())
                : std::vector<Node *>{};
  }

  std::vector<Node *> sources(const Dag &dag) const override {
    return {dag.getSource()};
  }
  std::vector<Node *> sinks(const Dag &dag) const override {
    return {dag.getSink()};
  }
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_REGEXDAG_FULLOVERLAY_H
