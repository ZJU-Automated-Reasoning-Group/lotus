#include "Concurrency/ValueFlow/ThreadAwareSVFG.h"

#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "Concurrency/MHP/IMHPAnalysis.h"

#include <algorithm>
#include <cassert>
#include <vector>

#include <llvm/Support/Casting.h>

using namespace llvm;

namespace lotus::analysis {

ThreadAwareSVFGBuilder::ThreadAwareSVFGBuilder(
    SVFG &graph, const mhp::IMHPAnalysis &mhp,
    const mhp::LockSetAnalysis *locks, const FilteredSVFGView *scope)
    : graph_(&graph), mhp_(&mhp), locks_(locks), scope_(scope) {
  assert((!scope_ || &scope_->source() == graph_) &&
         "filtered SVFG scope must view the overlay's base graph");
}

ThreadAwareSVFGBuilder::~ThreadAwareSVFGBuilder() { clear(); }

bool ThreadAwareSVFGBuilder::inScope(const SVFGNode *node) const {
  return node && (!scope_ || scope_->contains(node));
}

SVFGNodeBS ThreadAwareSVFGBuilder::intersectTargets(const SVFGNode &lhs,
                                                    const SVFGNode &rhs) const {
  const SVFGNodeBS *lhsPts = lhs.getPointsTo();
  const SVFGNodeBS *rhsPts = rhs.getPointsTo();
  if (!lhsPts || !rhsPts || lhsPts->empty() || rhsPts->empty())
    return {};

  const bool lhsUnknown =
      std::any_of(lhsPts->begin(), lhsPts->end(),
                  [&](uint32_t id) { return graph_->isUnknownObject(id); });
  const bool rhsUnknown =
      std::any_of(rhsPts->begin(), rhsPts->end(),
                  [&](uint32_t id) { return graph_->isUnknownObject(id); });
  if (lhsUnknown || rhsUnknown) {
    SVFGNodeBS guard;
    for (uint32_t id : *lhsPts)
      if (graph_->isUnknownObject(id))
        guard.insert(id);
    for (uint32_t id : *rhsPts)
      if (graph_->isUnknownObject(id))
        guard.insert(id);
    return guard;
  }

  const SVFGNodeBS *small = lhsPts;
  const SVFGNodeBS *large = rhsPts;
  if (large->size() < small->size())
    std::swap(small, large);

  SVFGNodeBS result;
  for (uint32_t id : *small)
    if (large->count(id) != 0)
      result.insert(id);
  return result;
}

bool ThreadAwareSVFGBuilder::addInterferenceEdge(SVFGNode &source,
                                                 SVFGNode &destination,
                                                 const SVFGNodeBS &guard) {
  for (SVFGEdge *edge : source.getOutEdges()) {
    if (edge && edge->getDstNode() == &destination &&
        edge->getEdgeKind() == SVFGEdgeK::ThreadMHPIndirectVF) {
      edge->addPointsTo(guard);
      return false;
    }
  }

  SVFGEdge *edge = graph_->addEdge(
      &source, &destination, SVFGEdgeK::ThreadMHPIndirectVF, nullptr, guard);
  if (!edge)
    return false;
  overlayEdges_.insert(edge);
  ++stats_.edgesAdded;
  return true;
}

const ThreadAwareSVFGBuilder::Statistics &ThreadAwareSVFGBuilder::build() {
  clear();
  stats_ = {};

  std::vector<StoreSVFGNode *> stores;
  std::vector<LoadSVFGNode *> loads;
  for (auto &entry : *graph_) {
    SVFGNode *node = entry.second;
    if (!inScope(node))
      continue;
    if (auto *store = dyn_cast<StoreSVFGNode>(node))
      stores.push_back(store);
    else if (auto *load = dyn_cast<LoadSVFGNode>(node))
      loads.push_back(load);
  }
  stats_.stores = stores.size();
  stats_.loads = loads.size();

  auto process = [&](StoreSVFGNode &source, SVFGNode &destination,
                     mhp::MemoryAccessKind destinationKind) {
    const Instruction *sourceInst = source.getInstruction();
    const Instruction *destinationInst = destination.getInstruction();
    if (!sourceInst || !destinationInst || sourceInst == destinationInst)
      return;

    const SVFGNodeBS guard = intersectTargets(source, destination);
    if (guard.empty())
      return;
    ++stats_.aliasCandidates;

    if (!mhp_->mayHappenInParallel(sourceInst, destinationInst))
      return;
    ++stats_.parallelCandidates;

    // A common lock prevents a race, but it does not prevent a value written
    // in one critical section from becoming visible to another thread later.
    // Keep the conservative flow edge until lock-span head/tail reasoning is
    // available, while recording the candidate for diagnostics.
    if (locks_ &&
        locks_->mustMutuallyExclude(sourceInst, mhp::MemoryAccessKind::Write,
                                    destinationInst, destinationKind)) {
      ++stats_.mutuallyExcludedCandidates;
    }
    addInterferenceEdge(source, destination, guard);
  };

  for (StoreSVFGNode *store : stores)
    for (LoadSVFGNode *load : loads)
      process(*store, *load, mhp::MemoryAccessKind::Read);

  for (std::size_t i = 0; i < stores.size(); ++i) {
    for (std::size_t j = i + 1; j < stores.size(); ++j) {
      process(*stores[i], *stores[j], mhp::MemoryAccessKind::Write);
      process(*stores[j], *stores[i], mhp::MemoryAccessKind::Write);
    }
  }

  return stats_;
}

void ThreadAwareSVFGBuilder::clear() {
  if (!graph_)
    return;
  for (SVFGEdge *edge : overlayEdges_)
    graph_->removeEdge(edge);
  overlayEdges_.clear();
}

} // namespace lotus::analysis
