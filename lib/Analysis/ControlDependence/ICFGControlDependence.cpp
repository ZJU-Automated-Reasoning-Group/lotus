//===- ICFGControlDependence.cpp - Whole-ICFG control dependence ----------===//

#include "Analysis/ControlDependence/ICFGControlDependence.h"

#include "Analysis/ControlDependence/CompactControlDependence.h"
#include "Analysis/ControlDependence/ControlClosure.h"
#include "Analysis/ControlDependence/ControlDependenceGraph.h"
#include "Analysis/ControlDependence/DOD.h"
#include "Analysis/ControlDependence/NTSCD.h"
#include "IR/ICFG/ICFG.h"

#include <algorithm>
#include <cassert>
#include <utility>
#include <vector>

namespace lotus::cd {

using detail::DependenceResult;
using detail::Graph;
using detail::GraphNode;
using detail::NodeSet;

class ICFGControlDependenceAnalysis::Impl {
public:
  Impl(const ICFG &icfg, ControlDependenceOptions options)
      : m_options(options) {
    assert(options.algorithm != Algorithm::Standard &&
           "standard CD cannot run on an ICFG");
    collectNodes(icfg);
    buildGraph();
    compute();
  }

  llvm::ArrayRef<const ICFGNode *> getDependencies(const ICFGNode *node) const {
    auto it = m_dependencies.find(node);
    return it == m_dependencies.end()
               ? llvm::ArrayRef<const ICFGNode *>()
               : llvm::ArrayRef<const ICFGNode *>(it->second);
  }

  llvm::ArrayRef<const ICFGNode *> getDependents(const ICFGNode *node) const {
    auto it = m_dependents.find(node);
    return it == m_dependents.end()
               ? llvm::ArrayRef<const ICFGNode *>()
               : llvm::ArrayRef<const ICFGNode *>(it->second);
  }

  NodeVector getClosure(llvm::ArrayRef<const ICFGNode *> nodes) const {
    assert(m_options.algorithm == Algorithm::StrongControlClosure &&
           "getClosure requires StrongControlClosure");
    if (m_options.algorithm != Algorithm::StrongControlClosure)
      return {};
    NodeSet initial;
    for (const ICFGNode *node : nodes) {
      auto it = m_icfgToGraph.find(node);
      if (it != m_icfgToGraph.end())
        initial.insert(it->second);
    }
    NodeSet closure = detail::computeStrongControlClosure(
        const_cast<Graph &>(m_graph), initial);
    NodeVector result;
    for (const ICFGNode *node : m_nodes)
      if (closure.count(m_icfgToGraph.lookup(node)))
        result.push_back(node);
    return result;
  }

  bool hasDODBiclique(const ICFGNode *decision) const {
    auto nodeIt = m_icfgToGraph.find(decision);
    return nodeIt != m_icfgToGraph.end() &&
           m_bicliques.count(nodeIt->second) != 0;
  }

  NodeVector getDODSide(const ICFGNode *decision, bool left) const {
    NodeVector result;
    auto nodeIt = m_icfgToGraph.find(decision);
    if (nodeIt == m_icfgToGraph.end())
      return result;
    auto bicliqueIt = m_bicliques.find(nodeIt->second);
    if (bicliqueIt == m_bicliques.end())
      return result;
    const auto &side =
        left ? bicliqueIt->second.left : bicliqueIt->second.right;
    result.reserve(side.count());
    for (unsigned id : side)
      result.push_back(m_nodes[id - 1]);
    return result;
  }

  bool isDOD(const ICFGNode *decision, const ICFGNode *first,
             const ICFGNode *second) const {
    auto decisionIt = m_icfgToGraph.find(decision);
    auto firstIt = m_icfgToGraph.find(first);
    auto secondIt = m_icfgToGraph.find(second);
    if (decisionIt == m_icfgToGraph.end() || firstIt == m_icfgToGraph.end() ||
        secondIt == m_icfgToGraph.end())
      return false;
    auto bicliqueIt = m_bicliques.find(decisionIt->second);
    return bicliqueIt != m_bicliques.end() &&
           bicliqueIt->second.contains(firstIt->second, secondIt->second);
  }

