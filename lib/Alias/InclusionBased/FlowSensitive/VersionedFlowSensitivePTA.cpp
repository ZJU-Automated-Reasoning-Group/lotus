#include "Alias/InclusionBased/FlowSensitive/VersionedFlowSensitivePTA.h"

#include "IR/ICFG/CallGraph.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>

#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Casting.h>

using namespace llvm;
using namespace lotus::analysis;

namespace lotus::alias {

namespace {
constexpr VersionedFlowSensitivePTA::NodeID InitialMeldToken = 1;
} // namespace

VersionedFlowSensitivePTA::VersionedFlowSensitivePTA(const SVFG &graph)
    : VersionedFlowSensitivePTA(graph, Config{}) {}

VersionedFlowSensitivePTA::VersionedFlowSensitivePTA(const SVFG &graph,
                                                     Config config)
    : graph_(&graph), config_(std::move(config)) {}

bool VersionedFlowSensitivePTA::inScope(const SVFGNode *node) const {
  return node && (!config_.scope || config_.scope->contains(node));
}

void VersionedFlowSensitivePTA::initializeRecursiveFunctions() {
  recursiveFunctions_.clear();
  if (const LTCallGraph *callGraph = graph_->getRefinedCallGraph()) {
    std::unordered_map<const Function *, int> index;
    std::unordered_map<const Function *, int> lowlink;
    std::unordered_set<const Function *> onStack;
    std::vector<const Function *> stack;
    int nextIndex = 0;
    std::function<void(const Function *)> visit =
        [&](const Function *function) {
          index[function] = lowlink[function] = nextIndex++;
          stack.push_back(function);
          onStack.insert(function);
          const LTCallGraphNode *node = (*callGraph)[function];
          for (const auto &edge : *node) {
            const Function *callee =
                edge.second ? edge.second->getFunction() : nullptr;
            if (!callee || callee->isDeclaration())
              continue;
            if (index.find(callee) == index.end()) {
              visit(callee);
              lowlink[function] = std::min(lowlink[function], lowlink[callee]);
            } else if (onStack.count(callee) != 0) {
              lowlink[function] = std::min(lowlink[function], index[callee]);
            }
          }
          if (lowlink[function] != index[function])
            return;
          std::vector<const Function *> component;
          do {
            const Function *member = stack.back();
            stack.pop_back();
            onStack.erase(member);
            component.push_back(member);
            if (member == function)
              break;
          } while (!stack.empty());
          bool recursive = component.size() > 1;
          if (!recursive && !component.empty()) {
            const LTCallGraphNode *single = (*callGraph)[component.front()];
            for (const auto &edge : *single)
              recursive |= edge.second == single;
          }
          if (recursive)
            recursiveFunctions_.insert(component.begin(), component.end());
        };
    for (const auto &entry : *callGraph)
      if (entry.first && !entry.first->isDeclaration() &&
          index.find(entry.first) == index.end())
        visit(entry.first);
    return;
  }

  Module *module = nullptr;
  for (const auto &[nodeID, node] : *graph_)
    if (const Function *function = node->getFunction()) {
      module = const_cast<Module *>(function->getParent());
      break;
    }
  if (!module)
    return;
  CallGraph callGraph(*module);
  for (scc_iterator<CallGraph *> it = scc_begin(&callGraph),
                                 end = scc_end(&callGraph);
       it != end; ++it) {
    const std::vector<CallGraphNode *> &component = *it;
    bool recursive = component.size() > 1;
    if (!recursive && component.size() == 1)
      for (const auto &edge : *component.front())
        recursive |= edge.second == component.front();
    if (recursive)
      for (CallGraphNode *node : component)
        if (node && node->getFunction())
          recursiveFunctions_.insert(node->getFunction());
  }
}

bool VersionedFlowSensitivePTA::functionHasKnownCaller(
    const Function *function) const {
  if (!function)
    return false;
  if (const LTCallGraph *callGraph = graph_->getRefinedCallGraph())
    for (const auto &entry : *callGraph) {
      if (!entry.second)
        continue;
      for (const auto &edge : *entry.second)
        if (edge.second && edge.second->getFunction() == function)
          return true;
    }
  return false;
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::expandIndirectObjects(
    const PointsToSet &objects) const {
  PointsToSet expanded = objects;
  for (ObjectID object : objects) {
    if (!graph_->isFieldInsensitiveObject(object))
      continue;
    const PointsToSet fields = graph_->getFieldObjects(object);
    expanded.insert(fields.begin(), fields.end());
  }
  return expanded;
}

bool VersionedFlowSensitivePTA::edgeCarriesObject(const SVFGEdge &edge,
                                                  ObjectID object) const {
  if (!isIndirectVFGEdge(edge.getEdgeKind()) || edge.getPointsTo().empty())
    return false;
  for (ObjectID guarded : edge.getPointsTo()) {
    if (graph_->isUnknownObject(guarded))
      return true;
    if (guarded == object)
      return true;
    if (graph_->isFieldInsensitiveObject(guarded) &&
        graph_->getFieldObjects(guarded).count(object) != 0)
      return true;
  }
  return false;
}

bool VersionedFlowSensitivePTA::storeMayTarget(const StoreSVFGNode &store,
                                               ObjectID object) const {
  PointsToSet targets = expandIndirectObjects(store.getMemoryPointsTo());
  if (targets.empty()) {
    const auto *instruction =
        dyn_cast_or_null<StoreInst>(store.getInstruction());
    if (instruction) {
      const PointsToSet &known =
          graph_->getObjectIds(instruction->getPointerOperand());
      targets = expandIndirectObjects(known);
    }
  }
  for (ObjectID target : targets)
    if (target == object || graph_->isUnknownObject(target))
      return true;
  return false;
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::objectsWithFields(const Value *pointer) const {
  PointsToSet objects = pointerTargets(pointer);
  if (objects.empty() && pointer) {
    const PointsToSet &known = graph_->getObjectIds(pointer);
    objects.insert(known.begin(), known.end());
    if (ObjectID object = graph_->getObjectId(pointer))
      objects.insert(object);
  }
  PointsToSet expanded = objects;
  for (ObjectID object : objects) {
    const PointsToSet fields = graph_->getFieldObjects(object);
    expanded.insert(fields.begin(), fields.end());
  }
  return expanded;
}

bool VersionedFlowSensitivePTA::intrinsicMayDefine(
    const ActualOutSVFGNode &actualOut, ObjectID object) const {
  const auto *intrinsic =
      dyn_cast_or_null<IntrinsicInst>(actualOut.getCallSite());
  if (!intrinsic || intrinsic->arg_size() == 0)
    return false;
  switch (intrinsic->getIntrinsicID()) {
  case Intrinsic::memcpy:
  case Intrinsic::memmove:
  case Intrinsic::memset:
    break;
  default:
    return false;
  }
  const PointsToSet destinations =
      objectsWithFields(intrinsic->getArgOperand(0));
  return destinations.count(object) != 0 ||
         std::any_of(destinations.begin(), destinations.end(),
                     [&](ObjectID destination) {
                       return graph_->isUnknownObject(destination);
                     });
}

bool VersionedFlowSensitivePTA::memoryPhiNeedsInitial(
    const MSSAPhiSVFGNode &phi, ObjectID object) const {
  for (const SVFGEdge *edge : phi.getInEdges()) {
    const SVFGNode *source = edge ? edge->getSrcNode() : nullptr;
    if (isa_and_nonnull<EntryChiSVFGNode>(source))
      return true;
    if (const auto *formalIn = dyn_cast_or_null<FormalInSVFGNode>(source))
      if (!isDeltaNode(*source, object) &&
          !functionHasKnownCaller(formalIn->getFunction()))
        return true;
  }
  const ICFGNode *icfgNode = phi.getICFGNode();
  const BasicBlock *block = icfgNode ? icfgNode->getBasicBlock() : nullptr;
  if (!block)
    return false;
  const unsigned predecessorCount =
      static_cast<unsigned>(std::distance(pred_begin(block), pred_end(block)));
  return phi.getOpVerNum() < predecessorCount;
}

bool VersionedFlowSensitivePTA::isDeltaNode(const SVFGNode &node,
                                            ObjectID object) const {
  const auto *memoryNode = dyn_cast<MSSASVFGNode>(&node);
  if (!memoryNode)
    return false;
  const PointsToSet regions =
      expandIndirectObjects(memoryNode->getDefSVFVars());
  const bool carriesObject =
      regions.count(object) != 0 ||
      std::any_of(regions.begin(), regions.end(), [&](ObjectID region) {
        return graph_->isUnknownObject(region);
      });
  if (!carriesObject)
    return false;
  if (const Function *function = graph_->isFunEntrySVFGNode(&node))
    return !graph_->getIndCallSitesInvokingCallee(function).empty();
  if (const CallBase *callSite = graph_->isCallSiteRetSVFGNode(&node))
    return !callSite->getCalledFunction();
  return false;
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::relevantObjects() const {
  PointsToSet objects;
  for (const auto &[object, label] : graph_->getObjectDebugMap())
    objects.insert(object);
  for (const auto &[nodeID, node] : *graph_) {
    if (!inScope(node))
      continue;
    for (const SVFGEdge *edge : node->getOutEdges()) {
      if (!edge || !isIndirectVFGEdge(edge->getEdgeKind()))
        continue;
      const PointsToSet guarded = expandIndirectObjects(edge->getPointsTo());
      objects.insert(guarded.begin(), guarded.end());
    }
    if (const auto *store = dyn_cast<StoreSVFGNode>(node)) {
      const PointsToSet targets =
          expandIndirectObjects(store->getMemoryPointsTo());
      objects.insert(targets.begin(), targets.end());
    }
    if (const auto *load = dyn_cast<LoadSVFGNode>(node)) {
      const PointsToSet targets =
          expandIndirectObjects(load->getMemoryPointsTo());
      objects.insert(targets.begin(), targets.end());
    }
  }
  return objects;
}

VersionedFlowSensitivePTA::Version
VersionedFlowSensitivePTA::internVersion(ObjectID object, const MeldSet &meld) {
  auto &melds = versionMelds_[object];
  for (std::size_t i = 0; i < melds.size(); ++i)
    if (melds[i] == meld)
      return static_cast<Version>(i + 1);
  melds.push_back(meld);
  return static_cast<Version>(melds.size());
}

bool VersionedFlowSensitivePTA::addReliance(ObjectID object, Version source,
                                            Version destination) {
  if (source == InvalidVersion || destination == InvalidVersion ||
      source == destination)
    return false;
  auto &destinations = versionReliance_[object][source];
  if (std::find(destinations.begin(), destinations.end(), destination) ==
      destinations.end()) {
    destinations.push_back(destination);
    return true;
  }
  return false;
}

void VersionedFlowSensitivePTA::labelObject(ObjectID object) {
  std::unordered_map<NodeID, MeldSet> consumedMeld;
  std::unordered_map<NodeID, MeldSet> yieldedMeld;

  for (const auto &[nodeID, node] : *graph_) {
    if (!inScope(node))
      continue;
    bool hasIncoming = false;
    for (const SVFGEdge *edge : node->getInEdges()) {
      if (edge && inScope(edge->getSrcNode()) &&
          edgeCarriesObject(*edge, object)) {
        hasIncoming = true;
        break;
      }
    }
    // Lotus MemorySSA currently represents a CFG join with a memory PHI whose
    // explicit incoming SVFG edges can all name the materialized store side;
    // the bypass/entry definition is retained in the PHI operands but has no
    // dedicated value-flow node. Seed such PHIs with the initial token so the
    // meld version represents both the bypass and updated paths.
    const auto *memoryPhi = dyn_cast<MSSAPhiSVFGNode>(node);
    if (isDeltaNode(*node, object))
      consumedMeld[nodeID].insert(nodeID + 2);
    else if (!hasIncoming ||
             (memoryPhi && memoryPhiNeedsInitial(*memoryPhi, object)))
      consumedMeld[nodeID].insert(InitialMeldToken);
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[nodeID, node] : *graph_) {
      if (!inScope(node))
        continue;
      bool definesNewVersion = false;
      if (const auto *store = dyn_cast<StoreSVFGNode>(node))
        definesNewVersion = storeMayTarget(*store, object);
      if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(node))
        definesNewVersion |= intrinsicMayDefine(*actualOut, object);
      // Match SVF's storesYieldedMeldVersion: a definition's yielded meld is
      // its fresh prelabel, not consume U prelabel. Weak updates explicitly
      // copy consume into yield during transfer; strong updates do not.
      MeldSet nextYield;
      if (definesNewVersion)
        nextYield.insert(nodeID + 2);
      else
        nextYield = consumedMeld[nodeID];
      if (yieldedMeld[nodeID] != nextYield) {
        yieldedMeld[nodeID] = std::move(nextYield);
        changed = true;
      }
      for (const SVFGEdge *edge : node->getOutEdges()) {
        if (!edge || !inScope(edge->getDstNode()) ||
            !edgeCarriesObject(*edge, object))
          continue;
        if (isDeltaNode(*edge->getDstNode(), object))
          continue;
        MeldSet &destination = consumedMeld[edge->getDstNode()->getId()];
        const std::size_t oldSize = destination.size();
        destination.insert(yieldedMeld[nodeID].begin(),
                           yieldedMeld[nodeID].end());
        changed |= oldSize != destination.size();
      }
    }
  }

  MeldSet initialMeld;
  initialMeld.insert(InitialMeldToken);
  initialVersion_[object] = internVersion(object, initialMeld);
  for (const auto &[nodeID, node] : *graph_) {
    if (!inScope(node))
      continue;
    if (!consumedMeld[nodeID].empty()) {
      const Version consumed = internVersion(object, consumedMeld[nodeID]);
      consume_[nodeID][object] = consumed;
      const auto *memoryPhi = dyn_cast<MSSAPhiSVFGNode>(node);
      if (memoryPhi && memoryPhiNeedsInitial(*memoryPhi, object) &&
          consumedMeld[nodeID].count(InitialMeldToken) != 0)
        addReliance(object, initialVersion_[object], consumed);
    }
    if (!yieldedMeld[nodeID].empty())
      yield_[nodeID][object] = internVersion(object, yieldedMeld[nodeID]);
  }

  for (const auto &[nodeID, node] : *graph_) {
    if (!inScope(node))
      continue;
    for (const SVFGEdge *edge : node->getOutEdges()) {
      if (!edge || !inScope(edge->getDstNode()) ||
          !edgeCarriesObject(*edge, object))
        continue;
      addReliance(object, getYield(nodeID, object),
                  getConsume(edge->getDstNode()->getId(), object));
    }
  }
  for (const auto &[nodeID, node] : *graph_) {
    if (!inScope(node) ||
        !isa<LoadSVFGNode, StoreSVFGNode, ActualOutSVFGNode>(node))
      continue;
    const Version consumed = getConsume(nodeID, object);
    if (consumed == InvalidVersion)
      continue;
    auto &statements = statementReliance_[object][consumed];
    if (std::find(statements.begin(), statements.end(), nodeID) ==
        statements.end())
      statements.push_back(nodeID);
  }
}

std::vector<VersionedFlowSensitivePTA::FootprintEntry>
VersionedFlowSensitivePTA::versionFootprint(ObjectID object) const {
  std::vector<FootprintEntry> footprint;
  for (const auto &[nodeID, node] : *graph_) {
    if (!inScope(node))
      continue;
    if (const auto *store = dyn_cast<StoreSVFGNode>(node))
      if (storeMayTarget(*store, object))
        footprint.push_back({1, nodeID, nodeID});
    if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(node))
      if (intrinsicMayDefine(*actualOut, object))
        footprint.push_back({2, nodeID, nodeID});
    if (isDeltaNode(*node, object))
      footprint.push_back({5, nodeID, nodeID});
    if (const auto *memoryPhi = dyn_cast<MSSAPhiSVFGNode>(node);
        memoryPhi && memoryPhiNeedsInitial(*memoryPhi, object))
      footprint.push_back({3, nodeID, nodeID});
    for (const SVFGEdge *edge : node->getOutEdges())
      if (edge && inScope(edge->getDstNode()) &&
          edgeCarriesObject(*edge, object))
        footprint.push_back({4, nodeID, edge->getDstNode()->getId()});
  }
  std::sort(footprint.begin(), footprint.end());
  return footprint;
}

void VersionedFlowSensitivePTA::copyVersionLabels(ObjectID canonical,
                                                  ObjectID object) {
  std::vector<std::pair<NodeID, Version>> consumed;
  std::vector<std::pair<NodeID, Version>> yielded;
  for (const auto &[location, objects] : consume_) {
    auto it = objects.find(canonical);
    if (it != objects.end())
      consumed.emplace_back(location, it->second);
  }
  for (const auto &[location, objects] : yield_) {
    auto it = objects.find(canonical);
    if (it != objects.end())
      yielded.emplace_back(location, it->second);
  }
  for (const auto &[location, version] : consumed)
    consume_[location][object] = version;
  for (const auto &[location, version] : yielded)
    yield_[location][object] = version;
  initialVersion_[object] = initialVersion_[canonical];
  versionMelds_[object] = versionMelds_[canonical];
  versionReliance_[object] = versionReliance_[canonical];
  statementReliance_[object] = statementReliance_[canonical];
}

void VersionedFlowSensitivePTA::buildVersionProcessingOrder() {
  versionProcessingOrder_.clear();
  versionProcessingOrder_.reserve(versionMelds_.size());
  for (const auto &[object, melds] : versionMelds_)
    versionProcessingOrder_.push_back(object);
  auto occurrenceWeight = [&](ObjectID object) {
    std::size_t weight = 1;
    auto objectIt = versionReliance_.find(object);
    if (objectIt != versionReliance_.end())
      for (const auto &[source, destinations] : objectIt->second)
        weight += 1 + destinations.size();
    for (const auto &[location, objects] : consume_)
      weight += objects.count(object);
    return weight;
  };
  if (config_.clusterVersionedObjects)
    std::stable_sort(versionProcessingOrder_.begin(),
                     versionProcessingOrder_.end(),
                     [&](ObjectID lhs, ObjectID rhs) {
                       const std::size_t leftWeight = occurrenceWeight(lhs);
                       const std::size_t rightWeight = occurrenceWeight(rhs);
                       return leftWeight != rightWeight
                                  ? leftWeight > rightWeight
                                  : lhs < rhs;
                     });
  else
    std::sort(versionProcessingOrder_.begin(), versionProcessingOrder_.end());
}

void VersionedFlowSensitivePTA::buildVersionLabels() {
  consume_.clear();
  yield_.clear();
  versionReliance_.clear();
  statementReliance_.clear();
  initialVersion_.clear();
  versionMelds_.clear();
  equivalentObject_.clear();
  const PointsToSet objects = relevantObjects();
  std::map<std::vector<FootprintEntry>, ObjectID> footprintOwners;
  for (ObjectID object : objects) {
    std::vector<FootprintEntry> footprint = versionFootprint(object);
    auto owner = footprintOwners.find(footprint);
    if (owner == footprintOwners.end()) {
      equivalentObject_[object] = object;
      footprintOwners.emplace(std::move(footprint), object);
      labelObject(object);
    } else {
      equivalentObject_[object] = owner->second;
      copyVersionLabels(owner->second, object);
      ++stats_.equivalentObjects;
    }
  }
  stats_.versionedObjects = objects.size();
  stats_.versions = 0;
  for (const auto &[object, melds] : versionMelds_)
    stats_.versions += melds.size();
  buildVersionProcessingOrder();
  ++stats_.relabelings;
}

VersionedFlowSensitivePTA::Version
VersionedFlowSensitivePTA::getConsume(NodeID location, ObjectID object) const {
  auto locationIt = consume_.find(location);
  if (locationIt == consume_.end())
    return InvalidVersion;
  auto objectIt = locationIt->second.find(object);
  return objectIt == locationIt->second.end() ? InvalidVersion
                                              : objectIt->second;
}

VersionedFlowSensitivePTA::Version
VersionedFlowSensitivePTA::getYield(NodeID location, ObjectID object) const {
  auto locationIt = yield_.find(location);
  if (locationIt == yield_.end())
    return InvalidVersion;
  auto objectIt = locationIt->second.find(object);
  return objectIt == locationIt->second.end() ? InvalidVersion
                                              : objectIt->second;
}

VersionedFlowSensitivePTA::Version
VersionedFlowSensitivePTA::initialVersion(ObjectID object) const {
  auto it = initialVersion_.find(object);
  return it == initialVersion_.end() ? InvalidVersion : it->second;
}

const VersionedFlowSensitivePTA::PointsToSet &
VersionedFlowSensitivePTA::versionedPointsTo(ObjectID object,
                                             Version version) const {
  static const PointsToSet empty;
  auto it = versionedPointsTo_.find({object, version});
  return it == versionedPointsTo_.end() ? empty : it->second;
}

const std::vector<VersionedFlowSensitivePTA::Version> &
VersionedFlowSensitivePTA::getReliantVersions(ObjectID object,
                                              Version version) const {
  static const std::vector<Version> empty;
  auto objectIt = versionReliance_.find(object);
  if (objectIt == versionReliance_.end())
    return empty;
  auto versionIt = objectIt->second.find(version);
  return versionIt == objectIt->second.end() ? empty : versionIt->second;
}

VersionedFlowSensitivePTA::ObjectID
VersionedFlowSensitivePTA::canonicalVersionObject(ObjectID object) const {
  auto it = equivalentObject_.find(object);
  return it == equivalentObject_.end() ? object : it->second;
}

const std::vector<VersionedFlowSensitivePTA::NodeID> &
VersionedFlowSensitivePTA::getDependentStatements(ObjectID object,
                                                  Version version) const {
  static const std::vector<NodeID> empty;
  auto objectIt = statementReliance_.find(object);
  if (objectIt == statementReliance_.end())
    return empty;
  auto versionIt = objectIt->second.find(version);
  return versionIt == objectIt->second.end() ? empty : versionIt->second;
}

bool VersionedFlowSensitivePTA::writeVersionedAnalysisResultToFile(
    const std::string &filename) const {
  std::ofstream output(filename, std::ios::trunc);
  if (!output)
    return false;
  output << "LOTUS_VFSPTA 1\n";
  for (const auto &[location, objects] : consume_)
    for (const auto &[object, version] : objects)
      output << "C " << location << ' ' << object << ' ' << version << '\n';
  for (const auto &[location, objects] : yield_)
    for (const auto &[object, version] : objects)
      output << "Y " << location << ' ' << object << ' ' << version << '\n';
  for (const auto &[object, version] : initialVersion_)
    output << "I " << object << ' ' << version << '\n';
  for (const auto &[object, canonical] : equivalentObject_)
    output << "Q " << object << ' ' << canonical << '\n';
  for (const auto &[object, melds] : versionMelds_)
    for (std::size_t index = 0; index < melds.size(); ++index) {
      output << "M " << object << ' ' << index + 1 << ' '
             << melds[index].size();
      for (NodeID token : melds[index])
        output << ' ' << token;
      output << '\n';
    }
  for (const auto &[key, pointsToSet] : versionedPointsTo_) {
    output << "P " << key.object << ' ' << key.version << ' '
           << pointsToSet.size();
    for (ObjectID target : pointsToSet)
      output << ' ' << target;
    output << '\n';
  }
  for (const auto &[node, pointsToSet] : topLevelPointsTo_) {
    output << "T " << node << ' ' << pointsToSet.size();
    for (ObjectID target : pointsToSet)
      output << ' ' << target;
    output << '\n';
  }
  for (const auto &[object, sources] : versionReliance_)
    for (const auto &[source, destinations] : sources) {
      output << "R " << object << ' ' << source << ' ' << destinations.size();
      for (Version destination : destinations)
        output << ' ' << destination;
      output << '\n';
    }
  for (const auto &[object, versions] : statementReliance_)
    for (const auto &[version, statements] : versions) {
      output << "D " << object << ' ' << version << ' ' << statements.size();
      for (NodeID statement : statements)
        output << ' ' << statement;
      output << '\n';
    }
  output << "E\n";
  return output.good();
}

bool VersionedFlowSensitivePTA::readVersionedAnalysisResultFromFile(
    const std::string &filename) {
  std::ifstream input(filename);
  std::string magic;
  unsigned formatVersion = 0;
  if (!(input >> magic >> formatVersion) || magic != "LOTUS_VFSPTA" ||
      formatVersion != 1)
    return false;

  topLevelPointsTo_.clear();
  versionedPointsTo_.clear();
  consume_.clear();
  yield_.clear();
  versionReliance_.clear();
  statementReliance_.clear();
  initialVersion_.clear();
  versionMelds_.clear();
  equivalentObject_.clear();
  stats_ = {};

  auto readSet = [&](std::size_t count, PointsToSet &set) {
    for (std::size_t i = 0; i < count; ++i) {
      ObjectID value = 0;
      if (!(input >> value))
        return false;
      set.insert(value);
    }
    return true;
  };
  std::string record;
  while (input >> record) {
    if (record == "E")
      break;
    if (record == "C" || record == "Y") {
      NodeID location = 0;
      ObjectID object = 0;
      Version version = InvalidVersion;
      if (!(input >> location >> object >> version))
        return false;
      (record == "C" ? consume_ : yield_)[location][object] = version;
    } else if (record == "I") {
      ObjectID object = 0;
      Version version = InvalidVersion;
      if (!(input >> object >> version))
        return false;
      initialVersion_[object] = version;
    } else if (record == "Q") {
      ObjectID object = 0;
      ObjectID canonical = 0;
      if (!(input >> object >> canonical))
        return false;
      equivalentObject_[object] = canonical;
    } else if (record == "M") {
      ObjectID object = 0;
      Version version = InvalidVersion;
      std::size_t count = 0;
      if (!(input >> object >> version >> count) || version == InvalidVersion)
        return false;
      auto &melds = versionMelds_[object];
      if (melds.size() < version)
        melds.resize(version);
      if (!readSet(count, melds[version - 1]))
        return false;
    } else if (record == "P") {
      ObjectID object = 0;
      Version version = InvalidVersion;
      std::size_t count = 0;
      if (!(input >> object >> version >> count) ||
          !readSet(count, versionedPointsTo_[{object, version}]))
        return false;
    } else if (record == "T") {
      NodeID node = 0;
      std::size_t count = 0;
      if (!(input >> node >> count) || !readSet(count, topLevelPointsTo_[node]))
        return false;
    } else if (record == "R") {
      ObjectID object = 0;
      Version source = InvalidVersion;
      std::size_t count = 0;
      if (!(input >> object >> source >> count))
        return false;
      auto &destinations = versionReliance_[object][source];
      for (std::size_t i = 0; i < count; ++i) {
        Version destination = InvalidVersion;
        if (!(input >> destination))
          return false;
        destinations.push_back(destination);
      }
    } else if (record == "D") {
      ObjectID object = 0;
      Version version = InvalidVersion;
      std::size_t count = 0;
      if (!(input >> object >> version >> count))
        return false;
      auto &statements = statementReliance_[object][version];
      for (std::size_t i = 0; i < count; ++i) {
        NodeID statement = 0;
        if (!(input >> statement))
          return false;
        statements.push_back(statement);
      }
    } else {
      return false;
    }
  }
  if (record != "E")
    return false;

  for (const auto &[nodeID, node] : *graph_)
    if (inScope(node))
      ++stats_.nodes;
  stats_.versionedObjects = versionMelds_.size();
  for (const auto &[object, canonical] : equivalentObject_)
    stats_.equivalentObjects += object != canonical;
  for (const auto &[object, melds] : versionMelds_)
    stats_.versions += melds.size();
  for (const auto &[key, pointsToSet] : versionedPointsTo_)
    stats_.versionedFacts += pointsToSet.size();
  for (const auto &[object, versions] : statementReliance_)
    for (const auto &[version, statements] : versions)
      stats_.statementReliances += statements.size();
  buildVersionProcessingOrder();
  return true;
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::constantPointsTo(const Constant *constant) const {
  PointsToSet result;
  if (!constant || isa<ConstantPointerNull>(constant) ||
      !constant->getType()->isPointerTy())
    return result;
  const PointsToSet &known = graph_->getObjectIds(constant);
  result.insert(known.begin(), known.end());
  if (!result.empty())
    return result;
  if (const auto *expression = dyn_cast<ConstantExpr>(constant))
    if (expression->isCast() ||
        expression->getOpcode() == Instruction::GetElementPtr)
      return constantPointsTo(dyn_cast<Constant>(expression->getOperand(0)));
  if (ObjectID object = graph_->getObjectId(constant->stripPointerCasts()))
    result.insert(object);
  return result;
}

void VersionedFlowSensitivePTA::initializeGlobalMemory() {
  const Module *module = nullptr;
  for (const auto &[nodeID, node] : *graph_)
    if (const Function *function = node->getFunction()) {
      module = function->getParent();
      break;
    }
  if (!module)
    return;

  for (const GlobalVariable &global : module->globals()) {
    if (!global.hasInitializer())
      continue;
    PointsToSet storageBases;
    if (ObjectID object = graph_->getObjectId(&global))
      storageBases.insert(object);
    if (storageBases.empty()) {
      const PointsToSet &known = graph_->getObjectIds(&global);
      storageBases.insert(known.begin(), known.end());
    }
    auto storageAtOffset = [&](uint64_t offset) {
      PointsToSet storage;
      for (ObjectID base : storageBases) {
        const SVFG::ObjectInfo *info = graph_->getObjectInfo(base);
        const ObjectID canonical =
            info && info->baseObjId != 0 ? info->baseObjId : base;
        const ObjectID field = offset == 0
                                   ? canonical
                                   : graph_->getOffsetObject(canonical, offset);
        if (field != 0)
          storage.insert(field);
      }
      return storage;
    };

    const DataLayout &layout = module->getDataLayout();
    std::function<void(const Constant *, Type *, uint64_t)> seed =
        [&](const Constant *constant, Type *type, uint64_t offset) {
          if (!constant || !type)
            return;
          if (type->isPointerTy()) {
            const PointsToSet pointsTo = constantPointsTo(constant);
            for (ObjectID storage : storageAtOffset(offset)) {
              const Version version = initialVersion(storage);
              if (version != InvalidVersion)
                versionedPointsTo_[{storage, version}].insert(pointsTo.begin(),
                                                              pointsTo.end());
            }
            return;
          }
          if (auto *structure = dyn_cast<StructType>(type)) {
            const StructLayout *structureLayout =
                layout.getStructLayout(structure);
            for (unsigned field = 0; field < structure->getNumElements();
                 ++field)
              seed(constant->getAggregateElement(field),
                   structure->getElementType(field),
                   offset + structureLayout->getElementOffset(field));
            return;
          }
          if (auto *array = dyn_cast<ArrayType>(type)) {
            const uint64_t stride =
                layout.getTypeAllocSize(array->getElementType());
            for (uint64_t element = 0; element < array->getNumElements();
                 ++element)
              seed(constant->getAggregateElement(element),
                   array->getElementType(), offset + element * stride);
          }
        };
    seed(global.getInitializer(), global.getValueType(), 0);
  }
}

const Value *
VersionedFlowSensitivePTA::accessPointer(const Instruction *instruction) {
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

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::pointerTargets(const Value *pointer) const {
  if (!pointer || !pointer->getType()->isPointerTy())
    return {};
  const SVFGNode *node = graph_->getValueNode(pointer);
  return node ? pointsTo(node) : PointsToSet{};
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::selectAccessTargets(
    const PointsToSet &flowSensitiveTargets,
    const PointsToSet &preAnalysisTargets) const {
  if (preAnalysisTargets.empty())
    return flowSensitiveTargets;
  if (flowSensitiveTargets.empty())
    return preAnalysisTargets;
  const bool preUnknown = std::any_of(
      preAnalysisTargets.begin(), preAnalysisTargets.end(),
      [&](ObjectID object) { return graph_->isUnknownObject(object); });
  if (preUnknown)
    return flowSensitiveTargets;
  const bool flowUnknown = std::any_of(
      flowSensitiveTargets.begin(), flowSensitiveTargets.end(),
      [&](ObjectID object) { return graph_->isUnknownObject(object); });
  return flowUnknown ? preAnalysisTargets : flowSensitiveTargets;
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::directInput(const SVFGNode &node) const {
  PointsToSet result;
  for (const SVFGEdge *edge : node.getInEdges()) {
    if (!edge || !inScope(edge->getSrcNode()) ||
        !isDirectVFGEdge(edge->getEdgeKind()))
      continue;
    const PointsToSet &source = pointsTo(edge->getSrcNode());
    result.insert(source.begin(), source.end());
  }
  return result;
}

bool VersionedFlowSensitivePTA::isStrongUpdate(
    const PointsToSet &targets) const {
  if (targets.size() != 1)
    return false;
  const ObjectID object = *targets.begin();
  const SVFG::ObjectInfo *info = graph_->getObjectInfo(object);
  if (!info || info->isUnknown || info->isHeap || info->isArray ||
      info->isFieldInsensitive)
    return false;
  const Value *allocation = graph_->getObjectValue(object);
  if (!allocation && info->baseObjId != 0)
    allocation = graph_->getObjectValue(info->baseObjId);
  if (const auto *instruction = dyn_cast_or_null<Instruction>(allocation))
    if (recursiveFunctions_.count(instruction->getFunction()) != 0)
      return false;
  return true;
}

void VersionedFlowSensitivePTA::updateConnectedNodes(
    const std::vector<const SVFGEdge *> &newEdges) {
  for (const SVFGEdge *edge : newEdges) {
    if (!edge || !isIndirectVFGEdge(edge->getEdgeKind()))
      continue;
    PointsToSet objects = expandIndirectObjects(edge->getPointsTo());
    const bool wildcard =
        std::any_of(objects.begin(), objects.end(), [&](ObjectID object) {
          return graph_->isUnknownObject(object);
        });
    if (wildcard)
      objects = relevantObjects();
    for (ObjectID object : objects) {
      if (isDeltaNode(*edge->getDstNode(), object)) {
        const Version deltaVersion =
            getConsume(edge->getDstNode()->getId(), object);
        const auto melds = versionMelds_.find(object);
        const bool hasDedicatedVersion =
            deltaVersion != InvalidVersion && melds != versionMelds_.end() &&
            deltaVersion <= melds->second.size() &&
            melds->second[deltaVersion - 1] ==
                MeldSet{edge->getDstNode()->getId() + 2};
        if (!hasDedicatedVersion) {
          topologyChanged_ = true;
          continue;
        }
      }
      const Version source = getYield(edge->getSrcNode()->getId(), object);
      const Version destination =
          getConsume(edge->getDstNode()->getId(), object);
      if (source == InvalidVersion || destination == InvalidVersion) {
        // The auxiliary pre-analysis did not prelabel this object at one end
        // of the new edge. Rebuilding labels is the conservative fallback.
        topologyChanged_ = true;
        continue;
      }
      if (addReliance(object, source, destination))
        ++stats_.deltaVersionUpdates;
    }
  }
  buildVersionProcessingOrder();
}

bool VersionedFlowSensitivePTA::resolveIndirectCalls(
    const SVFGNode &node, const PointsToSet &pointsToSet) {
  if (!config_.connectIndirectCall)
    return false;
  bool changed = false;
  const NodeID functionPointer =
      node.hasValueId() ? node.getValueId() : node.getId();
  for (const CallBase *callSite : graph_->getIndCallSites(functionPointer))
    for (ObjectID object : pointsToSet) {
      const auto *target =
          dyn_cast_or_null<Function>(graph_->getObjectValue(object));
      if (!target || target->isDeclaration())
        continue;
      std::unordered_set<const SVFGEdge *> oldEdges;
      for (const auto &[nodeID, graphNode] : *graph_)
        for (const SVFGEdge *edge : graphNode->getOutEdges())
          if (edge)
            oldEdges.insert(edge);
      const auto oldRecursiveFunctions = recursiveFunctions_;
      if (config_.connectIndirectCall(callSite, target)) {
        ++stats_.indirectCallEdges;
        changed = true;
        std::vector<const SVFGEdge *> newEdges;
        for (const auto &[nodeID, graphNode] : *graph_)
          for (const SVFGEdge *edge : graphNode->getOutEdges())
            if (edge && oldEdges.count(edge) == 0)
              newEdges.push_back(edge);
        updateConnectedNodes(newEdges);
        initializeRecursiveFunctions();
        if (recursiveFunctions_ != oldRecursiveFunctions)
          topologyChanged_ = true;
      }
    }
  return changed;
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::gepTransfer(const GepSVFGNode &gep) const {
  const auto *instruction = dyn_cast_or_null<GetElementPtrInst>(gep.getValue());
  PointsToSet bases = directInput(gep);
  if (!instruction || instruction->getSourceElementType()->isArrayTy())
    return bases;
  PointsToSet result;
  for (ObjectID base : bases) {
    if (graph_->isFieldInsensitiveObject(base)) {
      result.insert(base);
      continue;
    }
    const SVFG::GepAccessInfo access = graph_->getGepAccess(instruction);
    if (access.valid) {
      ObjectID canonical = base;
      uint64_t baseOffset = 0;
      if (const SVFG::ObjectInfo *info = graph_->getObjectInfo(base)) {
        if (info->baseObjId != 0)
          canonical = info->baseObjId;
        if (info->hasFieldOffset)
          baseOffset = info->fieldOffset;
      }
      if (access.relativeOffset <=
          std::numeric_limits<uint64_t>::max() - baseOffset) {
        const uint64_t totalOffset = baseOffset + access.relativeOffset;
        const ObjectID field =
            totalOffset == 0 ? canonical
                             : graph_->getOffsetObject(canonical, totalOffset);
        if (field != 0) {
          result.insert(field);
          continue;
        }
      }
    }
    if (ObjectID mapped = graph_->getGepObject(instruction, base))
      result.insert(mapped);
  }
  if (result.empty()) {
    const PointsToSet &known = graph_->getObjectIds(gep.getValue());
    result.insert(known.begin(), known.end());
  }
  return result;
}

bool VersionedFlowSensitivePTA::propagateVersions() {
  bool changed = false;
  for (ObjectID object : versionProcessingOrder_) {
    auto objectIt = versionReliance_.find(object);
    if (objectIt == versionReliance_.end())
      continue;
    for (const auto &[sourceVersion, destinations] : objectIt->second) {
      const PointsToSet &source = versionedPointsTo(object, sourceVersion);
      for (Version destinationVersion : destinations) {
        PointsToSet &destination =
            versionedPointsTo_[{object, destinationVersion}];
        const std::size_t oldSize = destination.size();
        destination.insert(source.begin(), source.end());
        if (oldSize != destination.size()) {
          changed = true;
          ++stats_.versionPropagations;
        }
      }
    }
  }
  return changed;
}

bool VersionedFlowSensitivePTA::processIntrinsicActualOut(
    const ActualOutSVFGNode &actualOut) {
  const auto *intrinsic =
      dyn_cast_or_null<IntrinsicInst>(actualOut.getCallSite());
  if (!intrinsic || intrinsic->arg_size() < 2)
    return false;
  const Intrinsic::ID intrinsicId = intrinsic->getIntrinsicID();
  if (intrinsicId != Intrinsic::memcpy && intrinsicId != Intrinsic::memmove &&
      intrinsicId != Intrinsic::memset)
    return false;

  struct Location {
    ObjectID object = 0;
    ObjectID base = 0;
    uint64_t offset = 0;
  };
  auto locations = [&](const Value *pointer) {
    std::vector<Location> result;
    for (ObjectID object : objectsWithFields(pointer)) {
      const SVFG::ObjectInfo *info = graph_->getObjectInfo(object);
      result.push_back({object,
                        info && info->baseObjId != 0 ? info->baseObjId : object,
                        info && info->hasFieldOffset ? info->fieldOffset : 0});
    }
    return result;
  };
  auto roots = [&](const Value *pointer) {
    PointsToSet objects = pointerTargets(pointer);
    if (objects.empty()) {
      const PointsToSet &known = graph_->getObjectIds(pointer);
      objects.insert(known.begin(), known.end());
    }
    std::vector<Location> result;
    for (ObjectID object : objects) {
      const SVFG::ObjectInfo *info = graph_->getObjectInfo(object);
      result.push_back({object,
                        info && info->baseObjId != 0 ? info->baseObjId : object,
                        info && info->hasFieldOffset ? info->fieldOffset : 0});
    }
    return result;
  };
  auto memoryBeforeCall = [&](ObjectID object) {
    PointsToSet result;
    bool foundVersion = false;
    for (const SVFGNode *actualIn :
         graph_->getActualIns(actualOut.getCallSite())) {
      Version version = getConsume(actualIn->getId(), object);
      if (version == InvalidVersion)
        version = getYield(actualIn->getId(), object);
      if (version == InvalidVersion)
        continue;
      foundVersion = true;
      const PointsToSet &points = versionedPointsTo(object, version);
      result.insert(points.begin(), points.end());
    }
    for (const SVFGEdge *edge : actualOut.getInEdges()) {
      const SVFGNode *source = edge ? edge->getSrcNode() : nullptr;
      if (!source)
        continue;
      Version version = getYield(source->getId(), object);
      if (version == InvalidVersion)
        continue;
      foundVersion = true;
      const PointsToSet &points = versionedPointsTo(object, version);
      result.insert(points.begin(), points.end());
    }
    if (!foundVersion) {
      Version version = getConsume(actualOut.getId(), object);
      if (version == InvalidVersion)
        version = initialVersion(object);
      const PointsToSet &points = versionedPointsTo(object, version);
      result.insert(points.begin(), points.end());
    }
    return result;
  };

  const Value *destinationPointer = intrinsic->getArgOperand(0);
  const std::vector<Location> destinationLocations =
      locations(destinationPointer);
  const std::vector<Location> destinationRoots = roots(destinationPointer);
  const auto *length = intrinsic->arg_size() >= 3
                           ? dyn_cast<ConstantInt>(intrinsic->getArgOperand(2))
                           : nullptr;
  const bool exactLength = length != nullptr;
  const uint64_t copiedBytes = length ? length->getZExtValue() : 0;
  auto pointerSizeForObject = [&](ObjectID object) {
    for (const auto &entry : graph_->getValueNodeMap()) {
      const auto *gep = dyn_cast_or_null<GetElementPtrInst>(entry.first);
      if (!gep || graph_->getObjectIds(gep).count(object) == 0)
        continue;
      if (const auto *pointerType =
              dyn_cast<PointerType>(gep->getResultElementType()))
        return static_cast<uint64_t>(
            intrinsic->getModule()->getDataLayout().getPointerSize(
                pointerType->getAddressSpace()));
    }
    return static_cast<uint64_t>(
        intrinsic->getModule()->getDataLayout().getPointerSize());
  };
  auto fullyCovered = [&](ObjectID object, uint64_t relativeOffset) {
    const uint64_t pointerSize = pointerSizeForObject(object);
    return !exactLength || (relativeOffset <= copiedBytes &&
                            pointerSize <= copiedBytes - relativeOffset);
  };

  std::vector<Location> sourceLocations;
  std::vector<Location> sourceRoots;
  if (intrinsicId == Intrinsic::memcpy || intrinsicId == Intrinsic::memmove) {
    sourceLocations = locations(intrinsic->getArgOperand(1));
    sourceRoots = roots(intrinsic->getArgOperand(1));
  }

  ObjectID unknownObject = 0;
  for (const auto &[object, label] : graph_->getObjectDebugMap())
    if (graph_->isUnknownObject(object)) {
      unknownObject = object;
      break;
    }

  bool changed = false;
  for (const Location &destination : destinationLocations) {
    uint64_t relativeOffset = std::numeric_limits<uint64_t>::max();
    for (const Location &root : destinationRoots)
      if (root.base == destination.base && destination.offset >= root.offset)
        relativeOffset =
            std::min(relativeOffset, destination.offset - root.offset);

    PointsToSet replacement;
    const bool outsideWrite =
        relativeOffset == std::numeric_limits<uint64_t>::max() ||
        (exactLength && relativeOffset >= copiedBytes);
    if (outsideWrite) {
      replacement = memoryBeforeCall(destination.object);
    } else if (intrinsicId == Intrinsic::memcpy ||
               intrinsicId == Intrinsic::memmove) {
      bool matchedSource = false;
      for (const Location &sourceRoot : sourceRoots) {
        const uint64_t sourceOffset = sourceRoot.offset + relativeOffset;
        if (sourceOffset < sourceRoot.offset)
          continue;
        for (const Location &source : sourceLocations)
          if (source.base == sourceRoot.base && source.offset == sourceOffset) {
            matchedSource = true;
            const PointsToSet sourceMemory = memoryBeforeCall(source.object);
            replacement.insert(sourceMemory.begin(), sourceMemory.end());
          }
      }
      if (!matchedSource && unknownObject != 0)
        replacement.insert(unknownObject);
    } else {
      const auto *fill = dyn_cast<ConstantInt>(intrinsic->getArgOperand(1));
      if ((!fill || !fill->isZero()) && unknownObject != 0)
        replacement.insert(unknownObject);
    }

    if (!outsideWrite && !fullyCovered(destination.object, relativeOffset)) {
      if (unknownObject != 0) {
        replacement.clear();
        replacement.insert(unknownObject);
      }
    } else if (!outsideWrite && !exactLength) {
      const PointsToSet oldMemory = memoryBeforeCall(destination.object);
      replacement.insert(oldMemory.begin(), oldMemory.end());
    }

    const Version yielded = getYield(actualOut.getId(), destination.object);
    if (yielded == InvalidVersion)
      continue;
    PointsToSet &output = versionedPointsTo_[{destination.object, yielded}];
    const std::size_t oldSize = output.size();
    output.insert(replacement.begin(), replacement.end());
    if (oldSize != output.size()) {
      changed = true;
    }
  }
  return changed;
}

bool VersionedFlowSensitivePTA::processNode(const SVFGNode &node) {
  ++stats_.nodeProcesses;
  PointsToSet next = pointsTo(&node);
  if (const auto *address = dyn_cast<AddrSVFGNode>(&node)) {
    ObjectID object = address->getObjectId();
    if (object == 0 && address->getValue())
      object = graph_->getObjectId(address->getValue());
    next.clear();
    if (object != 0)
      next.insert(object);
  } else if (const auto *load = dyn_cast<LoadSVFGNode>(&node)) {
    next.clear();
    PointsToSet targets = pointerTargets(accessPointer(load->getInstruction()));
    targets = selectAccessTargets(targets, load->getMemoryPointsTo());
    targets = expandIndirectObjects(targets);
    for (ObjectID object : targets) {
      if (graph_->isConstantObject(object))
        continue;
      Version consumed = getConsume(node.getId(), object);
      if (consumed == InvalidVersion)
        consumed = initialVersion(object);
      const PointsToSet &memory = versionedPointsTo(object, consumed);
      next.insert(memory.begin(), memory.end());
    }
  } else if (const auto *store = dyn_cast<StoreSVFGNode>(&node)) {
    const auto *instruction =
        dyn_cast_or_null<StoreInst>(store->getInstruction());
    const PointsToSet stored =
        instruction ? pointerTargets(instruction->getValueOperand())
                    : directInput(node);
    PointsToSet targets = instruction
                              ? pointerTargets(instruction->getPointerOperand())
                              : PointsToSet{};
    targets = selectAccessTargets(targets, store->getMemoryPointsTo());
    targets = expandIndirectObjects(targets);
    const bool strong = isStrongUpdate(targets);
    bool updated = false;
    for (ObjectID object : targets) {
      if (graph_->isConstantObject(object))
        continue;
      const Version yielded = getYield(node.getId(), object);
      if (yielded == InvalidVersion)
        continue;
      PointsToSet replacement = stored;
      if (!strong) {
        Version consumed = getConsume(node.getId(), object);
        if (consumed == InvalidVersion)
          consumed = initialVersion(object);
        const PointsToSet &oldMemory = versionedPointsTo(object, consumed);
        replacement.insert(oldMemory.begin(), oldMemory.end());
      }
      PointsToSet &destination = versionedPointsTo_[{object, yielded}];
      const std::size_t oldSize = destination.size();
      destination.insert(replacement.begin(), replacement.end());
      if (oldSize != destination.size()) {
        updated = true;
      }
    }
    if (!targets.empty()) {
      if (strong)
        strongUpdateSites_.insert(node.getId());
      else
        weakUpdateSites_.insert(node.getId());
    }
    if (updated)
      return true;
  } else if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(&node)) {
    if (processIntrinsicActualOut(*actualOut))
      return true;
  } else if (const auto *gep = dyn_cast<GepSVFGNode>(&node)) {
    next = gepTransfer(*gep);
  } else if (!isa<MSSASVFGNode>(&node)) {
    next = directInput(node);
  }

  PointsToSet &current = topLevelPointsTo_[node.getId()];
  if (current == next)
    return false;
  current = std::move(next);
  resolveIndirectCalls(node, current);
  return true;
}

bool VersionedFlowSensitivePTA::solveCurrentTopology() {
  bool anyChanged = false;
  bool changed = true;
  while (changed && !topologyChanged_) {
    changed = propagateVersions();
    for (const auto &[nodeID, node] : *graph_)
      if (inScope(node))
        changed |= processNode(*node);
    anyChanged |= changed;
  }
  return anyChanged;
}

const VersionedFlowSensitivePTA::Statistics &
VersionedFlowSensitivePTA::solve() {
  stats_ = {};
  do {
    topologyChanged_ = false;
    initializeRecursiveFunctions();
    topLevelPointsTo_.clear();
    versionedPointsTo_.clear();
    strongUpdateSites_.clear();
    weakUpdateSites_.clear();
    buildVersionLabels();
    initializeGlobalMemory();
    solveCurrentTopology();
  } while (topologyChanged_);

  for (const auto &[nodeID, node] : *graph_)
    if (inScope(node))
      ++stats_.nodes;
  stats_.versionedFacts = 0;
  for (const auto &[versionedObject, pointsToSet] : versionedPointsTo_)
    stats_.versionedFacts += pointsToSet.size();
  stats_.statementReliances = 0;
  for (const auto &[object, versions] : statementReliance_)
    for (const auto &[version, statements] : versions)
      stats_.statementReliances += statements.size();
  stats_.strongUpdates = strongUpdateSites_.size();
  stats_.weakUpdates = weakUpdateSites_.size();
  return stats_;
}

const VersionedFlowSensitivePTA::PointsToSet &
VersionedFlowSensitivePTA::pointsTo(const SVFGNode *node) const {
  static const PointsToSet empty;
  if (!node)
    return empty;
  auto it = topLevelPointsTo_.find(node->getId());
  return it == topLevelPointsTo_.end() ? empty : it->second;
}

std::optional<VersionedFlowSensitivePTA::PointsToSet>
VersionedFlowSensitivePTA::pointsTo(const Value *value) const {
  if (!value || !value->getType()->isPointerTy())
    return std::nullopt;
  const SVFGNode *node = graph_->getValueNode(value);
  if (!inScope(node))
    return std::nullopt;
  return pointsTo(node);
}

std::optional<bool>
VersionedFlowSensitivePTA::mayAlias(const Value *lhs, const Value *rhs) const {
  auto left = pointsTo(lhs);
  auto right = pointsTo(rhs);
  if (!left || !right)
    return std::nullopt;
  const PointsToSet *small = &*left;
  const PointsToSet *large = &*right;
  if (large->size() < small->size())
    std::swap(small, large);
  for (ObjectID object : *small)
    if (large->count(object) != 0)
      return true;
  return false;
}

} // namespace lotus::alias
