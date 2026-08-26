//===- ControlDependence.cpp - LLVM control-dependence adapter ------------===//

#include "Analysis/ControlDependence/ControlDependence.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"

#include "Analysis/ControlDependence/CompactControlDependence.h"
#include "Analysis/ControlDependence/ControlClosure.h"
#include "Analysis/ControlDependence/ControlDependenceGraph.h"
#include "Analysis/ControlDependence/DOD.h"
#include "Analysis/ControlDependence/NTSCD.h"
#include "Analysis/ControlDependence/SCD.h"

#include <algorithm>
#include <cassert>
#include <utility>
#include <vector>

namespace lotus::cd {

using detail::DependenceResult;
using detail::Graph;
using detail::GraphNode;
using detail::NodeSet;

class ControlDependenceAnalysis::Impl {
public:
  Impl(llvm::Function &function, ControlDependenceOptions options)
      : m_function(function), m_options(options),
        m_graph(function.getName().str()) {
    buildGraph();
    if (!function.isDeclaration())
      compute();
  }

  llvm::ArrayRef<const llvm::BasicBlock *>
  getDependencies(const llvm::BasicBlock *block) const {
    auto it = m_dependencies.find(block);
    return it == m_dependencies.end()
               ? llvm::ArrayRef<const llvm::BasicBlock *>()
               : llvm::ArrayRef<const llvm::BasicBlock *>(it->second);
  }

  llvm::ArrayRef<const llvm::BasicBlock *>
  getDependents(const llvm::BasicBlock *block) const {
    auto it = m_dependents.find(block);
    return it == m_dependents.end()
               ? llvm::ArrayRef<const llvm::BasicBlock *>()
               : llvm::ArrayRef<const llvm::BasicBlock *>(it->second);
  }

  BlockVector
  getClosure(llvm::ArrayRef<const llvm::BasicBlock *> blocks) const {
    assert(m_options.algorithm == Algorithm::StrongControlClosure &&
           "getClosure requires StrongControlClosure");
    if (m_options.algorithm != Algorithm::StrongControlClosure)
      return {};

    NodeSet initial;
    for (const llvm::BasicBlock *block : blocks) {
      auto it = m_blockToNode.find(block);
      if (it != m_blockToNode.end())
        initial.insert(it->second);
    }
    NodeSet closure = detail::computeStrongControlClosure(
        const_cast<Graph &>(m_graph), initial);

    BlockVector result;
    for (const llvm::BasicBlock &block : m_function) {
      auto it = m_blockToNode.find(&block);
      if (it != m_blockToNode.end() && closure.count(it->second))
        result.push_back(&block);
    }
    return result;
  }

  bool hasDODBiclique(const llvm::BasicBlock *decision) const {
    auto nodeIt = m_blockToNode.find(decision);
    return nodeIt != m_blockToNode.end() &&
           m_bicliques.count(nodeIt->second) != 0;
  }

  BlockVector getDODSide(const llvm::BasicBlock *decision, bool left) const {
    BlockVector result;
    auto nodeIt = m_blockToNode.find(decision);
    if (nodeIt == m_blockToNode.end())
      return result;
    auto bicliqueIt = m_bicliques.find(nodeIt->second);
    if (bicliqueIt == m_bicliques.end())
      return result;
    const auto &side =
        left ? bicliqueIt->second.left : bicliqueIt->second.right;
    result.reserve(side.count());
    for (unsigned id : side)
      result.push_back(m_nodeToBlock[id - 1]);
    return result;
  }

  bool isDOD(const llvm::BasicBlock *decision, const llvm::BasicBlock *first,
             const llvm::BasicBlock *second) const {
    auto decisionIt = m_blockToNode.find(decision);
    auto firstIt = m_blockToNode.find(first);
    auto secondIt = m_blockToNode.find(second);
    if (decisionIt == m_blockToNode.end() || firstIt == m_blockToNode.end() ||
        secondIt == m_blockToNode.end())
      return false;
    auto bicliqueIt = m_bicliques.find(decisionIt->second);
    return bicliqueIt != m_bicliques.end() &&
           bicliqueIt->second.contains(firstIt->second, secondIt->second);
  }

  BlockVector
  getDependencyClosure(llvm::ArrayRef<const llvm::BasicBlock *> seed) const {
    if (m_options.algorithm != Algorithm::DODNTSCDCompact)
      return {};
    NodeSet initial;
    for (const llvm::BasicBlock *block : seed) {
      auto it = m_blockToNode.find(block);
      if (it != m_blockToNode.end())
        initial.insert(it->second);
    }
    NodeSet closure = detail::computeCompactDependencyClosure(
        const_cast<Graph &>(m_graph), initial, m_compactNTSCD, m_bicliques);
    BlockVector result;
    for (const llvm::BasicBlock &block : m_function)
      if (closure.count(m_blockToNode.lookup(&block)))
        result.push_back(&block);
    return result;
  }

