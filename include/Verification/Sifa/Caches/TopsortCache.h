//===-- Verification/Sifa/Caches/TopsortCache.h ---------------------------===//
//
// Topological sort cache for RegexDags (ported from Ultimate Sifa).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_CACHES_TOPSORTCACHE_H
#define LOTUS_VERIFICATION_SIFA_CACHES_TOPSORTCACHE_H

#include "Verification/Sifa/RegexDag/RegexDag.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus {
namespace sifa {

template <typename L> class TopsortCache final {
public:
  using Dag = RegexDag<L>;
  using Node = RegexDagNode<L>;

  std::vector<Node *> topsort(const Dag &dag) {
    const auto it = cache_.find(&dag);
    if (it != cache_.end() && isStillValid(dag, it->second)) {
      return it->second;
    }
    auto order = compute(dag);
    cache_[&dag] = order;
    return order;
  }

private:
  std::vector<Node *> compute(const Dag &dag);
  bool isStillValid(const Dag &dag, const std::vector<Node *> &order) const;

  std::unordered_map<const Dag *, std::vector<Node *>> cache_;
};

template <typename L>
bool TopsortCache<L>::isStillValid(const Dag &dag,
                                   const std::vector<Node *> &order) const {
  std::unordered_set<Node *> nodes;
  std::queue<Node *> q;
  if (dag.getSource()) {
    q.push(dag.getSource());
    nodes.insert(dag.getSource());
  }
  while (!q.empty()) {
    Node *cur = q.front();
    q.pop();
    for (Node *n : cur->getOutgoingNodes()) {
      if (nodes.insert(n).second) {
        q.push(n);
      }
    }
  }

  if (order.size() != nodes.size()) {
    return false;
  }

  std::unordered_map<Node *, std::size_t> pos;
  pos.reserve(order.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    Node *n = order[i];
    if (nodes.erase(n) == 0) {
      return false;
    }
    pos.emplace(n, i);
  }
  if (!nodes.empty()) {
    return false;
  }

  for (const auto &kv : pos) {
    Node *src = kv.first;
    const std::size_t srcPos = kv.second;
    for (Node *dst : src->getOutgoingNodes()) {
      auto dstIt = pos.find(dst);
      if (dstIt != pos.end() && srcPos >= dstIt->second) {
        return false;
      }
    }
  }
  return true;
}

template <typename L>
std::vector<typename TopsortCache<L>::Node *>
TopsortCache<L>::compute(const Dag &dag) {
  using Node = typename TopsortCache<L>::Node;
  // Work on reachable subgraph from source.
  std::unordered_set<Node *> nodes;
  std::queue<Node *> q;
  if (dag.getSource()) {
    q.push(dag.getSource());
    nodes.insert(dag.getSource());
  }
  while (!q.empty()) {
    Node *cur = q.front();
    q.pop();
    for (Node *n : cur->getOutgoingNodes()) {
      if (nodes.insert(n).second) {
        q.push(n);
      }
    }
  }

  std::unordered_map<Node *, int> indeg;
  for (Node *n : nodes) {
    indeg[n] = 0;
  }
  for (Node *n : nodes) {
    for (Node *m : n->getOutgoingNodes()) {
      if (nodes.count(m)) {
        indeg[m] += 1;
      }
    }
  }

  std::queue<Node *> zeros;
  for (auto &kv : indeg) {
    if (kv.second == 0)
      zeros.push(kv.first);
  }

  std::vector<Node *> out;
  while (!zeros.empty()) {
    Node *n = zeros.front();
    zeros.pop();
    out.push_back(n);
    for (Node *m : n->getOutgoingNodes()) {
      if (!nodes.count(m))
        continue;
      int &next = indeg[m];
      next -= 1;
      if (next == 0)
        zeros.push(m);
    }
  }
  return out;
}

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_CACHES_TOPSORTCACHE_H
