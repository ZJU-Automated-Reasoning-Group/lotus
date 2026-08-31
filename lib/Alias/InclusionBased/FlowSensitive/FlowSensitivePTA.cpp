#include "Alias/InclusionBased/FlowSensitive/FlowSensitivePTA.h"

#include "IR/ICFG/CallGraph.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <unordered_set>

#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
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

const FlowSensitivePTA::MemoryState &
FlowSensitivePTA::inState(const SVFGNode *node) const {
  static const MemoryState empty;
  if (!node)
    return empty;
  auto it = dfIn_.find(node->getId());
  return it == dfIn_.end() ? empty : it->second;
}

FlowSensitivePTA::PointsToSet
FlowSensitivePTA::expandIndirectObjects(const PointsToSet &objects) const {
  PointsToSet expanded = objects;
  for (ObjectID object : objects) {
    if (!graph_->isFieldInsensitiveObject(object))
      continue;
    const PointsToSet fields = graph_->getFieldObjects(object);
    expanded.insert(fields.begin(), fields.end());
  }
  return expanded;
}

void FlowSensitivePTA::initializeRecursiveFunctions() {
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
            const LTCallGraphNode *calleeNode = edge.second;
            const Function *callee =
                calleeNode ? calleeNode->getFunction() : nullptr;
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
    for (const auto &entry : *callGraph) {
      const Function *function = entry.first;
      if (function && !function->isDeclaration() &&
          index.find(function) == index.end())
        visit(function);
    }
    return;
  }

  Module *module = nullptr;
  for (const auto &entry : *graph_) {
    if (const Function *function = entry.second->getFunction()) {
      module = const_cast<Module *>(function->getParent());
      break;
    }
  }
  if (!module)
    return;
  CallGraph callGraph(*module);
  for (scc_iterator<CallGraph *> it = scc_begin(&callGraph),
                                 end = scc_end(&callGraph);
       it != end; ++it) {
    const std::vector<CallGraphNode *> &component = *it;
    bool recursive = component.size() > 1;
    if (!recursive && component.size() == 1) {
      CallGraphNode *node = component.front();
      for (const auto &edge : *node)
        recursive |= edge.second == node;
    }
    if (!recursive)
      continue;
    for (CallGraphNode *node : component)
      if (node && node->getFunction())
        recursiveFunctions_.insert(node->getFunction());
  }
}

FlowSensitivePTA::StoredSet
FlowSensitivePTA::constantPointsTo(const Constant *constant) {
  StoredSet result;
  if (!constant || isa<ConstantPointerNull>(constant) ||
      !constant->getType()->isPointerTy())
    return result;
  if (const auto *expression = dyn_cast<ConstantExpr>(constant)) {
    if (expression->getOpcode() == Instruction::GetElementPtr) {
      const auto *gep = cast<GEPOperator>(expression);
      const Value *underlying = gep->getPointerOperand()->stripPointerCasts();
      const Module *module = nullptr;
      if (const auto *global = dyn_cast<GlobalValue>(underlying))
        module = global->getParent();
      else if (const auto *instruction = dyn_cast<Instruction>(underlying))
        module = instruction->getModule();
      if (module) {
        APInt offset(module->getDataLayout().getIndexTypeSizeInBits(
                         gep->getPointerOperandType()),
                     0);
        if (gep->accumulateConstantOffset(module->getDataLayout(), offset)) {
          StoredSet bases = constantPointsTo(
              dyn_cast<Constant>(gep->getPointerOperand()));
          StoredSet exact;
          for (ObjectID base : materialize(bases)) {
            ObjectID canonicalBase = base;
            uint64_t baseOffset = 0;
            if (const SVFG::ObjectInfo *info = graph_->getObjectInfo(base)) {
              if (info->baseObjId != 0)
                canonicalBase = info->baseObjId;
              if (info->hasFieldOffset)
                baseOffset = info->fieldOffset;
            }
            const uint64_t gepOffset = offset.getZExtValue();
            if (gepOffset >
                std::numeric_limits<uint64_t>::max() - baseOffset)
              continue;
            const uint64_t totalOffset = baseOffset + gepOffset;
            const ObjectID field =
                totalOffset == 0
                    ? canonicalBase
                    : graph_->getOffsetObject(canonicalBase, totalOffset);
            if (field != 0)
              merge(exact, singleton(field));
          }
          if (!materialize(exact).empty())
            return exact;
        }
      }
      const PointsToSet &known = graph_->getObjectIds(constant);
      for (ObjectID object : known)
        merge(result, singleton(object));
      if (!known.empty())
        return result;
      return constantPointsTo(
          dyn_cast<Constant>(expression->getOperand(0)));
    }
  }
  const PointsToSet &known = graph_->getObjectIds(constant);
  for (ObjectID object : known)
    merge(result, singleton(object));
  if (!known.empty())
    return result;
  if (const auto *expression = dyn_cast<ConstantExpr>(constant))
    if (expression->isCast())
      return constantPointsTo(dyn_cast<Constant>(expression->getOperand(0)));
  const Value *stripped = constant->stripPointerCasts();
  if (const ObjectID object = graph_->getObjectId(stripped))
    merge(result, singleton(object));
  return result;
}

