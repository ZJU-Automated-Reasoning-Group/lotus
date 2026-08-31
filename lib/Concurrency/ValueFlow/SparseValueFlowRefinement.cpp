#include "Concurrency/ValueFlow/SparseValueFlowRefinement.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>

using namespace llvm;

namespace lotus::analysis {
namespace {

const Value *accessPointer(const Instruction *instruction) {
  if (const auto *load = dyn_cast_or_null<LoadInst>(instruction))
    return load->getPointerOperand();
  if (const auto *store = dyn_cast_or_null<StoreInst>(instruction))
    return store->getPointerOperand();
  if (const auto *rmw = dyn_cast_or_null<AtomicRMWInst>(instruction))
    return rmw->getPointerOperand();
  if (const auto *cmpxchg = dyn_cast_or_null<AtomicCmpXchgInst>(instruction))
    return cmpxchg->getPointerOperand();
  return nullptr;
}

} // namespace

SparseValueFlowRefinement::WorkingSet
SparseValueFlowRefinement::singleton(uint32_t object) {
  WorkingSet result;
  if (backend_ == lotus::alias::PointsToSetBackend::HashConsed)
    result.interned = hashConsedArena_.singleton(object);
  else
    result.mutableSet.insert(object);
  return result;
}

bool SparseValueFlowRefinement::mergeSet(WorkingSet &destination,
                                         const WorkingSet &source) {
  if (backend_ == lotus::alias::PointsToSetBackend::HashConsed) {
    const auto old = destination.interned;
    destination.interned =
        hashConsedArena_.unite(destination.interned, source.interned);
    return old != destination.interned;
  }
  const std::size_t oldSize = destination.mutableSet.size();
  destination.mutableSet.insert(source.mutableSet.begin(),
                                source.mutableSet.end());
  return oldSize != destination.mutableSet.size();
}

bool SparseValueFlowRefinement::mergeSet(WorkingSet &destination,
                                         const SVFGNodeBS &source) {
  if (backend_ == lotus::alias::PointsToSetBackend::HashConsed) {
    WorkingSet interned;
    interned.interned = hashConsedArena_.intern(source);
    return mergeSet(destination, interned);
  }
  const std::size_t oldSize = destination.mutableSet.size();
  destination.mutableSet.insert(source.begin(), source.end());
  return oldSize != destination.mutableSet.size();
}

const SVFGNodeBS &
SparseValueFlowRefinement::materialize(const WorkingSet &set) const {
  return backend_ == lotus::alias::PointsToSetBackend::HashConsed
             ? hashConsedArena_.get(set.interned)
             : set.mutableSet;
}

const SparseValueFlowRefinement::WorkingSet &
SparseValueFlowRefinement::nodeSet(const SVFGNode *node) const {
  static const WorkingSet empty;
  if (!node)
    return empty;
  auto it = nodePointsTo_.find(node->getId());
  return it == nodePointsTo_.end() ? empty : it->second;
}

const SparseValueFlowRefinement::WorkingSet &
SparseValueFlowRefinement::memorySet(const SVFGNode *node) const {
  static const WorkingSet empty;
  if (!node)
    return empty;
  auto it = memoryValues_.find(node->getId());
  return it == memoryValues_.end() ? empty : it->second;
}

bool SparseValueFlowRefinement::containsUnknown(
    const SVFGNodeBS &pointsToSet) const {
  return std::any_of(pointsToSet.begin(), pointsToSet.end(),
                     [&](uint32_t id) { return graph_->isUnknownObject(id); });
}

bool SparseValueFlowRefinement::inScope(const SVFGNode *node) const {
  return node && (!scope_ || scope_->contains(node));
}

bool SparseValueFlowRefinement::isStrongUpdate(
    const StoreSVFGNode &store) const {
  const SVFGNodeBS &targets = store.getMemoryPointsTo();
  if (targets.size() != 1 || containsUnknown(targets))
    return false;

  return graph_->isSingletonObject(*targets.begin());
}

const SparseValueFlowRefinement::Statistics &
SparseValueFlowRefinement::solve() {
  nodePointsTo_.clear();
  memoryValues_.clear();
  nodeComplete_.clear();
  memoryComplete_.clear();
  stats_ = {};
  if (backend_ == lotus::alias::PointsToSetBackend::HashConsed)
    hashConsedArena_.reset();

  for (const auto &entry : *graph_) {
    const SVFGNode *node = entry.second;
    if (!inScope(node))
      continue;
    if (const auto *addr = dyn_cast_or_null<AddrSVFGNode>(node)) {
      uint32_t object = addr->getObjectId();
      if (object == 0 && addr->getValue())
        object = graph_->getObjectId(addr->getValue());
      if (object != 0) {
        nodePointsTo_[node->getId()] = singleton(object);
        nodeComplete_[node->getId()] = !graph_->isUnknownObject(object);
      }
    } else if (isa<NullPtrSVFGNode>(node)) {
      nodeComplete_[node->getId()] = true;
    }
  }

  bool changed = true;
  std::unordered_set<uint32_t> strongUpdateNodes;
  const std::size_t maxIterations =
      std::max<std::size_t>(1, graph_->getNumNodes() * 2U + 1U);
  while (changed && stats_.iterations < maxIterations) {
    changed = false;
    ++stats_.iterations;

    for (const auto &entry : *graph_) {
      const SVFGNode *node = entry.second;
      if (!inScope(node) || isa<AddrSVFGNode>(node) ||
          isa<NullPtrSVFGNode>(node))
        continue;

      const uint32_t nodeId = node->getId();

      if (const auto *store = dyn_cast<StoreSVFGNode>(node)) {
        WorkingSet transfer;
        bool complete = true;
        const StoreInst *instruction =
            dyn_cast_or_null<StoreInst>(store->getInstruction());
        const Value *storedValue =
            instruction ? instruction->getValueOperand() : nullptr;
        if (storedValue && storedValue->getType()->isPointerTy()) {
          const SVFGNode *storedNode = graph_->getValueNode(storedValue);
          if (!storedNode || !hasCompletePointsTo(storedNode))
            complete = false;
          else
            mergeSet(transfer, nodeSet(storedNode));
        }

        const bool strong = isStrongUpdate(*store);
        bool sawStrongSequentialInput = false;
        for (const SVFGEdge *edge : store->getInEdges()) {
          if (!edge || !isIndirectVFGEdge(edge->getEdgeKind()))
            continue;
          const SVFGNode *source = edge->getSrcNode();
          if (!inScope(source))
            continue;
          const bool threadEdge = edge->isThreadMHPEdge();
          if (!threadEdge && strong) {
            sawStrongSequentialInput = true;
            continue;
          }
          mergeSet(transfer, memorySet(source));
          if (!memoryComplete_[source->getId()])
            complete = false;
        }
        if (strong && sawStrongSequentialInput)
          strongUpdateNodes.insert(nodeId);

        changed |= mergeSet(memoryValues_[nodeId], transfer);
        if (complete && !memoryComplete_[nodeId]) {
          memoryComplete_[nodeId] = true;
          changed = true;
        }
        continue;
      }

      if (const auto *load = dyn_cast<LoadSVFGNode>(node)) {
        WorkingSet transfer;
        bool complete = true;
        bool sawMemoryInput = false;
        for (const SVFGEdge *edge : load->getInEdges()) {
          if (!edge || !isIndirectVFGEdge(edge->getEdgeKind()))
            continue;
          const SVFGNode *source = edge->getSrcNode();
          if (!inScope(source))
            continue;
          sawMemoryInput = true;
          mergeSet(transfer, memorySet(source));
          if (!memoryComplete_[source->getId()])
            complete = false;
        }
        changed |= mergeSet(nodePointsTo_[nodeId], transfer);
        if (sawMemoryInput && complete && !nodeComplete_[nodeId]) {
          nodeComplete_[nodeId] = true;
          changed = true;
        }
        continue;
      }

      if (isa<MSSASVFGNode>(node)) {
        WorkingSet transfer;
        bool complete = true;
        bool sawMemoryInput = false;
        for (const SVFGEdge *edge : node->getInEdges()) {
          if (!edge || !isIndirectVFGEdge(edge->getEdgeKind()))
            continue;
          const SVFGNode *source = edge->getSrcNode();
          if (!inScope(source))
            continue;
          sawMemoryInput = true;
          mergeSet(transfer, memorySet(source));
          if (!memoryComplete_[source->getId()])
            complete = false;
        }
        changed |= mergeSet(memoryValues_[nodeId], transfer);
        if (sawMemoryInput && complete && !memoryComplete_[nodeId]) {
          memoryComplete_[nodeId] = true;
          changed = true;
        }
        continue;
      }

      if (!node->getValue() || !node->getValue()->getType()->isPointerTy())
        continue;

      WorkingSet transfer;
      bool complete = true;
      bool sawDirectInput = false;
      for (const SVFGEdge *edge : node->getInEdges()) {
        if (!edge || !isDirectVFGEdge(edge->getEdgeKind()))
          continue;
        const SVFGNode *source = edge->getSrcNode();
        if (!inScope(source))
          continue;
        sawDirectInput = true;
        mergeSet(transfer, nodeSet(source));
        if (!hasCompletePointsTo(source))
          complete = false;
      }
      changed |= mergeSet(nodePointsTo_[nodeId], transfer);
      if (sawDirectInput && complete && !nodeComplete_[nodeId]) {
        nodeComplete_[nodeId] = true;
        changed = true;
      }
    }
  }

  for (const auto &entry : nodePointsTo_)
    stats_.pointsToFacts += materialize(entry.second).size();
  for (const auto &entry : memoryValues_)
    stats_.memoryFacts += materialize(entry.second).size();
  stats_.strongUpdates = strongUpdateNodes.size();
  if (backend_ == lotus::alias::PointsToSetBackend::HashConsed) {
    const auto hashStats = hashConsedArena_.statistics();
    stats_.hashConsedUniqueSets = hashStats.uniqueSets;
    stats_.hashConsedStoredElements = hashStats.storedElements;
    stats_.hashConsedUnionRequests = hashStats.unionRequests;
    stats_.hashConsedUnionCacheHits = hashStats.unionCacheHits;
  }
  return stats_;
}

const SVFGNodeBS &
SparseValueFlowRefinement::pointsTo(const SVFGNode *node) const {
  static const SVFGNodeBS empty;
  if (!node)
    return empty;
  auto it = nodePointsTo_.find(node->getId());
  return it == nodePointsTo_.end() ? empty : materialize(it->second);
}

const SVFGNodeBS &
SparseValueFlowRefinement::memoryValue(const SVFGNode *node) const {
  static const SVFGNodeBS empty;
  if (!node)
    return empty;
  auto it = memoryValues_.find(node->getId());
  return it == memoryValues_.end() ? empty : materialize(it->second);
}

bool SparseValueFlowRefinement::hasCompletePointsTo(
    const SVFGNode *node) const {
  if (!node)
    return false;
  auto it = nodeComplete_.find(node->getId());
  return it != nodeComplete_.end() && it->second;
}

std::optional<SVFGNodeBS>
SparseValueFlowRefinement::pointsTo(const Value *value) const {
  if (!value || !value->getType()->isPointerTy())
    return std::nullopt;
  const SVFGNode *node = graph_->getValueNode(value);
  if (!inScope(node) || !hasCompletePointsTo(node))
    return std::nullopt;
  const SVFGNodeBS &result = pointsTo(node);
  if (containsUnknown(result))
    return std::nullopt;
  return result;
}

std::optional<SVFGNodeBS>
SparseValueFlowRefinement::accessTargets(const Instruction *access) const {
  const Value *pointer = accessPointer(access);
  if (!pointer)
    return std::nullopt;
  if (auto result = pointsTo(pointer))
    return result;

  // Fall back to the complete flow-insensitive guard attached to the access.
  const SVFGNode *node = graph_->getDef(access);
  if (!inScope(node))
    return std::nullopt;
  const SVFGNodeBS *targets = node ? node->getPointsTo() : nullptr;
  if (!targets || targets->empty() || containsUnknown(*targets))
    return std::nullopt;
  return *targets;
}

std::optional<bool>
SparseValueFlowRefinement::mayAliasAccesses(const Instruction *lhs,
                                            const Instruction *rhs) const {
  std::optional<SVFGNodeBS> lhsTargets = accessTargets(lhs);
  std::optional<SVFGNodeBS> rhsTargets = accessTargets(rhs);
  if (!lhsTargets || !rhsTargets)
    return std::nullopt;

  const SVFGNodeBS *small = &*lhsTargets;
  const SVFGNodeBS *large = &*rhsTargets;
  if (large->size() < small->size())
    std::swap(small, large);
  for (uint32_t object : *small)
    if (large->count(object) != 0)
      return true;
  return false;
}

} // namespace lotus::analysis
