//===- DOD.cpp - Decisive-order dependence algorithms --------------------===//
//
// Adapted from dg's generic DOD algorithms (MIT license).
// The implementation uses a small Lotus-owned graph and standard containers.
//
//===----------------------------------------------------------------------===//

#include "Analysis/ControlDependence/DOD.h"

#include "llvm/ADT/SparseBitVector.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace lotus::cd::detail {
namespace {

template <typename T> class SetQueue {
public:
  void push(T value) {
    if (m_present.insert(value).second)
      m_queue.push_back(value);
  }
  bool empty() const { return m_queue.empty(); }
  T popFront() {
    T value = m_queue.front();
    m_queue.pop_front();
    return value;
  }
  T popBack() {
    T value = m_queue.back();
    m_queue.pop_back();
    return value;
  }

private:
  std::deque<T> m_queue;
  std::unordered_set<T> m_present;
};

using IDSet = llvm::SparseBitVector<>;
using AllMaxPathResult = std::map<GraphNode *, IDSet>;

AllMaxPathResult computeAllMaxPaths(Graph &graph) {
  AllMaxPathResult result;
  std::unordered_map<GraphNode *, IDSet> colors;
  std::unordered_map<GraphNode *, size_t> counters;

  for (GraphNode *target : graph.nodes()) {
    for (GraphNode *node : graph.nodes())
      counters[node] = node->successors().size();

    colors[target].set(target->getID());
    std::vector<GraphNode *> worklist{target};
    while (!worklist.empty()) {
      GraphNode *node = worklist.back();
      worklist.pop_back();
      for (GraphNode *pred : node->predecessors()) {
        size_t &counter = counters[pred];
        assert(counter > 0);
        --counter;
        if (counter == 0 && !colors[pred].test(target->getID())) {
          colors[pred].set(target->getID());
          worklist.push_back(pred);
        }
      }
    }
  }

  for (GraphNode *node : graph.nodes())
    result.emplace(node, std::move(colors[node]));
  return result;
}

bool contains(const IDSet &set, const GraphNode *node) {
  return set.test(node->getID());
}

class DODComputer {
public:
  DependenceResult compute(Graph &graph, bool includeNTSCD) {
    DependenceMap dependencies;
    DependenceMap dependents;
    AllMaxPathResult allPaths = computeAllMaxPaths(graph);

    for (GraphNode *predicate : graph.predicates()) {
      // The DOD relation is defined here for binary decisions, as in dg.
      // Multi-way LLVM switches are ignored rather than asserting.
      if (predicate->successors().size() != 2)
        continue;
      computeDOD(predicate, graph, allPaths, dependencies, dependents);
      if (includeNTSCD)
        computeNTSCD(predicate, graph, allPaths, dependencies, dependents);
    }
    return {std::move(dependencies), std::move(dependents)};
  }

private:
  struct ColoredAP {
    Graph graph;
    IDSet blue;
    IDSet red;
    std::unordered_map<GraphNode *, GraphNode *> originalToAP;
    std::unordered_map<GraphNode *, GraphNode *> apToOriginal;

    GraphNode *createNode(GraphNode *original) {
      GraphNode *node = &graph.createNode();
      originalToAP[original] = node;
      apToOriginal[node] = original;
      return node;
    }
    GraphNode *getNode(GraphNode *original) const {
      auto it = originalToAP.find(original);
      return it == originalToAP.end() ? nullptr : it->second;
    }
    GraphNode *getOriginal(GraphNode *node) const {
      auto it = apToOriginal.find(node);
      return it == apToOriginal.end() ? nullptr : it->second;
    }
    bool isBlue(GraphNode *node) const { return contains(blue, node); }
    bool isRed(GraphNode *node) const { return contains(red, node); }
  };

  template <typename Callback>
  static void forEachFirstReachable(const IDSet &selected, GraphNode *from,
                                    Callback callback) {
    SetQueue<GraphNode *> queue;
    for (GraphNode *succ : from->successors())
      queue.push(succ);
    while (!queue.empty()) {
      GraphNode *current = queue.popBack();
      if (contains(selected, current)) {
        callback(current);
      } else {
        for (GraphNode *succ : current->successors())
          queue.push(succ);
      }
    }
  }

