//===-- Verification/Sifa/RegexDag/RegexDag.h -----------------------------===//
//
// RegexDAG container (ported from Ultimate Library-Sifa).
//
// Paper (TACAS 2020 "Ultimate Taipan..."): a directed acyclic graph whose
// vertices are labeled with regular expressions over the program's transitions
// (without calls/returns but with summary and enter statements for interproc.).
// Exactly one sink node per location of interest.
//
// Owns all nodes and keeps track of a single source and sink.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXDAG_H
#define LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXDAG_H

#include "Utils/Algorithms/PathExpressions/Regex.h"
#include "Verification/Sifa/RegexDag/RegexDagNode.h"

#include <memory>
#include <unordered_set>
#include <vector>

namespace lotus {
namespace sifa {

template <typename L> class RegexDag final {
public:
  using Node = RegexDagNode<L>;
  using RegexRef = typename Node::RegexRef;

  RegexDag() = default;

  RegexDag(RegexDag &&) noexcept = default;
  RegexDag &operator=(RegexDag &&) noexcept = default;

  RegexDag(const RegexDag &) = delete;
  RegexDag &operator=(const RegexDag &) = delete;

  Node *getSource() const { return source_; }
  Node *getSink() const { return sink_; }
  void setSource(Node *n) { source_ = n; }
  void setSink(Node *n) { sink_ = n; }

  Node *makeNode(RegexRef content) {
    nodes_.push_back(std::make_unique<Node>(std::move(content)));
    return nodes_.back().get();
  }

  Node *makeEpsilonNode() {
    return makeNode(lotus::pathexpressions::Regex<L>::epsilon());
  }

  Node *makeEmptySetNode() {
    return makeNode(lotus::pathexpressions::Regex<L>::emptySet());
  }

  /// Ultimate-aligned: singleNodeDag(label). DAG with one node (source = sink).
  static RegexDag singleNodeDag(RegexRef sourceSinkLabel) {
    RegexDag dag;
    Node *n = dag.makeNode(std::move(sourceSinkLabel));
    dag.setSource(n);
    dag.setSink(n);
    return dag;
  }

  /// Ultimate-aligned: makeEpsilon(). DAG representing the empty word ε.
  static RegexDag makeEpsilon() {
    return singleNodeDag(lotus::pathexpressions::Regex<L>::epsilon());
  }

  /// Ultimate-aligned: makeEmptySet(). DAG representing the never-matching
  /// regex ∅.
  static RegexDag makeEmptySet() {
    return singleNodeDag(lotus::pathexpressions::Regex<L>::emptySet());
  }

  /// Ultimate-aligned: collectNodes(). All nodes reachable from source (each
  /// once).
  std::vector<Node *> collectNodes() const {
    std::vector<Node *> out;
    std::unordered_set<Node *> visited;
    collectNodesFrom(source_, visited, out);
    return out;
  }

  const std::vector<std::unique_ptr<Node>> &nodes() const { return nodes_; }

private:
  static void collectNodesFrom(Node *cur, std::unordered_set<Node *> &visited,
                               std::vector<Node *> &out) {
    if (!cur || !visited.insert(cur).second)
      return;
    out.push_back(cur);
    for (Node *succ : cur->getOutgoingNodes()) {
      collectNodesFrom(succ, visited, out);
    }
  }

  std::vector<std::unique_ptr<Node>> nodes_;
  Node *source_ = nullptr;
  Node *sink_ = nullptr;
};

} // namespace sifa
} // namespace lotus

#include "Verification/Sifa/Cfg/Transition.h"
extern template class lotus::sifa::RegexDag<lotus::sifa::Transition>;

#endif // LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXDAG_H