  llvm::Function &m_function;
  ControlDependenceOptions m_options;

private:
  void buildGraph() {
    for (llvm::BasicBlock &block : m_function) {
      GraphNode *node = &m_graph.createNode();
      m_blockToNode[&block] = node;
      m_nodeToBlock.push_back(&block);
    }
    for (llvm::BasicBlock &block : m_function) {
      GraphNode *source = m_blockToNode.lookup(&block);
      for (llvm::BasicBlock *successor : llvm::successors(&block))
        m_graph.addEdge(*source, *m_blockToNode.lookup(successor));
    }
  }

  void compute() {
    switch (m_options.algorithm) {
    case Algorithm::Standard:
      materialize(detail::computeSCD(m_function, m_blockToNode));
      return;
    case Algorithm::NTSCD:
      materialize(detail::computeNTSCD(m_graph));
      return;
    case Algorithm::NTSCD2:
      materialize(detail::computeNTSCD2(m_graph));
      return;
    case Algorithm::NTSCDLegacy:
      // dg's legacy intraprocedural implementation is the same backwards
      // counter algorithm as NTSCD2; only its old ICFG adapter differed.
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
        const llvm::BasicBlock *block = blockFor(entry.first);
        BlockVector &blocks = destination[block];
        blocks.reserve(entry.second.size());
        for (GraphNode *related : entry.second)
          blocks.push_back(blockFor(related));
        std::sort(
            blocks.begin(), blocks.end(),
            [&](const llvm::BasicBlock *left, const llvm::BasicBlock *right) {
              return m_blockToNode.lookup(left)->getID() <
                     m_blockToNode.lookup(right)->getID();
            });
      }
    };
    materializeMap(result.first, m_dependencies);
    materializeMap(result.second, m_dependents);
  }

  const llvm::BasicBlock *blockFor(const GraphNode *node) const {
    assert(node && node->getID() > 0 && node->getID() <= m_nodeToBlock.size());
    return m_nodeToBlock[node->getID() - 1];
  }

  Graph m_graph;
  llvm::DenseMap<const llvm::BasicBlock *, GraphNode *> m_blockToNode;
  std::vector<const llvm::BasicBlock *> m_nodeToBlock;
  llvm::DenseMap<const llvm::BasicBlock *, BlockVector> m_dependencies;
  llvm::DenseMap<const llvm::BasicBlock *, BlockVector> m_dependents;
  DependenceResult m_compactNTSCD;
  detail::DODBicliqueMap m_bicliques;
};

ControlDependenceAnalysis::ControlDependenceAnalysis(
    llvm::Function &function, ControlDependenceOptions options)
    : m_impl(std::make_unique<Impl>(function, options)) {}

ControlDependenceAnalysis::~ControlDependenceAnalysis() = default;

ControlDependenceAnalysis::ControlDependenceAnalysis(
    ControlDependenceAnalysis &&) noexcept = default;

ControlDependenceAnalysis &ControlDependenceAnalysis::operator=(
    ControlDependenceAnalysis &&) noexcept = default;

llvm::Function &ControlDependenceAnalysis::getFunction() const {
  return m_impl->m_function;
}

Algorithm ControlDependenceAnalysis::getAlgorithm() const {
  return m_impl->m_options.algorithm;
}

llvm::ArrayRef<const llvm::BasicBlock *>
ControlDependenceAnalysis::getDependencies(
    const llvm::BasicBlock *block) const {
  return m_impl->getDependencies(block);
}

llvm::ArrayRef<const llvm::BasicBlock *>
ControlDependenceAnalysis::getDependents(const llvm::BasicBlock *block) const {
  return m_impl->getDependents(block);
}

bool ControlDependenceAnalysis::dependsOn(
    const llvm::BasicBlock *block, const llvm::BasicBlock *predicate) const {
  llvm::ArrayRef<const llvm::BasicBlock *> dependencies =
      getDependencies(block);
  return std::find(dependencies.begin(), dependencies.end(), predicate) !=
         dependencies.end();
}

ControlDependenceAnalysis::BlockVector ControlDependenceAnalysis::getClosure(
    llvm::ArrayRef<const llvm::BasicBlock *> blocks) const {
  return m_impl->getClosure(blocks);
}

bool ControlDependenceAnalysis::hasDODBiclique(
    const llvm::BasicBlock *decision) const {
  return m_impl->hasDODBiclique(decision);
}

ControlDependenceAnalysis::BlockVector
ControlDependenceAnalysis::getDODLeft(const llvm::BasicBlock *decision) const {
  return m_impl->getDODSide(decision, true);
}

ControlDependenceAnalysis::BlockVector
ControlDependenceAnalysis::getDODRight(const llvm::BasicBlock *decision) const {
  return m_impl->getDODSide(decision, false);
}

bool ControlDependenceAnalysis::isDOD(const llvm::BasicBlock *decision,
                                      const llvm::BasicBlock *first,
                                      const llvm::BasicBlock *second) const {
  return m_impl->isDOD(decision, first, second);
}

ControlDependenceAnalysis::BlockVector
ControlDependenceAnalysis::getDependencyClosure(
    llvm::ArrayRef<const llvm::BasicBlock *> seed) const {
  return m_impl->getDependencyClosure(seed);
}

} // namespace lotus::cd
