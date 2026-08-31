#include "Concurrency/ValueFlow/ThreadAwareSVFG.h"

#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "Concurrency/MHP/IMHPAnalysis.h"
#include "Concurrency/Thread/ThreadCreationTree.h"
#include "Concurrency/Utils/ThreadAPI.h"

#include <algorithm>
#include <cassert>
#include <vector>

#include <llvm/IR/Dominators.h>
#include <llvm/Support/Casting.h>

using namespace llvm;

namespace lotus::analysis {

ThreadAwareSVFGBuilder::ThreadAwareSVFGBuilder(
    SVFG &graph, const mhp::IMHPAnalysis &mhp,
    const mhp::LockSetAnalysis *locks, const FilteredSVFGView *scope,
    const ThreadCreationTree *threadTree)
    : graph_(&graph), mhp_(&mhp), locks_(locks), scope_(scope),
      threadTree_(threadTree) {
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

bool ThreadAwareSVFGBuilder::addOwnedEdge(SVFGNode &source,
                                          SVFGNode &destination, SVFGEdgeK kind,
                                          const CallBase *callSite,
                                          const SVFGNodeBS &guard) {
  for (SVFGEdge *edge : source.getOutEdges())
    if (edge && edge->getDstNode() == &destination &&
        edge->getEdgeKind() == kind && edge->getCallSite() == callSite) {
      edge->addPointsTo(guard);
      return false;
    }
  SVFGEdge *edge =
      graph_->addEdge(&source, &destination, kind, callSite, guard);
  if (!edge)
    return false;
  overlayEdges_.insert(edge);
  return true;
}

void ThreadAwareSVFGBuilder::connectForkFlow() {
  if (!threadTree_)
    return;
  ThreadAPI *threadAPI = ThreadAPI::getThreadAPI();
  for (const ThreadCreationTree::ForkRelation &relation :
       threadTree_->forkRelations()) {
    const CallBase *fork = relation.site;
    const Function *target = relation.target;
    if (!fork || !target)
      continue;

    const Value *payload = threadAPI->getActualParmAtForkSite(fork);
    SVFGNode *actual = payload ? graph_->getValueNode(payload) : nullptr;
    if (actual && inScope(actual)) {
      for (SVFGNode *formal : graph_->getFormalParms(target)) {
        auto *parameter = dyn_cast<FormalParmSVFGNode>(formal);
        if (!parameter || parameter->getParamIndex() != 0 || !inScope(formal))
          continue;
        if (addOwnedEdge(*actual, *formal, SVFGEdgeK::CallDir, fork))
          ++stats_.forkParameterEdges;
      }
    }

    DominatorTree dominators(*const_cast<Function *>(fork->getFunction()));
    for (SVFGNode *formal : graph_->getFormalIns(target)) {
      if (!formal || !inScope(formal))
        continue;
      const SVFGNodeBS *formalPointsTo = formal->getPointsTo();
      if (!formalPointsTo || formalPointsTo->empty())
        continue;

      std::vector<SVFGNode *> sources;
      for (SVFGNode *actualIn : graph_->getActualIns(fork))
        sources.push_back(actualIn);
      for (SVFGNode *callerIn : graph_->getFormalIns(fork->getFunction()))
        sources.push_back(callerIn);
      for (auto &entry : *graph_) {
        auto *store = dyn_cast<StoreSVFGNode>(entry.second);
        const Instruction *instruction =
            store ? store->getInstruction() : nullptr;
        if (!instruction || instruction->getFunction() != fork->getFunction() ||
            !dominators.dominates(instruction, fork))
          continue;
        sources.push_back(store);
      }

      for (SVFGNode *source : sources) {
        if (!source || !inScope(source))
          continue;
        SVFGNodeBS guard = intersectTargets(*source, *formal);
        if (guard.empty())
          continue;
        if (addOwnedEdge(*source, *formal, SVFGEdgeK::CallAIn, fork, guard))
          ++stats_.forkMemoryEdges;
      }
    }
  }
}

void ThreadAwareSVFGBuilder::connectJoinFlow() {
  if (!threadTree_ || !graph_->getICFG())
    return;
  for (const CallBase *join : threadTree_->threadCallGraph().joinSites()) {
    std::vector<const Function *> joined = threadTree_->joinedFunctions(join);
    if (joined.empty() || !join || !join->getFunction())
      continue;
    ICFGNode *joinNode =
        const_cast<ICFG *>(graph_->getICFG())->getRetICFGNode(join);
    DominatorTree dominators(*const_cast<Function *>(join->getFunction()));

    for (const Function *target : joined) {
      for (SVFGNode *formal : graph_->getFormalOuts(target)) {
        auto *formalOut = dyn_cast<FormalOutSVFGNode>(formal);
        if (!formalOut || !inScope(formalOut))
          continue;
        ActualOutSVFGNode *actualOut = nullptr;
        for (SVFGNode *existing : graph_->getActualOuts(join)) {
          auto *candidate = dyn_cast<ActualOutSVFGNode>(existing);
          if (candidate && candidate->getMemReg() == formalOut->getMemReg()) {
            actualOut = candidate;
            break;
          }
        }
        if (!actualOut) {
          actualOut = new ActualOutSVFGNode(
              graph_->getNextNodeId(), joinNode, join, formalOut->getMemReg(),
              formalOut->getDefSVFVars(), formalOut->getSSAVersion());
          graph_->addNode(actualOut);
          graph_->addActualOut(join, actualOut);
          ++stats_.joinMemoryNodes;
        }
        if (addOwnedEdge(*formalOut, *actualOut, SVFGEdgeK::RetAOut, join,
                         formalOut->getDefSVFVars()))
          ++stats_.joinMemoryEdges;

        for (auto &entry : *graph_) {
          SVFGNode *destination = entry.second;
          if (!destination || destination == actualOut || !inScope(destination))
            continue;
          if (!isa<LoadSVFGNode, StoreSVFGNode>(destination))
            continue;
          const Instruction *instruction = destination->getInstruction();
          if (!instruction ||
              instruction->getFunction() != join->getFunction() ||
              !dominators.dominates(join, instruction))
            continue;
          SVFGNodeBS guard = intersectTargets(*actualOut, *destination);
          if (guard.empty())
            continue;
          if (addOwnedEdge(*actualOut, *destination, SVFGEdgeK::IntraIndirect,
                           nullptr, guard))
            ++stats_.joinMemoryEdges;
        }
      }
    }
  }
}

void ThreadAwareSVFGBuilder::connectForkJoinMemoryFlow() {
  connectForkFlow();
  connectJoinFlow();
}

const ThreadAwareSVFGBuilder::Statistics &ThreadAwareSVFGBuilder::build() {
  clear();
  stats_ = {};
  connectForkJoinMemoryFlow();

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
