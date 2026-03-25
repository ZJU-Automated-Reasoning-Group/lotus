//===-- Verification/Sifa/RegexDag/IDagOverlay.h --------------------------===//
//
// Overlay interface for RegexDag traversal (ported from Ultimate Library-Sifa).
//
// An overlay restricts which edges of a RegexDag are visible to an algorithm.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_REGEXDAG_IDAGOVERLAY_H
#define LOTUS_VERIFICATION_SIFA_REGEXDAG_IDAGOVERLAY_H

#include "Verification/Sifa/RegexDag/RegexDag.h"

#include <vector>

namespace lotus {
namespace sifa {

template <typename L> class IDagOverlay {
public:
  using Node = RegexDagNode<L>;
  using Dag = RegexDag<L>;

  virtual ~IDagOverlay() = default;

  virtual std::vector<Node *> successorsOf(Node *node) const = 0;
  virtual std::vector<Node *> predecessorsOf(Node *node) const = 0;

  virtual std::vector<Node *> sources(const Dag &dag) const = 0;
  virtual std::vector<Node *> sinks(const Dag &dag) const = 0;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_REGEXDAG_IDAGOVERLAY_H