  NodeVector getDependencyClosure(llvm::ArrayRef<const ICFGNode *> seed) const {
    if (m_options.algorithm != Algorithm::DODNTSCDCompact)
      return {};
    NodeSet initial;
    for (const ICFGNode *node : seed) {
      auto it = m_icfgToGraph.find(node);
      if (it != m_icfgToGraph.end())
        initial.insert(it->second);
    }
    NodeSet closure = detail::computeCompactDependencyClosure(
        const_cast<Graph &>(m_graph), initial, m_compactNTSCD, m_bicliques);
    NodeVector result;
    for (const ICFGNode *node : m_nodes)
      if (closure.count(m_icfgToGraph.lookup(node)))
        result.push_back(node);
    return result;
  }

  ControlDependenceOptions m_options;

private:
  void collectNodes(const ICFG &icfg) {
    m_nodes.reserve(icfg.getTotalNodeNum());
    for (const auto &entry : icfg)
      m_nodes.push_back(entry.second);
    std::sort(m_nodes.begin(), m_nodes.end(),
              [](const ICFGNode *left, const ICFGNode *right) {
                return left->getId() < right->getId();
              });
  }

  void buildGraph() {
    for (const ICFGNode *node : m_nodes)
      m_icfgToGraph[node] = &m_graph.createNode();
    for (const ICFGNode *node : m_nodes) {
      GraphNode *source = m_icfgToGraph.lookup(node);
      for (const ICFGEdge *edge : node->getOutEdges()) {
        // A fully resolved call-to-return edge is an intraprocedural summary,
        // not a realizable whole-program path. Keep summary edges only for an
        // unresolved/external remainder that has no complete call/return path.
        if (const auto *summary = llvm::dyn_cast<CallToRetCFGEdge>(edge))
          if (!summary->hasUnresolvedCallee())
            continue;
        auto targetIt = m_icfgToGraph.find(edge->getDstNode());
        if (targetIt != m_icfgToGraph.end())
          m_graph.addEdge(*source, *targetIt->second);
      }
    }
  }

  void compute() {
    switch (m_options.algorithm) {
    case Algorithm::Standard:
      return;
    case Algorithm::NTSCD:
      materialize(detail::computeNTSCD(m_graph));
      return;
    case Algorithm::NTSCD2:
    case Algorithm::NTSCDLegacy:
      materialize(detail::computeNTSCD2(m_graph));
      return;
    case Algorithm::NTSCDRanganath:
      materialize(detail::computeNTSCDRanganath(m_graph, true));
      return;
    case Algorithm::NTSCDRanganathOriginal:
      materialize(detail::computeNTSCDRanganath(m_graph, false));
      return;
    case Algorithm::DOD:
      materialize(detail::computeDOD(m_graph));
      return;
    case Algorithm::DODRanganath:
      materialize(detail::computeDODRanganath(m_graph));
      return;
    case Algorithm::DODNTSCD:
      materialize(detail::computeDODNTSCD(m_graph));
      return;
    case Algorithm::StrongControlClosure:
      return;
    case Algorithm::NTSCDCompact: {
      detail::Inevitability inevitability =
          detail::computeInevitability(m_graph);
      m_compactNTSCD = detail::computeCompactNTSCD(m_graph, inevitability);
      materialize(m_compactNTSCD);
      return;
    }
    case Algorithm::DODCompact: {
      detail::Inevitability inevitability =
          detail::computeInevitability(m_graph);
      m_bicliques = detail::computeCompactDOD(m_graph, inevitability);
      materialize(
          detail::materializeCompactDODDependencies(m_graph, m_bicliques));
      return;
    }
    case Algorithm::DODNTSCDCompact: {
      detail::Inevitability inevitability =
          detail::computeInevitability(m_graph);
      m_compactNTSCD = detail::computeCompactNTSCD(m_graph, inevitability);
      m_bicliques = detail::computeCompactDOD(m_graph, inevitability);
      DependenceResult combined = m_compactNTSCD;
      mergeInto(combined, detail::materializeCompactDODDependencies(
                              m_graph, m_bicliques));
      materialize(std::move(combined));
      return;
    }
    }
  }

  static void mergeInto(DependenceResult &destination,
                        const DependenceResult &source) {
    for (const auto &entry : source.first)
      destination.first[entry.first].insert(entry.second.begin(),
                                            entry.second.end());
    for (const auto &entry : source.second)
      destination.second[entry.first].insert(entry.second.begin(),
                                             entry.second.end());
  }