  static ColoredAP createAP(const IDSet &selected, Graph &graph,
                            GraphNode *predicate) {
    ColoredAP result;
    for (GraphNode *node : graph.nodes())
      if (contains(selected, node))
        result.createNode(node);

    GraphNode *apPredicate = result.getNode(predicate);
    if (!apPredicate || result.graph.size() < 3)
      return {};

    for (GraphNode *apNode : result.graph.nodes()) {
      GraphNode *original = result.getOriginal(apNode);
      forEachFirstReachable(selected, original, [&](GraphNode *reachable) {
        GraphNode *apReachable = result.getNode(reachable);
        assert(apReachable);
        result.graph.addEdge(*apNode, *apReachable);
      });
    }
    if (apPredicate->successors().size() < 2)
      return {};
    return result;
  }

  static ColoredAP createColoredAP(const AllMaxPathResult &allPaths,
                                   Graph &graph, GraphNode *predicate) {
    auto pathIt = allPaths.find(predicate);
    if (pathIt == allPaths.end())
      return {};
    const IDSet &selected = pathIt->second;
    ColoredAP result = createAP(selected, graph, predicate);
    if (result.graph.empty() || predicate->successors().size() != 2)
      return {};

    auto colorFromSuccessor = [&](GraphNode *successor, IDSet &color,
                                  bool &overlap) {
      auto addColor = [&](GraphNode *original) {
        GraphNode *apNode = result.getNode(original);
        assert(apNode);
        if ((&color == &result.red && result.isBlue(apNode)) ||
            (&color == &result.blue && result.isRed(apNode)))
          overlap = true;
        color.set(apNode->getID());
      };
      if (contains(selected, successor))
        addColor(successor);
      else
        forEachFirstReachable(selected, successor, addColor);
    };

    bool overlap = false;
    colorFromSuccessor(predicate->successors()[0], result.blue, overlap);
    colorFromSuccessor(predicate->successors()[1], result.red, overlap);
    if (overlap || result.blue.empty() || result.red.empty())
      return {};
    return result;
  }

  static bool isSimpleAP(const ColoredAP &colored) {
    GraphNode *entry = nullptr;
    for (GraphNode *node : colored.graph.nodes()) {
      if (node->successors().size() > 1) {
        if (entry)
          return false;
        entry = node;
      } else if (node->successors().size() != 1 ||
                 node->successors().front() == node) {
        return false;
      }
    }
    if (!entry || entry->successors().empty())
      return false;

    std::set<GraphNode *> visited;
    GraphNode *start = entry->successors().front();
    GraphNode *current = start;
    do {
      if (!current || current == entry || !visited.insert(current).second ||
          current->successors().size() != 1)
        return false;
      current = current->successors().front();
    } while (current != start);
    return visited.size() == colored.graph.size() - 1;
  }

  template <typename FirstPredicate, typename SecondPredicate>
  static std::pair<GraphNode *, GraphNode *>
  findOnCycle(GraphNode *start, GraphNode *end, FirstPredicate firstPredicate,
              SecondPredicate secondPredicate) {
    GraphNode *first = nullptr;
    GraphNode *second = nullptr;
    GraphNode *node = start;
    do {
      if (firstPredicate(node))
        first = node;
      if (secondPredicate(node)) {
        second = node;
        break;
      }
      if (node->successors().size() != 1)
        return {nullptr, nullptr};
      node = node->successors().front();
    } while (node != end);
    return {first, second};
  }

  static void materializeDOD(ColoredAP &colored, GraphNode *predicate,
                             DependenceMap &dependencies,
                             DependenceMap &dependents) {
    if (!isSimpleAP(colored))
      return;

    GraphNode *blue1 = colored.graph.getNode(*colored.blue.begin());
    auto isBlue = [&](GraphNode *node) { return colored.isBlue(node); };
    auto isRed = [&](GraphNode *node) { return colored.isRed(node); };

    GraphNode *blue2 = nullptr;
    GraphNode *red1 = nullptr;
    std::tie(blue2, red1) = findOnCycle(blue1, blue1, isBlue, isRed);
    if (!blue2 || !red1)
      return;

    GraphNode *red2 = nullptr;
    GraphNode *blue3 = nullptr;
    std::tie(red2, blue3) = findOnCycle(red1, blue1, isRed, isBlue);
    if (!red2)
      return;
    if (blue3) {
      if (findOnCycle(blue3, blue1, isRed, isRed).first)
        return;
    } else {
      blue3 = blue1;
    }

    auto addRange = [&](GraphNode *begin, GraphNode *endColor) {
      GraphNode *current = begin;
      do {
        GraphNode *original = colored.getOriginal(current);
        assert(original);
        dependencies[original].insert(predicate);
        dependents[predicate].insert(original);
        current = current->successors().front();
      } while (current != endColor);
    };
    addRange(blue2, red1);
    addRange(red2, blue3);
  }

