#include "Concurrency/ValueFlow/SparseFlowSensitivePTA.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>

using namespace llvm;

namespace lotus::analysis {
namespace {

bool mergeSet(SVFGNodeBS &destination, const SVFGNodeBS &source) {
  const std::size_t oldSize = destination.size();
  destination.insert(source.begin(), source.end());
  return oldSize != destination.size();
}

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

bool SparseFlowSensitivePTA::containsUnknown(
    const SVFGNodeBS &pointsToSet) const {
  return std::any_of(pointsToSet.begin(), pointsToSet.end(),
                     [&](uint32_t id) { return graph_->isUnknownObject(id); });
}

bool SparseFlowSensitivePTA::inScope(const SVFGNode *node) const {
  return node && (!scope_ || scope_->contains(node));
}

bool SparseFlowSensitivePTA::isStrongUpdate(const StoreSVFGNode &store) const {
  const SVFGNodeBS &targets = store.getMemoryPointsTo();
  if (targets.size() != 1 || containsUnknown(targets))
    return false;

  return graph_->isSingletonObject(*targets.begin());
}

const SparseFlowSensitivePTA::Statistics &SparseFlowSensitivePTA::solve() {
  nodePointsTo_.clear();
  memoryValues_.clear();
  nodeComplete_.clear();
  memoryComplete_.clear();
  stats_ = {};

  for (const auto &entry : *graph_) {
    const SVFGNode *node = entry.second;
    if (!inScope(node))
      continue;
    if (const auto *addr = dyn_cast_or_null<AddrSVFGNode>(node)) {
      uint32_t object = addr->getObjectId();
      if (object == 0 && addr->getValue())
        object = graph_->getObjectId(addr->getValue());
      if (object != 0) {
        nodePointsTo_[node->getId()].insert(object);
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
        SVFGNodeBS transfer;
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
            mergeSet(transfer, pointsTo(storedNode));
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
          mergeSet(transfer, memoryValue(source));
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
        SVFGNodeBS transfer;
        bool complete = true;
        bool sawMemoryInput = false;
        for (const SVFGEdge *edge : load->getInEdges()) {
          if (!edge || !isIndirectVFGEdge(edge->getEdgeKind()))
            continue;
          const SVFGNode *source = edge->getSrcNode();
          if (!inScope(source))
            continue;
          sawMemoryInput = true;
          mergeSet(transfer, memoryValue(source));
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
        SVFGNodeBS transfer;
        bool complete = true;
        bool sawMemoryInput = false;
        for (const SVFGEdge *edge : node->getInEdges()) {
          if (!edge || !isIndirectVFGEdge(edge->getEdgeKind()))
            continue;
          const SVFGNode *source = edge->getSrcNode();
          if (!inScope(source))
            continue;
          sawMemoryInput = true;
          mergeSet(transfer, memoryValue(source));
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

      SVFGNodeBS transfer;
      bool complete = true;
      bool sawDirectInput = false;
      for (const SVFGEdge *edge : node->getInEdges()) {
        if (!edge || !isDirectVFGEdge(edge->getEdgeKind()))
          continue;
        const SVFGNode *source = edge->getSrcNode();
        if (!inScope(source))
          continue;
        sawDirectInput = true;
        mergeSet(transfer, pointsTo(source));
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
    stats_.pointsToFacts += entry.second.size();
  for (const auto &entry : memoryValues_)
    stats_.memoryFacts += entry.second.size();
  stats_.strongUpdates = strongUpdateNodes.size();
  return stats_;
}

const SVFGNodeBS &SparseFlowSensitivePTA::pointsTo(const SVFGNode *node) const {
  static const SVFGNodeBS empty;
  if (!node)
    return empty;
  auto it = nodePointsTo_.find(node->getId());
  return it == nodePointsTo_.end() ? empty : it->second;
}

const SVFGNodeBS &
SparseFlowSensitivePTA::memoryValue(const SVFGNode *node) const {
  static const SVFGNodeBS empty;
  if (!node)
    return empty;
  auto it = memoryValues_.find(node->getId());
  return it == memoryValues_.end() ? empty : it->second;
}

bool SparseFlowSensitivePTA::hasCompletePointsTo(const SVFGNode *node) const {
  if (!node)
    return false;
  auto it = nodeComplete_.find(node->getId());
  return it != nodeComplete_.end() && it->second;
}

std::optional<SVFGNodeBS>
SparseFlowSensitivePTA::pointsTo(const Value *value) const {
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
SparseFlowSensitivePTA::accessTargets(const Instruction *access) const {
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
SparseFlowSensitivePTA::mayAliasAccesses(const Instruction *lhs,
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