  void materialize(DependenceResult result) {
    auto materializeMap = [&](const detail::DependenceMap &source,
                              auto &destination) {
      for (const auto &entry : source) {
        const ICFGNode *node = icfgNodeFor(entry.first);
        NodeVector &relatedNodes = destination[node];
        relatedNodes.reserve(entry.second.size());
        for (GraphNode *related : entry.second)
          relatedNodes.push_back(icfgNodeFor(related));
        std::sort(relatedNodes.begin(), relatedNodes.end(),
                  [](const ICFGNode *left, const ICFGNode *right) {
                    return left->getId() < right->getId();
                  });
      }
    };
    materializeMap(result.first, m_dependencies);
    materializeMap(result.second, m_dependents);
  }

  const ICFGNode *icfgNodeFor(const GraphNode *node) const {
    assert(node && node->getID() > 0 && node->getID() <= m_nodes.size());
    return m_nodes[node->getID() - 1];
  }

  Graph m_graph;
  std::vector<const ICFGNode *> m_nodes;
  llvm::DenseMap<const ICFGNode *, GraphNode *> m_icfgToGraph;
  llvm::DenseMap<const ICFGNode *, NodeVector> m_dependencies;
  llvm::DenseMap<const ICFGNode *, NodeVector> m_dependents;
  DependenceResult m_compactNTSCD;
  detail::DODBicliqueMap m_bicliques;
};

ICFGControlDependenceAnalysis::ICFGControlDependenceAnalysis(
    const ICFG &icfg, ControlDependenceOptions options)
    : m_impl(std::make_unique<Impl>(icfg, options)) {}

ICFGControlDependenceAnalysis::~ICFGControlDependenceAnalysis() = default;

ICFGControlDependenceAnalysis::ICFGControlDependenceAnalysis(
    ICFGControlDependenceAnalysis &&) noexcept = default;

ICFGControlDependenceAnalysis &ICFGControlDependenceAnalysis::operator=(
    ICFGControlDependenceAnalysis &&) noexcept = default;

Algorithm ICFGControlDependenceAnalysis::getAlgorithm() const {
  return m_impl->m_options.algorithm;
}

llvm::ArrayRef<const ICFGNode *>
ICFGControlDependenceAnalysis::getDependencies(const ICFGNode *node) const {
  return m_impl->getDependencies(node);
}

llvm::ArrayRef<const ICFGNode *>
ICFGControlDependenceAnalysis::getDependents(const ICFGNode *node) const {
  return m_impl->getDependents(node);
}

bool ICFGControlDependenceAnalysis::dependsOn(const ICFGNode *node,
                                              const ICFGNode *predicate) const {
  llvm::ArrayRef<const ICFGNode *> dependencies = getDependencies(node);
  return std::find(dependencies.begin(), dependencies.end(), predicate) !=
         dependencies.end();
}

ICFGControlDependenceAnalysis::NodeVector
ICFGControlDependenceAnalysis::getClosure(
    llvm::ArrayRef<const ICFGNode *> nodes) const {
  return m_impl->getClosure(nodes);
}

bool ICFGControlDependenceAnalysis::hasDODBiclique(
    const ICFGNode *decision) const {
  return m_impl->hasDODBiclique(decision);
}

ICFGControlDependenceAnalysis::NodeVector
ICFGControlDependenceAnalysis::getDODLeft(const ICFGNode *decision) const {
  return m_impl->getDODSide(decision, true);
}

ICFGControlDependenceAnalysis::NodeVector
ICFGControlDependenceAnalysis::getDODRight(const ICFGNode *decision) const {
  return m_impl->getDODSide(decision, false);
}

bool ICFGControlDependenceAnalysis::isDOD(const ICFGNode *decision,
                                          const ICFGNode *first,
                                          const ICFGNode *second) const {
  return m_impl->isDOD(decision, first, second);
}

ICFGControlDependenceAnalysis::NodeVector
ICFGControlDependenceAnalysis::getDependencyClosure(
    llvm::ArrayRef<const ICFGNode *> seed) const {
  return m_impl->getDependencyClosure(seed);
}

} // namespace lotus::cd