  static void computeDOD(GraphNode *predicate, Graph &graph,
                         const AllMaxPathResult &allPaths,
                         DependenceMap &dependencies,
                         DependenceMap &dependents) {
    ColoredAP colored = createColoredAP(allPaths, graph, predicate);
    if (!colored.graph.empty())
      materializeDOD(colored, predicate, dependencies, dependents);
  }

  static void computeNTSCD(GraphNode *predicate, Graph &graph,
                           const AllMaxPathResult &allPaths,
                           DependenceMap &dependencies,
                           DependenceMap &dependents) {
    GraphNode *first = predicate->successors()[0];
    GraphNode *second = predicate->successors()[1];
    auto firstIt = allPaths.find(first);
    auto secondIt = allPaths.find(second);
    if (firstIt == allPaths.end() || secondIt == allPaths.end())
      return;
    for (GraphNode *node : graph.nodes()) {
      if (contains(firstIt->second, node) != contains(secondIt->second, node)) {
        dependencies[node].insert(predicate);
        dependents[predicate].insert(node);
      }
    }
  }
};

} // namespace
DependenceResult computeDOD(Graph &graph) {
  return DODComputer().compute(graph, false);
}

DependenceResult computeDODNTSCD(Graph &graph) {
  return DODComputer().compute(graph, true);
}

DependenceResult computeDODRanganath(Graph &graph) {
  enum class Color { White, Black, Uncolored };
  std::unordered_map<GraphNode *, Color> colors;

  auto onAllPaths = [&](GraphNode *from, GraphNode *target) {
    if (from == target)
      return true;

    struct NodeInfo {
      bool onStack{false};
      bool visited{false};
    };
    std::unordered_map<GraphNode *, NodeInfo> info;
    std::function<bool(GraphNode *)> visit = [&](GraphNode *node) {
      if (node == target)
        return true;
      info[node].visited = true;
      if (node->successors().empty())
        return false;

      for (GraphNode *successor : node->successors()) {
        if (info[successor].onStack)
          return false;
        if (!info[successor].visited) {
          info[successor].onStack = true;
          if (!visit(successor))
            return false;
          info[successor].onStack = false;
        }
      }
      return true;
    };
    info[from].onStack = true;
    return visit(from);
  };

  auto hasDependence = [&](GraphNode *predicate, GraphNode *white,
                           GraphNode *black) {
    for (GraphNode *node : graph.nodes())
      colors[node] = Color::Uncolored;
    colors[white] = Color::White;
    colors[black] = Color::Black;

    std::set<GraphNode *> visited{white, black};
    std::function<void(GraphNode *)> colorDAG = [&](GraphNode *node) {
      if (!visited.insert(node).second || node->successors().empty())
        return;
      for (GraphNode *successor : node->successors())
        colorDAG(successor);
      Color color = colors[node->successors().front()];
      for (GraphNode *successor : node->successors()) {
        if (colors[successor] != color) {
          color = Color::Uncolored;
          break;
        }
      }
      colors[node] = color;
    };
    colorDAG(predicate);

    bool whiteChild = false;
    bool blackChild = false;
    for (GraphNode *succ : predicate->successors()) {
      whiteChild |= colors[succ] == Color::White;
      blackChild |= colors[succ] == Color::Black;
    }
    return whiteChild && blackChild;
  };

  DependenceMap dependencies;
  DependenceMap dependents;
  for (GraphNode *predicate : graph.predicates()) {
    for (GraphNode *first : graph.nodes()) {
      for (GraphNode *second : graph.nodes()) {
        if (first == second)
          continue;
        if (onAllPaths(first, second) && onAllPaths(second, first) &&
            hasDependence(predicate, second, first)) {
          dependencies[first].insert(predicate);
          dependencies[second].insert(predicate);
          dependents[predicate].insert(first);
          // dg inserts the predicate itself here (`revCD[n].insert(n)`),
          // breaking the forward/reverse invariant. Record `second`, which is
          // the other dependent endpoint of the discovered DOD pair.
          dependents[predicate].insert(second);
        }
      }
    }
  }
  return {std::move(dependencies), std::move(dependents)};
}

} // namespace lotus::cd::detail
