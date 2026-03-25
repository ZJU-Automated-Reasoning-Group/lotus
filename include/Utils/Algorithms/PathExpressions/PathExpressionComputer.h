//===-- PathExpressions/PathExpressionComputer.h - Tarjan path expressions ===//
//
// Computes path expressions for a labeled graph (regular expression describing
// all paths between two nodes). Algorithm: Tarjan, "Fast Algorithms for
// Solving Path Problems", 1981, Chapter 2.
//
// Migrated from Ultimate Library-PathExpressions (v0.3.1).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_UTILS_GENERAL_PATHEXPRESSIONS_PATHEXPRESSIONCOMPUTER_H
#define LOTUS_UTILS_GENERAL_PATHEXPRESSIONS_PATHEXPRESSIONCOMPUTER_H

#include "Utils/Algorithms/PathExpressions/LabeledGraph.h"
#include "Utils/Algorithms/PathExpressions/Regex.h"

#include <cassert>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus {
namespace pathexpressions {

namespace detail {
struct IntPairHash {
  std::size_t operator()(const std::pair<int, int> &p) const {
    return std::hash<int>()(p.first) ^ (std::hash<int>()(p.second) << 1);
  }
};
} // namespace detail

/// Computes path expressions for a labeled graph.
/// For a fixed source node, returns a regex describing all paths to each node.
/// Complexity: O(n^3 + m) for n nodes, m edges (Tarjan Chapter 2).
template <typename N, typename L> class PathExpressionComputer {
public:
  using RegexRefT = RegexRef<L>;
  using Graph = ILabeledGraph<N, L>;

  explicit PathExpressionComputer(const Graph &graph) : graph_(graph) {
    mapNodesToInt();
  }

  /// Path expression (regex) from \p source to \p target.
  /// Nodes must be in the graph.
  RegexRefT exprBetween(const N &source, const N &target) {
    assert(graph_.getNodes().count(source) &&
           "Tried to compute path expression starting at non-existing node");
    assert(graph_.getNodes().count(target) &&
           "Tried to compute path expression ending at non-existing node");

    auto it = allPathsFromNode_.find(source);
    if (it == allPathsFromNode_.end()) {
      eliminate();
      auto allPathsFromSource = solve(source, extractPathSequence());
      it = allPathsFromNode_.emplace(source, std::move(allPathsFromSource))
               .first;
    }
    return it->second.at(static_cast<std::size_t>(intOf(target)));
  }

private:
  struct PathExpression {
    RegexRefT expr;
    int source;
    int target;
  };

  const Graph &graph_;
  std::unordered_map<N, int> nodeToInt_;
  std::vector<N> intToNode_;
  std::unordered_map<std::pair<int, int>, RegexRefT, detail::IntPairHash> P_;
  std::unordered_map<N, std::vector<RegexRefT>> allPathsFromNode_;
  bool eliminated_ = false;

  void mapNodesToInt() {
    int nextInt = 0;
    for (const N &node : graph_.getNodes()) {
      nodeToInt_[node] = nextInt++;
      intToNode_.push_back(node);
    }
  }

  int intOf(const N &node) const {
    auto it = nodeToInt_.find(node);
    if (it == nodeToInt_.end()) {
      throw std::invalid_argument(
          "Tried to access node which is not in the graph");
    }
    return it->second;
  }

  RegexRefT pathExpr(const int source, const int target) const {
    const auto it = P_.find({source, target});
    if (it == P_.end()) {
      return Regex<L>::emptySet();
    }
    return it->second;
  }

  void updatePathExpr(const int source, const int target, RegexRefT newExpr) {
    P_[{source, target}] = std::move(newExpr);
  }

  /// Tarjan's elimination phase: fill P(u,v) for all u,v.
  void eliminate() {
    if (eliminated_)
      return;

    const int n = static_cast<int>(graph_.getNodes().size());
    // initialization of table P(u,v) not necessary due to default values
    for (const auto &edge : graph_.getEdges()) {
      const int head = intOf(edge->getSource());
      const int tail = intOf(edge->getTarget());
      auto pht = pathExpr(head, tail);
      pht = Regex<L>::simplifiedUnion(Regex<L>::literal(edge->getLabel()), pht);
      updatePathExpr(head, tail, std::move(pht));
    }

    for (int v = 0; v < n; v++) {
      auto pvv = pathExpr(v, v);
      pvv = Regex<L>::simplifiedStar(pvv);
      updatePathExpr(v, v, pvv);
      for (int u = v + 1; u < n; u++) {
        auto puv = pathExpr(u, v);
        if (puv->isEmptySet()) {
          continue;
        }
        puv = Regex<L>::simplifiedConcatenation(puv, pvv);
        updatePathExpr(u, v, puv);
        for (int w = v + 1; w < n; w++) {
          const auto pvw = pathExpr(v, w);
          if (pvw->isEmptySet()) {
            continue;
          }
          const auto oldPuw = pathExpr(u, w);
          const auto a = Regex<L>::simplifiedConcatenation(puv, pvw);
          const auto puw = Regex<L>::simplifiedUnion(oldPuw, a);
          updatePathExpr(u, w, puw);
        }
      }
    }
    eliminated_ = true;
  }

  std::vector<PathExpression> extractPathSequence() const {
    const int n = static_cast<int>(graph_.getNodes().size());
    std::vector<PathExpression> pathSequence;
    for (int u = 0; u < n; u++) {
      for (int w = u; w < n; w++) {
        const auto reg = pathExpr(u, w);
        if (!reg->isEmptySet() && !reg->isEpsilon()) {
          pathSequence.push_back({reg, u, w});
        }
      }
    }
    for (int u = n - 1; u >= 0; u--) {
      for (int w = 0; w < u; w++) {
        const auto reg = pathExpr(u, w);
        if (!reg->isEmptySet()) {
          pathSequence.push_back({reg, u, w});
        }
      }
    }
    return pathSequence;
  }

  std::vector<RegexRefT>
  solve(const N &source,
        const std::vector<PathExpression> &pathSequence) const {
    const std::size_t n = graph_.getNodes().size();
    std::vector<RegexRefT> allPathsFromSource(n, Regex<L>::emptySet());
    allPathsFromSource.at(static_cast<std::size_t>(intOf(source))) =
        Regex<L>::epsilon();

    for (const auto &seqElement : pathSequence) {
      if (seqElement.source == seqElement.target) {
        const int vi = seqElement.source;
        const auto regexVi =
            allPathsFromSource.at(static_cast<std::size_t>(vi));
        allPathsFromSource.at(static_cast<std::size_t>(vi)) =
            Regex<L>::simplifiedConcatenation(regexVi, seqElement.expr);
      } else {
        const int vi = seqElement.source;
        const int wi = seqElement.target;
        const auto regexVi =
            allPathsFromSource.at(static_cast<std::size_t>(vi));
        const auto inter =
            Regex<L>::simplifiedConcatenation(regexVi, seqElement.expr);
        const auto regexWi =
            allPathsFromSource.at(static_cast<std::size_t>(wi));
        allPathsFromSource.at(static_cast<std::size_t>(wi)) =
            Regex<L>::simplifiedUnion(regexWi, inter);
      }
    }
    return allPathsFromSource;
  }
};

} // namespace pathexpressions
} // namespace lotus

#endif // LOTUS_UTILS_GENERAL_PATHEXPRESSIONS_PATHEXPRESSIONCOMPUTER_H