void FlowSensitivePTA::initializeGlobalMemory() {
  initialMemory_.clear();
  const Module *module = nullptr;
  for (const auto &entry : *graph_) {
    if (const Function *function = entry.second->getFunction()) {
      module = function->getParent();
      break;
    }
  }
  if (!module)
    return;
  for (const GlobalVariable &global : module->globals()) {
    if (!global.hasInitializer())
      continue;
    PointsToSet storageBases;
    if (const ObjectID object = graph_->getObjectId(&global))
      storageBases.insert(object);
    if (storageBases.empty()) {
      const PointsToSet &known = graph_->getObjectIds(&global);
      storageBases.insert(known.begin(), known.end());
    }
    auto underlyingGlobal = [](const Value *value) {
      const Value *current = value;
      std::unordered_set<const Value *> visited;
      while (current && visited.insert(current).second) {
        current = current->stripPointerCasts();
        if (const auto *gep = dyn_cast<GEPOperator>(current)) {
          current = gep->getPointerOperand();
          continue;
        }
        break;
      }
      return dyn_cast_or_null<GlobalVariable>(current);
    };
    auto storageAtOffset = [&](uint64_t offset) {
      PointsToSet storage;
      for (ObjectID base : storageBases) {
        const SVFG::ObjectInfo *baseInfo = graph_->getObjectInfo(base);
        const ObjectID canonicalBase =
            baseInfo && baseInfo->baseObjId != 0 ? baseInfo->baseObjId : base;
        const ObjectID object =
            offset == 0 ? canonicalBase
                        : graph_->getOffsetObject(canonicalBase, offset);
        if (object != 0)
          storage.insert(object);
      }
      for (const auto &[object, label] : graph_->getObjectDebugMap()) {
        if (underlyingGlobal(graph_->getObjectValue(object)) != &global)
          continue;
        const SVFG::ObjectInfo *info = graph_->getObjectInfo(object);
        if (info && info->hasFieldOffset && info->fieldOffset == offset)
          storage.insert(object);
      }
      return storage;
    };

    const DataLayout &layout = module->getDataLayout();
    std::function<void(const Constant *, Type *, uint64_t)> seedInitializer =
        [&](const Constant *constant, Type *type, uint64_t offset) {
          if (!constant || !type)
            return;
          if (type->isPointerTy()) {
            StoredSet pointsTo = constantPointsTo(constant);
            if (materialize(pointsTo).empty())
              return;
            for (ObjectID storage : storageAtOffset(offset))
              merge(initialMemory_[storage], pointsTo);
            return;
          }
          if (auto *structure = dyn_cast<StructType>(type)) {
            const StructLayout *structureLayout =
                layout.getStructLayout(structure);
            for (unsigned field = 0; field < structure->getNumElements();
                 ++field) {
              seedInitializer(constant->getAggregateElement(field),
                              structure->getElementType(field),
                              offset +
                                  structureLayout->getElementOffset(field));
            }
            return;
          }
          if (auto *array = dyn_cast<ArrayType>(type)) {
            for (uint64_t element = 0; element < array->getNumElements();
                 ++element)
              seedInitializer(constant->getAggregateElement(element),
                              array->getElementType(), offset);
          }
        };
    seedInitializer(global.getInitializer(), global.getValueType(), 0);
  }
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

FlowSensitivePTA::PointsToSet FlowSensitivePTA::selectAccessTargets(
    const StoredSet &flowSensitiveTargets,
    const PointsToSet &preAnalysisTargets) const {
  const PointsToSet &flow = materialize(flowSensitiveTargets);
  if (preAnalysisTargets.empty())
    return flow;
  if (flow.empty())
    return preAnalysisTargets;
  if (std::any_of(
          preAnalysisTargets.begin(), preAnalysisTargets.end(),
          [&](ObjectID object) { return graph_->isUnknownObject(object); }))
    return flow;
  if (std::any_of(flow.begin(), flow.end(), [&](ObjectID object) {
        return graph_->isUnknownObject(object);
      }))
    return preAnalysisTargets;
  return flow;
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
  bool hasIndirectPredecessor = false;
  for (const SVFGEdge *edge : node.getInEdges()) {
    if (!edge || !inScope(edge->getSrcNode()) ||
        !isIndirectVFGEdge(edge->getEdgeKind()))
      continue;
    hasIndirectPredecessor = true;
    const SVFGNode *source = edge->getSrcNode();
    const MemoryState &sourceState =
        isa<StoreSVFGNode, ActualOutSVFGNode>(source) ? outState(source)
                                                      : inState(source);
    PointsToSet guarded = edge->getPointsTo();
    const bool wildcard =
        std::any_of(guarded.begin(), guarded.end(), [&](ObjectID object) {
          return graph_->isUnknownObject(object);
        });
    if (guarded.empty())
      continue;
    if (wildcard) {
      mergeState(incoming, sourceState);
      continue;
    }
    guarded = expandIndirectObjects(guarded);
    for (ObjectID object : guarded) {
      auto value = sourceState.find(object);
      if (value != sourceState.end()) {
        merge(incoming[object], value->second);
      } else {
        // A wildcard fact is a conservative substitute only while no fact is
        // available for the precise guarded object. Unconditionally joining
        // it would collapse unrelated memory regions after one imprecise
        // access (for example a global function-pointer field and stack data).
        for (const auto &[sourceObject, sourceValue] : sourceState)
          if (graph_->isUnknownObject(sourceObject))
            merge(incoming[object], sourceValue);
      }
    }
  }
  if (!hasIndirectPredecessor)
    mergeState(incoming, initialMemory_);
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
    PointsToSet targetSet =
        selectAccessTargets(targets, load->getMemoryPointsTo());
    targetSet = expandIndirectObjects(targetSet);
    for (ObjectID object : targetSet) {
      if (graph_->isConstantObject(object))
        continue;
      auto value = incoming.find(object);
      if (value != incoming.end()) {
        merge(top, value->second);
      } else {
        for (const auto &[memoryObject, memoryValue] : incoming)
          if (graph_->isUnknownObject(memoryObject))
            merge(top, memoryValue);
      }
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
    PointsToSet targetSet =
        selectAccessTargets(targets, store->getMemoryPointsTo());
    const bool strong = isStrongUpdate(targetSet);
    bool updatedAny = false;
    for (ObjectID object : targetSet) {
      if (graph_->isConstantObject(object))
        continue;
      if (strong) {
        assign(outgoing[object], stored);
      } else {
        merge(outgoing[object], stored);
      }
      updatedAny = true;
    }
    if (updatedAny) {
      if (strong) {
        strongUpdateSites_.insert(node.getId());
        ++stats_.strongUpdateExecutions;
      } else {
        weakUpdateSites_.insert(node.getId());
        ++stats_.weakUpdateExecutions;
      }
    }
  } else if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(&node)) {
    const auto *intrinsic =
        dyn_cast_or_null<IntrinsicInst>(actualOut->getCallSite());
    if (intrinsic &&
        (intrinsic->getIntrinsicID() == Intrinsic::memcpy ||
         intrinsic->getIntrinsicID() == Intrinsic::memmove) &&
        intrinsic->arg_size() >= 2) {
      auto objectsWithFields = [&](const Value *pointer) {
        PointsToSet objects = materialize(pointerTargets(pointer));
        if (objects.empty()) {
          const PointsToSet &preAnalysis = graph_->getObjectIds(pointer);
          objects.insert(preAnalysis.begin(), preAnalysis.end());
        }
        PointsToSet expanded = objects;
        for (ObjectID object : objects) {
          const PointsToSet fields = graph_->getFieldObjects(object);
          expanded.insert(fields.begin(), fields.end());
        }
        auto underlyingBase = [](const Value *value) {
          const Value *current = value;
          std::unordered_set<const Value *> visited;
          while (current && visited.insert(current).second) {
            current = current->stripPointerCasts();
            if (const auto *gep = dyn_cast<GEPOperator>(current)) {
              current = gep->getPointerOperand();
              continue;
            }
            break;
          }
          return current;
        };
        const Value *base = underlyingBase(pointer);
        if (base) {
          for (const auto &[candidate, nodeID] : graph_->getValueNodeMap()) {
            if (!candidate || !candidate->getType()->isPointerTy() ||
                underlyingBase(candidate) != base)
              continue;
            const PointsToSet &candidateObjects =
                graph_->getObjectIds(candidate);
            expanded.insert(candidateObjects.begin(), candidateObjects.end());
            if (ObjectID candidateObject = graph_->getObjectId(candidate))
              expanded.insert(candidateObject);
          }
        }
        return expanded;
      };

      struct RootLocation {
        ObjectID base = 0;
        uint64_t offset = 0;
      };
      auto rootLocations = [&](const Value *pointer) {
        PointsToSet roots = materialize(pointerTargets(pointer));
        if (roots.empty()) {
          const PointsToSet &preAnalysis = graph_->getObjectIds(pointer);
          roots.insert(preAnalysis.begin(), preAnalysis.end());
        }
        std::vector<RootLocation> locations;
        for (ObjectID root : roots) {
          const SVFG::ObjectInfo *info = graph_->getObjectInfo(root);
          locations.push_back(
              {info && info->baseObjId != 0 ? info->baseObjId : root,
               info && info->hasFieldOffset ? info->fieldOffset : 0});
        }
        return locations;
      };

      MemoryState allInputs;
      for (const SVFGNode *actualInNode :
           graph_->getActualIns(actualOut->getCallSite())) {
        mergeState(allInputs, inState(actualInNode));
        mergeState(outgoing, inState(actualInNode));
      }
      for (const SVFGEdge *edge : node.getInEdges()) {
        const SVFGNode *source = edge ? edge->getSrcNode() : nullptr;
        if (isa_and_nonnull<StoreSVFGNode>(source))
          mergeState(allInputs, outState(source));
      }

      const std::vector<RootLocation> sourceRoots =
          rootLocations(intrinsic->getArgOperand(1));
      const std::vector<RootLocation> destinationRoots =
          rootLocations(intrinsic->getArgOperand(0));
      const auto *lengthConstant =
          intrinsic->arg_size() >= 3
              ? dyn_cast<ConstantInt>(intrinsic->getArgOperand(2))
              : nullptr;
      const bool hasExactLength = lengthConstant != nullptr;
      const uint64_t copyLength =
          lengthConstant ? lengthConstant->getZExtValue() : 0;
      auto pointerSizeForObject = [&](ObjectID object) {
        for (const auto &entry : graph_->getValueNodeMap()) {
          const Value *value = entry.first;
          const auto *gep = dyn_cast_or_null<GetElementPtrInst>(value);
          if (!gep || graph_->getObjectIds(gep).count(object) == 0)
            continue;
          const auto *pointerType =
              dyn_cast<PointerType>(gep->getResultElementType());
          if (pointerType)
            return static_cast<uint64_t>(
                intrinsic->getModule()->getDataLayout().getPointerSize(
                    pointerType->getAddressSpace()));
        }
        return static_cast<uint64_t>(
            intrinsic->getModule()->getDataLayout().getPointerSize());
      };
      auto fullyCopied = [&](uint64_t relativeOffset, uint64_t pointerSize) {
        return !hasExactLength || (relativeOffset <= copyLength &&
                                   pointerSize <= copyLength - relativeOffset);
      };
      ObjectID unknownObject = 0;
      for (const auto &[object, label] : graph_->getObjectDebugMap())
        if (graph_->isUnknownObject(object)) {
          unknownObject = object;
          break;
        }

      if (hasExactLength) {
        const PointsToSet destinationObjects =
            objectsWithFields(intrinsic->getArgOperand(0));
        StoredSet empty;
        for (ObjectID destination : destinationObjects) {
          const SVFG::ObjectInfo *info = graph_->getObjectInfo(destination);
          const ObjectID destinationBase =
              info && info->baseObjId != 0 ? info->baseObjId : destination;
          const uint64_t destinationOffset =
              info && info->hasFieldOffset ? info->fieldOffset : 0;
          for (const RootLocation &root : destinationRoots) {
            if (root.base != destinationBase || destinationOffset < root.offset)
              continue;
            const uint64_t relativeOffset = destinationOffset - root.offset;
            if (fullyCopied(relativeOffset,
                            pointerSizeForObject(destination))) {
              assign(outgoing[destination], empty);
            } else if (relativeOffset < copyLength && unknownObject != 0) {
              assign(outgoing[destination], singleton(unknownObject));
            }
          }
        }
      }

      MemoryState copiedByDestination;
      for (const auto &[source, value] : allInputs) {
        const SVFG::ObjectInfo *info = graph_->getObjectInfo(source);
        const ObjectID sourceBase =
            info && info->baseObjId != 0 ? info->baseObjId : source;
        const uint64_t sourceOffset =
            info && info->hasFieldOffset ? info->fieldOffset : 0;
        for (const RootLocation &sourceRoot : sourceRoots) {
          if (sourceRoot.base != sourceBase || sourceOffset < sourceRoot.offset)
            continue;
          const uint64_t relativeOffset = sourceOffset - sourceRoot.offset;
          if (!fullyCopied(relativeOffset, pointerSizeForObject(source)))
            continue;
          for (const RootLocation &destinationRoot : destinationRoots) {
            const uint64_t destinationOffset =
                destinationRoot.offset + relativeOffset;
            if (destinationOffset < destinationRoot.offset)
              continue;
            const ObjectID destination =
                destinationOffset == 0
                    ? destinationRoot.base
                    : graph_->getOffsetObject(destinationRoot.base,
                                              destinationOffset);
            if (destination != 0)
              merge(copiedByDestination[destination], value);
          }
        }
      }
      for (const auto &[destination, value] : copiedByDestination)
        if (hasExactLength)
          assign(outgoing[destination], value);
        else
          merge(outgoing[destination], value);
    }
  } else if (const auto *gep = dyn_cast<GepSVFGNode>(&node)) {
    const auto *instruction =
        dyn_cast_or_null<GetElementPtrInst>(gep->getValue());
    StoredSet bases = directInput(node);
    top = {};
    if (instruction && instruction->getSourceElementType()->isArrayTy()) {
      top = bases;
    } else if (instruction) {
      for (ObjectID base : materialize(bases)) {
        if (graph_->isFieldInsensitiveObject(base)) {
          merge(top, singleton(base));
          continue;
        }
        const SVFG::GepAccessInfo access = graph_->getGepAccess(instruction);
        if (access.valid) {
          ObjectID canonicalBase = base;
          uint64_t baseOffset = 0;
          if (const SVFG::ObjectInfo *info = graph_->getObjectInfo(base)) {
            if (info->baseObjId != 0)
              canonicalBase = info->baseObjId;
            if (info->hasFieldOffset)
              baseOffset = info->fieldOffset;
          }
          const uint64_t totalOffset = baseOffset + access.relativeOffset;
          if (totalOffset == 0) {
            merge(top, singleton(canonicalBase));
            continue;
          }
          if (const ObjectID offsetObject =
                  graph_->getOffsetObject(canonicalBase, totalOffset)) {
            merge(top, singleton(offsetObject));
            continue;
          }
        }
        const ObjectID mapped = graph_->getGepObject(instruction, base);
        if (mapped != 0) {
          merge(top, singleton(mapped));
          continue;
        }
        const PointsToSet &candidates = graph_->getObjectIds(gep->getValue());
        bool matchedBase = false;
        for (ObjectID candidate : candidates) {
          const SVFG::ObjectInfo *info = graph_->getObjectInfo(candidate);
          const ObjectID candidateBase =
              info && info->baseObjId != 0 ? info->baseObjId : candidate;
          if (candidate != base && candidateBase != base)
            continue;
          merge(top, singleton(candidate));
          matchedBase = true;
        }
        if (!matchedBase)
          for (ObjectID candidate : candidates)
            merge(top, singleton(candidate));
      }
    }
    if (materialize(top).empty()) {
      const PointsToSet &fields = gep->getValue()
                                      ? graph_->getObjectIds(gep->getValue())
                                      : PointsToSet{};
      for (ObjectID field : fields)
        merge(top, singleton(field));
    }
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
  initialMemory_.clear();
  strongUpdateSites_.clear();
  weakUpdateSites_.clear();
  stats_ = {};
  if (config_.setBackend == PointsToSetBackend::HashConsed)
    arena_.reset();
  initializeGlobalMemory();
  do {
    topologyChanged_ = false;
    initializeRecursiveFunctions();
    SCCInfo scc = computeSCCs();
    stats_.sccs = scc.components.size();
    stats_.nodes = 0;
    std::queue<std::size_t> worklist;
    std::vector<bool> queued(scc.components.size(), true);
    for (std::size_t i = 0; i < scc.components.size(); ++i) {
      worklist.push(i);
      stats_.nodes += scc.components[i].size();
      stats_.maxSccSize = std::max(stats_.maxSccSize, scc.components[i].size());
    }
    while (!worklist.empty()) {
      const std::size_t component = worklist.front();
      worklist.pop();
      queued[component] = false;
      bool anyChanged = false;
      std::queue<const SVFGNode *> localWorklist;
      std::unordered_set<NodeID> locallyQueued;
      for (const SVFGNode *node : scc.components[component]) {
        localWorklist.push(node);
        locallyQueued.insert(node->getId());
      }
      while (!localWorklist.empty() && !topologyChanged_) {
        const SVFGNode *node = localWorklist.front();
        localWorklist.pop();
        locallyQueued.erase(node->getId());
        if (!transfer(*node))
          continue;
        anyChanged = true;
        for (const SVFGEdge *edge : node->getOutEdges()) {
          const SVFGNode *successor = edge ? edge->getDstNode() : nullptr;
          if (!inScope(successor) ||
              scc.nodeToComponent[successor->getId()] != component ||
              locallyQueued.count(successor->getId()) != 0)
            continue;
          localWorklist.push(successor);
          locallyQueued.insert(successor->getId());
        }
      }
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
  stats_.strongUpdates = strongUpdateSites_.size();
  stats_.weakUpdates = weakUpdateSites_.size();
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
