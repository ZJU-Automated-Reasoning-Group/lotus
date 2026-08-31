#include "Alias/InclusionBased/FlowSensitive/FlowSensitivePTA.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <unordered_set>

#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>

using namespace llvm;
using namespace lotus::analysis;

namespace lotus::alias {

FlowSensitivePTA::FlowSensitivePTA(const SVFG &graph)
    : FlowSensitivePTA(graph, Config{}) {}

FlowSensitivePTA::FlowSensitivePTA(const SVFG &graph, Config config)
    : graph_(&graph), config_(config) {}

bool FlowSensitivePTA::inScope(const SVFGNode *node) const {
  return node && (!config_.scope || config_.scope->contains(node));
}

FlowSensitivePTA::StoredSet FlowSensitivePTA::singleton(ObjectID object) {
  StoredSet set;
  if (config_.setBackend == PointsToSetBackend::HashConsed)
    set.interned = arena_.singleton(object);
  else
    set.mutableSet.insert(object);
  return set;
}

const FlowSensitivePTA::PointsToSet &
FlowSensitivePTA::materialize(const StoredSet &set) const {
  return config_.setBackend == PointsToSetBackend::HashConsed
             ? arena_.get(set.interned)
             : set.mutableSet;
}

bool FlowSensitivePTA::merge(StoredSet &destination, const StoredSet &source) {
  if (config_.setBackend == PointsToSetBackend::HashConsed) {
    const auto old = destination.interned;
    destination.interned = arena_.unite(old, source.interned);
    return old != destination.interned;
  }
  const std::size_t oldSize = destination.mutableSet.size();
  destination.mutableSet.insert(source.mutableSet.begin(),
                                source.mutableSet.end());
  return oldSize != destination.mutableSet.size();
}

bool FlowSensitivePTA::assign(StoredSet &destination, const StoredSet &source) {
  if (config_.setBackend == PointsToSetBackend::HashConsed) {
    if (destination.interned == source.interned)
      return false;
    destination.interned = source.interned;
    return true;
  }
  if (destination.mutableSet == source.mutableSet)
    return false;
  destination.mutableSet = source.mutableSet;
  return true;
}

bool FlowSensitivePTA::mergeState(MemoryState &destination,
                                  const MemoryState &source) {
  bool changed = false;
  for (const auto &[object, pointsTo] : source)
    changed |= merge(destination[object], pointsTo);
  return changed;
}

bool FlowSensitivePTA::assignState(MemoryState &destination,
                                   const MemoryState &source) {
  if (destination.size() == source.size()) {
    bool equal = true;
    for (const auto &[object, pointsTo] : source) {
      auto current = destination.find(object);
      if (current == destination.end() ||
          materialize(current->second) != materialize(pointsTo)) {
        equal = false;
        break;
      }
    }
    if (equal)
      return false;
  }
  destination = source;
  return true;
}

const FlowSensitivePTA::StoredSet &
FlowSensitivePTA::topSet(const SVFGNode *node) const {
  static const StoredSet empty;
  if (!node)
    return empty;
  auto it = topLevelPointsTo_.find(node->getId());
  return it == topLevelPointsTo_.end() ? empty : it->second;
}

const FlowSensitivePTA::MemoryState &
FlowSensitivePTA::outState(const SVFGNode *node) const {
  static const MemoryState empty;
  if (!node)
    return empty;
  auto it = dfOut_.find(node->getId());
  return it == dfOut_.end() ? empty : it->second;
}

const Value *FlowSensitivePTA::accessPointer(const Instruction *instruction) {
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

FlowSensitivePTA::StoredSet
FlowSensitivePTA::pointerTargets(const Value *pointer) {
  if (!pointer || !pointer->getType()->isPointerTy())
    return {};
  return topSet(graph_->getValueNode(pointer));
}

FlowSensitivePTA::StoredSet
FlowSensitivePTA::directInput(const SVFGNode &node) {
  StoredSet result;
  for (const SVFGEdge *edge : node.getInEdges()) {
    if (edge && inScope(edge->getSrcNode()) &&
        isDirectVFGEdge(edge->getEdgeKind()))
      merge(result, topSet(edge->getSrcNode()));
  }
  return result;
}

bool FlowSensitivePTA::isStrongUpdate(const PointsToSet &targets) const {
  return targets.size() == 1 && graph_->isSingletonObject(*targets.begin()) &&
         !graph_->isUnknownObject(*targets.begin());
}

bool FlowSensitivePTA::resolveIndirectCalls(const SVFGNode &node,
                                            const StoredSet &pointsTo) {
  if (!config_.connectIndirectCall || !node.hasValueId())
    return false;
  bool changed = false;
  for (const CallBase *callSite : graph_->getIndCallSites(node.getValueId())) {
    for (ObjectID object : materialize(pointsTo)) {
      const auto *target =
          dyn_cast_or_null<Function>(graph_->getObjectValue(object));
      if (!target || target->isDeclaration())
        continue;
      if (config_.connectIndirectCall(callSite, target)) {
        ++stats_.indirectCallEdges;
        changed = true;
      }
    }
  }
  return changed;
}

bool FlowSensitivePTA::transfer(const SVFGNode &node) {
  ++stats_.nodeProcesses;
  bool changed = false;
  MemoryState incoming;
  for (const SVFGEdge *edge : node.getInEdges())
    if (edge && inScope(edge->getSrcNode()))
      mergeState(incoming, outState(edge->getSrcNode()));
  changed |= assignState(dfIn_[node.getId()], incoming);

  MemoryState outgoing = incoming;
  StoredSet top = topSet(&node);
  if (const auto *address = dyn_cast<AddrSVFGNode>(&node)) {
    ObjectID object = address->getObjectId();
    if (object == 0 && address->getValue())
      object = graph_->getObjectId(address->getValue());
    if (object != 0)
      top = singleton(object);
  } else if (const auto *load = dyn_cast<LoadSVFGNode>(&node)) {
    top = {};
    StoredSet targets = pointerTargets(accessPointer(load->getInstruction()));
    PointsToSet targetSet = materialize(targets);
    if (targetSet.empty())
      targetSet = load->getMemoryPointsTo();
    for (ObjectID object : targetSet) {
      auto value = incoming.find(object);
      if (value != incoming.end())
        merge(top, value->second);
    }
  } else if (const auto *store = dyn_cast<StoreSVFGNode>(&node)) {
    const auto *instruction =
        dyn_cast_or_null<StoreInst>(store->getInstruction());
    StoredSet stored = instruction
                           ? pointerTargets(instruction->getValueOperand())
                           : directInput(node);
    StoredSet targets = instruction
                            ? pointerTargets(instruction->getPointerOperand())
                            : StoredSet{};
    PointsToSet targetSet = materialize(targets);
    if (targetSet.empty())
      targetSet = store->getMemoryPointsTo();
    const bool strong = isStrongUpdate(targetSet);
    for (ObjectID object : targetSet) {
      if (strong) {
        assign(outgoing[object], stored);
        ++stats_.strongUpdates;
      } else {
        merge(outgoing[object], stored);
        ++stats_.weakUpdates;
      }
    }
  } else if (const auto *gep = dyn_cast<GepSVFGNode>(&node)) {
    const ObjectID field =
        gep->getValue() ? graph_->getObjectId(gep->getValue()) : 0;
    top = field != 0 ? singleton(field) : directInput(node);
  } else if (!isa<MSSASVFGNode>(&node)) {
    top = directInput(node);
  }

  const bool topChanged = assign(topLevelPointsTo_[node.getId()], top);
  changed |= topChanged;
  if (topChanged && resolveIndirectCalls(node, top))
    topologyChanged_ = true;
  changed |= assignState(dfOut_[node.getId()], outgoing);
  return changed;
}

FlowSensitivePTA::SCCInfo FlowSensitivePTA::computeSCCs() const {
  SCCInfo result;
  std::unordered_map<NodeID, int> index, lowlink;
  std::unordered_set<NodeID> onStack;
  std::vector<const SVFGNode *> stack;
  int nextIndex = 0;
  std::function<void(const SVFGNode *)> visit = [&](const SVFGNode *node) {
    index[node->getId()] = lowlink[node->getId()] = nextIndex++;
    stack.push_back(node);
    onStack.insert(node->getId());
    for (const SVFGEdge *edge : node->getOutEdges()) {
      const SVFGNode *successor = edge ? edge->getDstNode() : nullptr;
      if (!inScope(successor))
        continue;
      if (index.find(successor->getId()) == index.end()) {
        visit(successor);
        lowlink[node->getId()] =
            std::min(lowlink[node->getId()], lowlink[successor->getId()]);
      } else if (onStack.count(successor->getId()) != 0) {
        lowlink[node->getId()] =
            std::min(lowlink[node->getId()], index[successor->getId()]);
      }
    }
    if (lowlink[node->getId()] != index[node->getId()])
      return;
    result.components.emplace_back();
    do {
      const SVFGNode *member = stack.back();
      stack.pop_back();
      onStack.erase(member->getId());
      result.components.back().push_back(member);
      if (member == node)
        break;
    } while (!stack.empty());
  };
  for (const auto &entry : *graph_)
    if (inScope(entry.second) && index.find(entry.first) == index.end())
      visit(entry.second);

  for (std::size_t i = 0; i < result.components.size(); ++i)
    for (const SVFGNode *node : result.components[i])
      result.nodeToComponent[node->getId()] = i;
  result.successors.resize(result.components.size());
  for (std::size_t i = 0; i < result.components.size(); ++i) {
    std::unordered_set<std::size_t> successors;
    for (const SVFGNode *node : result.components[i])
      for (const SVFGEdge *edge : node->getOutEdges())
        if (edge && inScope(edge->getDstNode())) {
          const std::size_t target =
              result.nodeToComponent[edge->getDstNode()->getId()];
          if (target != i)
            successors.insert(target);
        }
    result.successors[i].assign(successors.begin(), successors.end());
  }
  return result;
}

const FlowSensitivePTA::Statistics &FlowSensitivePTA::solve() {
  topLevelPointsTo_.clear();
  dfIn_.clear();
  dfOut_.clear();
  stats_ = {};
  if (config_.setBackend == PointsToSetBackend::HashConsed)
    arena_.reset();
  do {
    topologyChanged_ = false;
    SCCInfo scc = computeSCCs();
    stats_.sccs = scc.components.size();
    stats_.nodes = 0;
    std::queue<std::size_t> worklist;
    std::vector<bool> queued(scc.components.size(), true);
    for (std::size_t i = 0; i < scc.components.size(); ++i) {
      worklist.push(i);
      stats_.nodes += scc.components[i].size();
      stats_.maxSccSize =
          std::max(stats_.maxSccSize, scc.components[i].size());
    }
    while (!worklist.empty()) {
      const std::size_t component = worklist.front();
      worklist.pop();
      queued[component] = false;
      bool anyChanged = false;
      bool localChanged;
      do {
        localChanged = false;
        for (const SVFGNode *node : scc.components[component])
          localChanged |= transfer(*node);
        anyChanged |= localChanged;
      } while (localChanged && !topologyChanged_);
      if (topologyChanged_)
        break;
      if (anyChanged)
        for (std::size_t successor : scc.successors[component])
          if (!queued[successor]) {
            queued[successor] = true;
            worklist.push(successor);
          }
    }
  } while (topologyChanged_);
  for (const auto &[node, pointsTo] : topLevelPointsTo_)
    stats_.topLevelFacts += materialize(pointsTo).size();
  for (const auto &[node, state] : dfIn_)
    for (const auto &[object, pointsTo] : state)
      stats_.memoryInFacts += materialize(pointsTo).size();
  for (const auto &[node, state] : dfOut_)
    for (const auto &[object, pointsTo] : state)
      stats_.memoryOutFacts += materialize(pointsTo).size();
  if (config_.setBackend == PointsToSetBackend::HashConsed) {
    const auto hashStats = arena_.statistics();
    stats_.hashConsedUniqueSets = hashStats.uniqueSets;
    stats_.hashConsedUnionCacheHits = hashStats.unionCacheHits;
  }
  return stats_;
}

const FlowSensitivePTA::PointsToSet &
FlowSensitivePTA::pointsTo(const SVFGNode *node) const {
  static const PointsToSet empty;
  if (!node)
    return empty;
  auto it = topLevelPointsTo_.find(node->getId());
  return it == topLevelPointsTo_.end() ? empty : materialize(it->second);
}

std::optional<FlowSensitivePTA::PointsToSet>
FlowSensitivePTA::pointsTo(const Value *value) const {
  if (!value || !value->getType()->isPointerTy())
    return std::nullopt;
  const SVFGNode *node = graph_->getValueNode(value);
  if (!inScope(node))
    return std::nullopt;
  return pointsTo(node);
}

const FlowSensitivePTA::PointsToSet &
FlowSensitivePTA::memoryIn(const SVFGNode *node, ObjectID object) const {
  static const PointsToSet empty;
  if (!node)
    return empty;
  auto state = dfIn_.find(node->getId());
  if (state == dfIn_.end())
    return empty;
  auto value = state->second.find(object);
  return value == state->second.end() ? empty : materialize(value->second);
}

const FlowSensitivePTA::PointsToSet &
FlowSensitivePTA::memoryOut(const SVFGNode *node, ObjectID object) const {
  static const PointsToSet empty;
  if (!node)
    return empty;
  auto state = dfOut_.find(node->getId());
  if (state == dfOut_.end())
    return empty;
  auto value = state->second.find(object);
  return value == state->second.end() ? empty : materialize(value->second);
}

std::optional<bool> FlowSensitivePTA::mayAlias(const Value *lhs,
                                               const Value *rhs) const {
  auto left = pointsTo(lhs);
  auto right = pointsTo(rhs);
  if (!left || !right)
    return std::nullopt;
  const PointsToSet *small = &*left, *large = &*right;
  if (large->size() < small->size())
    std::swap(small, large);
  for (ObjectID object : *small)
    if (large->count(object) != 0)
      return true;
  return false;
}

} // namespace lotus::alias
