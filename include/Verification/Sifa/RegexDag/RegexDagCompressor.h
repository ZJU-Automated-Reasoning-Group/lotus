//===-- Verification/Sifa/RegexDag/RegexDagCompressor.h -------------------===//
//
// RegexDag compression by node merging (ported from Ultimate Library-Sifa).
//
// Best-effort compression:
//  - not necessarily minimal,
//  - not canonical,
//  - but idempotent.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXDAGCOMPRESSOR_H
#define LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXDAGCOMPRESSOR_H

#include "Verification/Sifa/RegexDag/RegexDag.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus {
namespace sifa {

template <typename L> struct RegexRefHash {
  std::size_t operator()(const lotus::pathexpressions::RegexRef<L> &r) const {
    return r ? r->hashCode() : 0;
  }
};

template <typename L> struct RegexRefEq {
  bool operator()(const lotus::pathexpressions::RegexRef<L> &a,
                  const lotus::pathexpressions::RegexRef<L> &b) const {
    if (a == b)
      return true;
    if (!a || !b)
      return false;
    return a->equals(*b);
  }
};

template <typename L> class RegexDagCompressor final {
public:
  using Node = RegexDagNode<L>;
  using Dag = RegexDag<L>;
  using RegexRef = lotus::pathexpressions::RegexRef<L>;

  Dag &compress(Dag &dag) {
    dag_ = &dag;
    mergedFlag_ = true;
    while (mergedFlag_) {
      mergedFlag_ = false;
      searchAndMerge(dag_->getSource(), /*forward=*/true);
      searchAndMerge(dag_->getSink(), /*forward=*/false);
    }
    return *dag_;
  }

private:
  void searchAndMerge(Node *startNode, bool forward) {
    std::unordered_set<Node *> visited;
    std::queue<Node *> work;
    visited.insert(startNode);
    work.push(startNode);

    while (!work.empty()) {
      Node *predator = work.front();
      work.pop();

      mergeInDirection(predator, forward);

      const auto &next =
          forward ? predator->getOutgoingNodes() : predator->getIncomingNodes();
      for (Node *n : next) {
        if (visited.insert(n).second) {
          work.push(n);
        }
      }
      eliminateIfEpsilon(predator);
    }
  }

  void mergeInDirection(Node *baseNode, bool forward) {
    mergeTable_.clear();
    const std::unordered_set<Node *> candidates =
        safeCandidates(baseNode, forward);
    for (Node *c : candidates) {
      mergeTable_[c->getContent()].insert(c);
    }
    for (auto &kv : mergeTable_) {
      groupToSingleNode(kv.first, kv.second);
    }
  }

  std::unordered_set<Node *> safeCandidates(Node *base, bool forward) {
    std::unordered_set<Node *> candidates;
    const auto &direct =
        forward ? base->getOutgoingNodes() : base->getIncomingNodes();
    for (Node *n : direct) {
      candidates.insert(n);
    }

    // Remove candidates that are transitively reachable from other candidates.
    std::unordered_set<Node *> transitivelyReachable;
    for (Node *start : candidates) {
      std::queue<Node *> q;
      std::unordered_set<Node *> seen;
      q.push(start);
      seen.insert(start);
      while (!q.empty()) {
        Node *cur = q.front();
        q.pop();
        const auto &nbrs =
            forward ? cur->getOutgoingNodes() : cur->getIncomingNodes();
        for (Node *n : nbrs) {
          if (seen.insert(n).second) {
            if (candidates.count(n)) {
              transitivelyReachable.insert(n);
            }
            q.push(n);
          }
        }
      }
    }

    for (Node *n : transitivelyReachable) {
      candidates.erase(n);
    }
    return candidates;
  }

  Node *groupToSingleNode(const RegexRef &label,
                          const std::unordered_set<Node *> &mergeGroup) {
    if (mergeGroup.size() <= 1) {
      return *mergeGroup.begin();
    }
    Node *merged = dag_->makeNode(label);
    for (Node *prey : mergeGroup) {
      merge(merged, prey);
    }
    return merged;
  }

  void merge(Node *predator, Node *prey) {
    mergedFlag_ = true;

    // Remove prey from its neighbors.
    for (Node *in : prey->getIncomingNodes()) {
      in->removeOutgoing(prey);
    }
    for (Node *out : prey->getOutgoingNodes()) {
      out->removeIncoming(prey);
    }

    // Copy incoming edges to predator.
    std::unordered_set<Node *> ignore(predator->getIncomingNodes().begin(),
                                      predator->getIncomingNodes().end());
    ignore.insert(predator);
    for (Node *in : prey->getIncomingNodes()) {
      if (!ignore.count(in)) {
        predator->connectIncoming(in);
      }
    }

    // Copy outgoing edges to predator.
    ignore.clear();
    ignore.insert(predator->getOutgoingNodes().begin(),
                  predator->getOutgoingNodes().end());
    ignore.insert(predator);
    for (Node *out : prey->getOutgoingNodes()) {
      if (!ignore.count(out)) {
        predator->connectOutgoing(out);
      }
    }

    if (prey == dag_->getSink()) {
      dag_->setSink(predator);
    }
    if (prey == dag_->getSource()) {
      dag_->setSource(predator);
    }
  }

  void eliminateIfEpsilon(Node *eps) {
    if (!eps || !eps->isEpsilon()) {
      return;
    }

    const auto &in0 = eps->getIncomingNodes();
    const auto &out0 = eps->getOutgoingNodes();
    if (in0.empty() && out0.empty()) {
      return;
    }

    if (eps == dag_->getSource()) {
      if (out0.size() != 1) {
        return;
      }
      dag_->setSource(out0.front());
    } else if (eps == dag_->getSink()) {
      if (in0.size() != 1) {
        return;
      }
      dag_->setSink(in0.front());
    }

    std::vector<Node *> in(in0.begin(), in0.end());
    std::vector<Node *> out(out0.begin(), out0.end());
    for (Node *inNode : in) {
      for (Node *outNode : out) {
        inNode->connectOutgoing(outNode);
      }
    }
    for (Node *inNode : in) {
      inNode->removeOutgoing(eps);
    }
    for (Node *outNode : out) {
      outNode->removeIncoming(eps);
    }
  }

  Dag *dag_ = nullptr;
  bool mergedFlag_ = false;

  std::unordered_map<RegexRef, std::unordered_set<Node *>, RegexRefHash<L>,
                     RegexRefEq<L>>
      mergeTable_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXDAGCOMPRESSOR_H
