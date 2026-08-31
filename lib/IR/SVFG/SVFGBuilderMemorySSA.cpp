//===- SVFGBuilderMemorySSA.cpp -- SVFG Memory SSA Implementation
//---------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//
//
// This file contains Memory SSA construction and interprocedural edge methods
//
//===----------------------------------------------------------------------===//

#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFGBuilder.h"

#include <functional>
#include <queue>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

using namespace lotus::analysis;
using namespace llvm;

// Helper function to get Module from ICFG
static const Module *getModuleFromICFG(const ICFG *icfg) {
  if (!icfg)
    return nullptr;

  // Iterate through ICFG nodes to find a function
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    if (const Function *F = node->getFunction()) {
      return F->getParent();
    }
  }
  return nullptr;
}

static const ICFGNode *findICFGNodeForBlock(const ICFG *icfg,
                                            const BasicBlock *bb) {
  if (!icfg || !bb)
    return nullptr;
  return const_cast<ICFG *>(icfg)->getIntraBlockNode(bb);
}

static const ICFGNode *findReturnSiteICFGNode(const ICFG *icfg,
                                              const CallBase *call) {
  if (!icfg || !call)
    return nullptr;
  return const_cast<ICFG *>(icfg)->getRetICFGNode(call);
}

static const ICFGNode *findFunctionExitICFGNode(const ICFG *icfg,
                                                const Function *F) {
  if (!icfg || !F || F->isDeclaration())
    return nullptr;
  return const_cast<ICFG *>(icfg)->getFunExitICFGNode(F);
}

static const Function *getProgramEntryFunction(const ICFG *icfg) {
  if (!icfg)
    return nullptr;
  const GlobalInitBlockNode *globalInit =
      const_cast<ICFG *>(icfg)->getGlobalInitICFGNode();
  if (!globalInit)
    return nullptr;

  const Function *soleRoot = nullptr;
  for (const ICFGEdge *edge : globalInit->getOutEdges()) {
    const ICFGNode *dst = edge ? edge->getDstNode() : nullptr;
    const auto *entry = dyn_cast_or_null<FunEntryBlockNode>(dst);
    const Function *F = entry ? entry->getFunction() : nullptr;
    if (!F)
      continue;
    if (F->getName() == "main")
      return F;
    if (!soleRoot)
      soleRoot = F;
    else if (soleRoot != F)
      soleRoot = nullptr;
  }
  return soleRoot;
}

static bool icfgHasCallEdgeTo(const ICFG *icfg, const CallBase *call,
                              const Function *callee) {
  if (!icfg || !call || !callee || callee->isDeclaration())
    return false;

  const ICFGNode *callerNode =
      const_cast<ICFG *>(icfg)->getIntraBlockNode(call->getParent());
  const ICFGNode *calleeEntry =
      const_cast<ICFG *>(icfg)->getFunEntryICFGNode(callee);
  if (!callerNode || !calleeEntry)
    return false;

  for (const auto *edge : callerNode->getOutEdges()) {
    const auto *callEdge = llvm::dyn_cast<CallCFGEdge>(edge);
    if (!callEdge)
      continue;
    if (callEdge->getDstNode() == calleeEntry &&
        callEdge->getCallSite() == call)
      return true;
  }
  return false;
}

static void ensureICFGInterEdges(const ICFG *icfg, const CallBase *call,
                                 const Function *callee) {
  if (!icfg || !call || !callee || callee->isDeclaration())
    return;

  ICFG *mutableICFG = const_cast<ICFG *>(icfg);
  ICFGNode *callerNode = mutableICFG->getIntraBlockNode(call->getParent());
  ICFGNode *calleeEntryNode = mutableICFG->getFunEntryICFGNode(callee);
  if (!callerNode || !calleeEntryNode)
    return;

  (void)mutableICFG->addCallEdge(callerNode, calleeEntryNode, call);

  ICFGNode *returnSiteNode = mutableICFG->getRetICFGNode(call);
  if (!returnSiteNode)
    return;

  ICFGNode *calleeExitNode = mutableICFG->getFunExitICFGNode(callee);
  if (calleeExitNode)
    (void)mutableICFG->addRetEdge(calleeExitNode, returnSiteNode, call);
}

static std::vector<const Function *>
filterCalleesByICFG(const ICFG *icfg, const CallBase *call,
                    const std::vector<const Function *> &ptaCallees) {
  if (!icfg)
    return ptaCallees;

  std::vector<const Function *> filtered;
  filtered.reserve(ptaCallees.size());
  for (const Function *callee : ptaCallees) {
    if (icfgHasCallEdgeTo(icfg, call, callee)) {
      filtered.push_back(callee);
    }
  }

  // Keep PTA resolution if ICFG does not expose matching call edges.
  return filtered.empty() ? ptaCallees : filtered;
}

static SVFGNodeBS intersectPointsToSets(const SVFGNodeBS &lhs,
                                        const SVFGNodeBS &rhs,
                                        uint32_t unknownObjId) {
  // Empty means "no objects" (not "unknown"). Unknown is represented explicitly
  // via a wildcard object ID.
  if (lhs.empty() || rhs.empty())
    return SVFGNodeBS{};

  if (unknownObjId != 0 &&
      (lhs.count(unknownObjId) != 0 || rhs.count(unknownObjId) != 0)) {
    return SVFGNodeBS{unknownObjId};
  }

  SVFGNodeBS out;
  const SVFGNodeBS *small = &lhs;
  const SVFGNodeBS *large = &rhs;
  if (rhs.size() < lhs.size()) {
    small = &rhs;
    large = &lhs;
  }
  for (uint32_t id : *small) {
    if (large->count(id))
      out.insert(id);
  }
  return out;
}

void SVFGBuilder::buildMemorySSA() {
  if (!config.buildMSSA)
    return;

  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  using MemRegPtsMap = std::unordered_map<uint32_t, SVFGNodeBS>;
  struct FunctionMemorySummary {
    std::unordered_set<unsigned> readArgs;
    std::unordered_set<unsigned> writeArgs;
    MemRegPtsMap readGlobals;
    MemRegPtsMap writeGlobals;
  };

  auto addRegion = [](MemRegPtsMap &dst, uint32_t memReg,
                      const SVFGNodeBS &pts) {
    auto &bucket = dst[memReg];
    bucket.insert(pts.begin(), pts.end());
  };

  auto getPtsForMemReg = [&](uint32_t memReg) -> SVFGNodeBS {
    if (memReg == 0)
      return {};
    auto ptsIt = memRegToPts.find(memReg);
    if (ptsIt != memRegToPts.end())
      return ptsIt->second;
    auto objIt = memRegToObjId.find(memReg);
    if (objIt != memRegToObjId.end())
      return SVFGNodeBS{objIt->second};
    return SVFGNodeBS{getOrCreateUnknownObjId()};
  };

  auto getGlobalBase = [](const Value *pointer) -> const GlobalValue * {
    const Value *current = pointer;
    std::unordered_set<const Value *> visited;
    while (current && visited.insert(current).second) {
      current = current->stripPointerCasts();
      if (const auto *global = dyn_cast<GlobalValue>(current))
        return global;
      if (const auto *gep = dyn_cast<GEPOperator>(current)) {
        current = gep->getPointerOperand();
        continue;
      }
      break;
    }
    return nullptr;
  };

  auto getDominatingStoredValue = [](const LoadInst *load) -> const Value * {
    if (!load || !load->getFunction())
      return nullptr;
    const Value *location = load->getPointerOperand()->stripPointerCasts();
    DominatorTree dominance(*const_cast<Function *>(load->getFunction()));
    const StoreInst *nearest = nullptr;
    for (const User *user : location->users()) {
      const auto *store = dyn_cast<StoreInst>(user);
      if (!store || store->getFunction() != load->getFunction() ||
          store->getPointerOperand()->stripPointerCasts() != location ||
          !dominance.dominates(store, load))
        continue;
      if (!nearest || dominance.dominates(nearest, store))
        nearest = store;
    }
    return nearest ? nearest->getValueOperand() : nullptr;
  };

  auto getSpilledFormalOrigin = [&](const Value *value) -> const Argument * {
    const auto *instruction = dyn_cast_or_null<Instruction>(value);
    const Function *context =
        instruction ? instruction->getFunction() : nullptr;
    if (!context)
      return nullptr;
    std::unordered_set<const Value *> visited;
    std::unordered_set<const Argument *> origins;
    std::function<void(const Value *)> visit = [&](const Value *current) {
      if (!current || !current->getType()->isPointerTy())
        return;
      current = current->stripPointerCasts();
      if (!visited.insert(current).second)
        return;
      if (const auto *argument = dyn_cast<Argument>(current)) {
        if (argument->getParent() == context)
          origins.insert(argument);
        return;
      }
      if (const auto *load = dyn_cast<LoadInst>(current)) {
        visit(getDominatingStoredValue(load));
        return;
      }
      if (const auto *phi = dyn_cast<PHINode>(current)) {
        for (const Value *incoming : phi->incoming_values())
          visit(incoming);
        return;
      }
      if (const auto *select = dyn_cast<SelectInst>(current)) {
        visit(select->getTrueValue());
        visit(select->getFalseValue());
        return;
      }
      if (const auto *gep = dyn_cast<GEPOperator>(current))
        visit(gep->getPointerOperand());
    };
    visit(value);
    return origins.size() == 1 ? *origins.begin() : nullptr;
  };

  auto getCanonicalRegionForPointer =
      [&](const Value *ptr) -> std::pair<uint32_t, SVFGNodeBS> {
    if (!ptr || !ptr->getType()->isPointerTy())
      return {0, {}};
    if (isa<GetElementPtrInst>(ptr)) {
      const SVFGNodeBS &mapped = svfg->getObjectIds(ptr);
      if (!mapped.empty()) {
        const uint32_t memRegId =
            getOrCreateMemRegForPointsTo(mapped, getMemoryRegionScope(ptr));
        auto canonical = memRegToPts.find(memRegId);
        return {memRegId,
                canonical == memRegToPts.end() ? mapped : canonical->second};
      }
    }
    std::vector<const void *> ptsVoid = getPointsToSet(ptr);
    SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);
    if (!objIds.empty()) {
      const uint32_t memRegId =
          getOrCreateMemRegForPointsTo(objIds, getMemoryRegionScope(ptr));
      auto canonical = memRegToPts.find(memRegId);
      return {memRegId,
              canonical == memRegToPts.end() ? objIds : canonical->second};
    }
    if (const Argument *origin = getSpilledFormalOrigin(ptr)) {
      SVFGNodeBS originObjects =
          convertPTAObjectsToObjIDs(getPointsToSet(origin));
      if (!originObjects.empty()) {
        const uint32_t memRegId = getOrCreateMemRegForPointsTo(
            originObjects, getMemoryRegionScope(origin));
        auto canonical = memRegToPts.find(memRegId);
        return {memRegId, canonical == memRegToPts.end() ? originObjects
                                                         : canonical->second};
      }
      return {getOrCreateMemReg(origin), SVFGNodeBS{getOrCreateUnknownObjId()}};
    }
    if (const GlobalValue *global = getGlobalBase(ptr)) {
      // AserPTA may omit a field object for ConstantExpr GEPs. Give that
      // global-derived location its own canonical object instead of merging
      // it into the universal black-hole object; otherwise an imprecise
      // function-pointer field contaminates unrelated stack regions.
      SVFG::ObjectInfo info;
      info.isGlobal = true;
      info.isSingleton = true;
      info.isConstant = isa<GlobalVariable>(global) &&
                        cast<GlobalVariable>(global)->isConstant();
      const uint32_t object = getOrCreateCanonicalObjectIdForValue(ptr, info);
      if (const auto *gep = dyn_cast<GEPOperator>(ptr)) {
        APInt offset(M->getDataLayout().getIndexTypeSizeInBits(
                         gep->getPointerOperandType()),
                     0);
        if (gep->accumulateConstantOffset(M->getDataLayout(), offset)) {
          const uint32_t baseObject =
              getOrCreateCanonicalObjectIdForValue(global, info);
          svfg->setObjectBase(object, baseObject);
          svfg->setObjectOffset(object, offset.getZExtValue());
          svfg->setOffsetObject(baseObject, offset.getZExtValue(), object);
        }
      }
      const SVFGNodeBS synthetic{object};
      const uint32_t memRegId = getOrCreateMemRegForPointsTo(synthetic);
      return {memRegId, synthetic};
    }
    const uint32_t unknown = getOrCreateUnknownObjId();
    return {getOrCreateMemReg(ptr), SVFGNodeBS{unknown}};
  };

  auto mergeRegions = [&](MemRegPtsMap &dst, const MemRegPtsMap &src) {
    for (const auto &entry : src)
      addRegion(dst, entry.first, entry.second);
  };

  auto regionMapsEqual = [](const MemRegPtsMap &lhs, const MemRegPtsMap &rhs) {
    if (lhs.size() != rhs.size())
      return false;
    for (const auto &entry : lhs) {
      auto it = rhs.find(entry.first);
      if (it == rhs.end() || it->second != entry.second)
        return false;
    }
    return true;
  };

  auto summariesEqual = [&](const FunctionMemorySummary &lhs,
                            const FunctionMemorySummary &rhs) {
    return lhs.readArgs == rhs.readArgs && lhs.writeArgs == rhs.writeArgs &&
           regionMapsEqual(lhs.readGlobals, rhs.readGlobals) &&
           regionMapsEqual(lhs.writeGlobals, rhs.writeGlobals);
  };

  std::function<bool(const Value *, std::unordered_set<const Value *> &)>
      isGlobalDerivedPointerRec =
          [&](const Value *value,
              std::unordered_set<const Value *> &visited) -> bool {
    if (!value || !value->getType()->isPointerTy())
      return false;

    const Value *base = value->stripPointerCasts();
    if (!visited.insert(base).second)
      return false;

    if (isa<GlobalVariable>(base) || isa<GlobalAlias>(base))
      return true;

    // Constant-expression GEPs are common in unoptimized IR for global
    // structure fields. They are GEPOperators but not Instructions, so they
    // must be peeled before the instruction-only cases below.
    if (const auto *gep = dyn_cast<GEPOperator>(base))
      return isGlobalDerivedPointerRec(gep->getPointerOperand(), visited);

    const auto *inst = dyn_cast<Instruction>(base);
    if (!inst)
      return false;

    if (const auto *load = dyn_cast<LoadInst>(inst))
      return isGlobalDerivedPointerRec(load->getPointerOperand(), visited);

    if (const auto *phi = dyn_cast<PHINode>(inst)) {
      for (const Value *incoming : phi->incoming_values()) {
        if (isGlobalDerivedPointerRec(incoming, visited))
          return true;
      }
      return false;
    }

    if (const auto *sel = dyn_cast<SelectInst>(inst)) {
      return isGlobalDerivedPointerRec(sel->getTrueValue(), visited) ||
             isGlobalDerivedPointerRec(sel->getFalseValue(), visited);
    }

    if (const auto *gep = dyn_cast<GetElementPtrInst>(inst))
      return isGlobalDerivedPointerRec(gep->getPointerOperand(), visited);

    if (const auto *castInst = dyn_cast<CastInst>(inst))
      return isGlobalDerivedPointerRec(castInst->getOperand(0), visited);

    return false;
  };

  auto isGlobalDerivedPointer = [&](const Value *value) {
    std::unordered_set<const Value *> visited;
    return isGlobalDerivedPointerRec(value, visited);
  };

  SVFGNodeBS globalReachableObjIds;
  if (config.includeGlobals) {
    std::queue<uint32_t> worklist;
    auto enqueuePts = [&](const SVFGNodeBS &pts) {
      for (uint32_t objId : pts) {
        if (globalReachableObjIds.insert(objId).second)
          worklist.push(objId);
      }
    };

    for (const GlobalVariable &gv : M->globals()) {
      enqueuePts(getObjectIdsForValue(&gv));
      if (const uint32_t globalObjId = svfg->getObjectId(&gv))
        enqueuePts(SVFGNodeBS{globalObjId});
    }

    while (!worklist.empty()) {
      const uint32_t objId = worklist.front();
      worklist.pop();
      const Value *objValue = svfg->getObjectValue(objId);
      if (!objValue || !objValue->getType()->isPointerTy())
        continue;
      enqueuePts(getObjectIdsForValue(objValue));
    }
  }

  auto collectVisibleRegions = [&](const Value *ptr) -> MemRegPtsMap {
    MemRegPtsMap regions;
    if (!ptr || !ptr->getType()->isPointerTy())
      return regions;

    std::vector<const void *> ptsVoid = getPointsToSet(ptr);
    SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);
    const Value *base = ptr->stripPointerCasts();
    const bool directlyGlobal =
        isa<GlobalVariable>(base) || isa<GlobalAlias>(base);
    bool globallyVisiblePointees =
        directlyGlobal || isGlobalDerivedPointer(ptr);
    if (!globallyVisiblePointees && !globalReachableObjIds.empty()) {
      for (uint32_t objId : objIds) {
        if (globalReachableObjIds.count(objId) != 0) {
          globallyVisiblePointees = true;
          break;
        }
      }
    }

    if (globallyVisiblePointees) {
      if (!objIds.empty()) {
        const uint32_t region =
            getOrCreateMemRegForPointsTo(objIds, getMemoryRegionScope(ptr));
        auto canonical = memRegToPts.find(region);
        addRegion(regions, region,
                  canonical == memRegToPts.end() ? objIds : canonical->second);
      } else {
        const auto region = getCanonicalRegionForPointer(ptr);
        if (region.first != 0)
          addRegion(regions, region.first, region.second);
      }
      return regions;
    }

    if (directlyGlobal) {
      addRegion(regions, getOrCreateMemReg(base),
                SVFGNodeBS{getOrCreateUnknownObjId()});
    }

    return regions;
  };

  auto collectFormalPointerOrigins = [&](const Function *context,
                                         const Value *value) {
    std::unordered_set<unsigned> origins;
    std::unordered_set<const Value *> visited;
    std::function<void(const Value *)> visit = [&](const Value *current) {
      if (!current || !current->getType()->isPointerTy())
        return;
      current = current->stripPointerCasts();
      if (!visited.insert(current).second)
        return;
      if (const auto *argument = dyn_cast<Argument>(current)) {
        if (argument->getParent() == context)
          origins.insert(argument->getArgNo());
        return;
      }
      if (const auto *load = dyn_cast<LoadInst>(current)) {
        visit(getDominatingStoredValue(load));
        return;
      }
      if (const auto *phi = dyn_cast<PHINode>(current)) {
        for (const Value *incoming : phi->incoming_values())
          visit(incoming);
        return;
      }
      if (const auto *select = dyn_cast<SelectInst>(current)) {
        visit(select->getTrueValue());
        visit(select->getFalseValue());
        return;
      }
      if (const auto *gep = dyn_cast<GEPOperator>(current))
        visit(gep->getPointerOperand());
    };
    visit(value);
    return origins;
  };

  auto recordVisibleAccess = [&](const Function *context, const Value *ptr,
                                 FunctionMemorySummary &summary, bool isRead,
                                 bool isWrite) {
    if (!ptr || !ptr->getType()->isPointerTy())
      return;

    if (const auto *arg = dyn_cast<Argument>(ptr->stripPointerCasts())) {
      if (arg->getParent() == context) {
        if (isRead)
          summary.readArgs.insert(arg->getArgNo());
        if (isWrite)
          summary.writeArgs.insert(arg->getArgNo());
        return;
      }
    }

    const std::unordered_set<unsigned> syntacticOrigins =
        collectFormalPointerOrigins(context, ptr);
    if (!syntacticOrigins.empty()) {
      for (unsigned argument : syntacticOrigins) {
        if (isRead)
          summary.readArgs.insert(argument);
        if (isWrite)
          summary.writeArgs.insert(argument);
      }
      return;
    }

    // A memory operand is often a load from an unoptimized local slot that
    // contains a formal pointer (e.g. -O0 parameter spilling). Attribute the
    // access to every formal whose auxiliary points-to set overlaps it. This
    // is the region-level equivalent of SVF's PAG/MemSSA parameter summary;
    // checking only `ptr` itself for Argument misses all such spilled forms.
    const SVFGNodeBS accessObjects =
        convertPTAObjectsToObjIDs(getPointsToSet(ptr));
    bool matchedArgument = false;
    if (context && !accessObjects.empty()) {
      for (const Argument &argument : context->args()) {
        if (!argument.getType()->isPointerTy())
          continue;
        const SVFGNodeBS argumentObjects =
            convertPTAObjectsToObjIDs(getPointsToSet(&argument));
        if (argumentObjects.empty())
          continue;
        if (intersectPointsToSets(accessObjects, argumentObjects,
                                  getOrCreateUnknownObjId())
                .empty())
          continue;
        if (isRead)
          summary.readArgs.insert(argument.getArgNo());
        if (isWrite)
          summary.writeArgs.insert(argument.getArgNo());
        matchedArgument = true;
      }
    }
    if (matchedArgument)
      return;

    MemRegPtsMap visibleRegs = collectVisibleRegions(ptr);
    if (isRead)
      mergeRegions(summary.readGlobals, visibleRegs);
    if (isWrite)
      mergeRegions(summary.writeGlobals, visibleRegs);
  };

  auto getCallTargets = [&](const CallBase *call) {
    std::vector<const Function *> callees;
    if (!call)
      return callees;

    if (const Function *directCallee = call->getCalledFunction()) {
      if (!directCallee->isDeclaration())
        callees.push_back(directCallee);
      return callees;
    }

    // Match SVF's DDA setup: even when top-level call/return edges are left
    // unresolved for on-demand refinement, MemorySSA still needs the
    // pre-analysis indirect-call targets to build ActualIn/ActualOut nodes and
    // memory summaries. Without those nodes, DDA can never reach the
    // callsite-ret/function-entry memory nodes that trigger resolveFunPtr().
    if (config.usePointerAnalysis && ptaSolverWrapper &&
        ptaSolverWrapper->solver) {
      callees = getIndirectCallTargets(call);
      callees = filterCalleesByICFG(icfg, call, callees);
    }
    return callees;
  };

  auto collectDirectSummary = [&](const Function &F) {
    FunctionMemorySummary summary;
    for (const BasicBlock &bb : F) {
      for (const Instruction &inst : bb) {
        if (const auto *load = dyn_cast<LoadInst>(&inst)) {
          if (isAddressTakenPointer(load->getPointerOperand()))
            recordVisibleAccess(&F, load->getPointerOperand(), summary, true,
                                false);
          continue;
        }
        if (const auto *store = dyn_cast<StoreInst>(&inst)) {
          if (isAddressTakenPointer(store->getPointerOperand()))
            recordVisibleAccess(&F, store->getPointerOperand(), summary, false,
                                true);
          continue;
        }
        if (const auto *rmw = dyn_cast<AtomicRMWInst>(&inst)) {
          if (isAddressTakenPointer(rmw->getPointerOperand()))
            recordVisibleAccess(&F, rmw->getPointerOperand(), summary, true,
                                true);
          continue;
        }
        if (const auto *cmpxchg = dyn_cast<AtomicCmpXchgInst>(&inst)) {
          if (isAddressTakenPointer(cmpxchg->getPointerOperand()))
            recordVisibleAccess(&F, cmpxchg->getPointerOperand(), summary, true,
                                true);
          continue;
        }
        const auto *call = dyn_cast<CallBase>(&inst);
        if (!call)
          continue;
        const auto *intrinsic = dyn_cast<IntrinsicInst>(call);
        if (!intrinsic)
          continue;
        switch (intrinsic->getIntrinsicID()) {
        case Intrinsic::memcpy:
        case Intrinsic::memmove:
          if (call->arg_size() >= 1 &&
              call->getArgOperand(0)->getType()->isPointerTy()) {
            recordVisibleAccess(&F, call->getArgOperand(0), summary, false,
                                true);
          }
          if (call->arg_size() >= 2 &&
              call->getArgOperand(1)->getType()->isPointerTy()) {
            recordVisibleAccess(&F, call->getArgOperand(1), summary, true,
                                false);
          }
          break;
        case Intrinsic::memset:
          if (call->arg_size() >= 1 &&
              call->getArgOperand(0)->getType()->isPointerTy()) {
            recordVisibleAccess(&F, call->getArgOperand(0), summary, false,
                                true);
          }
          break;
        default:
          break;
        }
      }
    }
    return summary;
  };

  std::unordered_map<const Function *, FunctionMemorySummary> directSummaries;
  std::unordered_map<const Function *, FunctionMemorySummary> summaries;
  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;
    directSummaries[&F] = collectDirectSummary(F);
    summaries[&F] = directSummaries[&F];
  }

  bool summaryChanged = true;
  while (summaryChanged) {
    summaryChanged = false;
    for (const Function &F : *M) {
      if (F.isDeclaration())
        continue;

      FunctionMemorySummary next = directSummaries[&F];
      for (const BasicBlock &bb : F) {
        for (const Instruction &inst : bb) {
          const auto *call = dyn_cast<CallBase>(&inst);
          if (!call)
            continue;
          if (isa<IntrinsicInst>(call))
            continue;

          const bool mayRead = callMayReadMemory(call);
          const bool mayWrite = callMayModifyMemory(call);
          if (!mayRead && !mayWrite)
            continue;

          const std::vector<const Function *> callees = getCallTargets(call);
          if (callees.empty()) {
            for (unsigned i = 0; i < call->arg_size(); ++i) {
              const Value *arg = call->getArgOperand(i);
              if (!arg->getType()->isPointerTy())
                continue;
              recordVisibleAccess(&F, arg, next,
                                  mayRead && callArgMayReadMemory(call, i),
                                  mayWrite && callArgMayModifyMemory(call, i));
            }
            continue;
          }

          for (const Function *callee : callees) {
            auto calleeIt = summaries.find(callee);
            if (calleeIt == summaries.end())
              continue;
            const FunctionMemorySummary &calleeSummary = calleeIt->second;

            if (mayRead) {
              for (unsigned argIdx : calleeSummary.readArgs) {
                if (argIdx >= call->arg_size() ||
                    !callArgMayReadMemory(call, argIdx))
                  continue;
                recordVisibleAccess(&F, call->getArgOperand(argIdx), next, true,
                                    false);
              }
              mergeRegions(next.readGlobals, calleeSummary.readGlobals);
            }

            if (mayWrite) {
              for (unsigned argIdx : calleeSummary.writeArgs) {
                if (argIdx >= call->arg_size() ||
                    !callArgMayModifyMemory(call, argIdx))
                  continue;
                recordVisibleAccess(&F, call->getArgOperand(argIdx), next,
                                    false, true);
              }
              mergeRegions(next.writeGlobals, calleeSummary.writeGlobals);
            }
          }
        }
      }

      if (!summariesEqual(next, summaries[&F])) {
        summaries[&F] = std::move(next);
        summaryChanged = true;
      }
    }
  }

  std::unordered_map<const CallBase *, MemRegPtsMap> callReadRegions;
  std::unordered_map<const CallBase *, MemRegPtsMap> callWriteRegions;
  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;
    for (const BasicBlock &bb : F) {
      for (const Instruction &inst : bb) {
        const auto *call = dyn_cast<CallBase>(&inst);
        if (!call)
          continue;

        const bool mayRead = callMayReadMemory(call);
        const bool mayWrite = callMayModifyMemory(call);
        if (!mayRead && !mayWrite)
          continue;

        MemRegPtsMap readRegs;
        MemRegPtsMap writeRegs;
        const auto *intrinsic = dyn_cast<IntrinsicInst>(call);
        if (intrinsic) {
          auto addIntrinsicRegion = [&](unsigned argument, bool read,
                                        bool write) {
            if (argument >= call->arg_size())
              return;
            const Value *pointer = call->getArgOperand(argument);
            if (!pointer->getType()->isPointerTy())
              return;
            const auto region = getCanonicalRegionForPointer(pointer);
            if (region.first == 0)
              return;
            if (read)
              addRegion(readRegs, region.first, region.second);
            if (write)
              addRegion(writeRegs, region.first, region.second);
          };
          switch (intrinsic->getIntrinsicID()) {
          case Intrinsic::memcpy:
          case Intrinsic::memmove:
            addIntrinsicRegion(0, false, true);
            addIntrinsicRegion(1, true, false);
            break;
          case Intrinsic::memset:
            addIntrinsicRegion(0, false, true);
            break;
          default:
            continue;
          }
        } else {
          const std::vector<const Function *> callees = getCallTargets(call);
          if (callees.empty()) {
            for (unsigned i = 0; i < call->arg_size(); ++i) {
              const Value *arg = call->getArgOperand(i);
              if (!arg->getType()->isPointerTy())
                continue;
              MemRegPtsMap argRegs;
              const auto region = getCanonicalRegionForPointer(arg);
              const uint32_t memRegId = region.first;
              const SVFGNodeBS &pts = region.second;
              if (memRegId == 0)
                continue;
              addRegion(argRegs, memRegId, pts);
              if (mayRead && callArgMayReadMemory(call, i))
                mergeRegions(readRegs, argRegs);
              if (mayWrite && callArgMayModifyMemory(call, i))
                mergeRegions(writeRegs, argRegs);
            }
          } else {
            for (const Function *callee : callees) {
              auto calleeIt = summaries.find(callee);
              if (calleeIt == summaries.end())
                continue;
              const FunctionMemorySummary &calleeSummary = calleeIt->second;

              if (mayRead) {
                for (unsigned argIdx : calleeSummary.readArgs) {
                  if (argIdx >= call->arg_size() ||
                      !callArgMayReadMemory(call, argIdx))
                    continue;
                  const Value *arg = call->getArgOperand(argIdx);
                  if (!arg->getType()->isPointerTy())
                    continue;
                  const auto region = getCanonicalRegionForPointer(arg);
                  const uint32_t memRegId = region.first;
                  const SVFGNodeBS &pts = region.second;
                  if (memRegId != 0)
                    addRegion(readRegs, memRegId, pts);
                }
                mergeRegions(readRegs, calleeSummary.readGlobals);
              }

              if (mayWrite) {
                for (unsigned argIdx : calleeSummary.writeArgs) {
                  if (argIdx >= call->arg_size() ||
                      !callArgMayModifyMemory(call, argIdx))
                    continue;
                  const Value *arg = call->getArgOperand(argIdx);
                  if (!arg->getType()->isPointerTy())
                    continue;
                  const auto region = getCanonicalRegionForPointer(arg);
                  const uint32_t memRegId = region.first;
                  const SVFGNodeBS &pts = region.second;
                  if (memRegId != 0)
                    addRegion(writeRegs, memRegId, pts);
                }
                mergeRegions(writeRegs, calleeSummary.writeGlobals);
              }
            }
          }
        }

        // A may-write region is also an input to the callee: weak updates and
        // untouched objects must see the reaching memory version. Model the
        // SVF ActualIn/FormalIn side for MOD regions even when the callee does
        // not explicitly read the old value, while ActualOut remains limited
        // to the may-write set.
        mergeRegions(readRegs, writeRegs);
        if (!readRegs.empty())
          callReadRegions.emplace(call, std::move(readRegs));
        if (!writeRegs.empty())
          callWriteRegions.emplace(call, std::move(writeRegs));
      }
    }
  }

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;
    DominatorTree dominance(const_cast<Function &>(F));

    // Canonical MemorySSA surface:
    //   - Load/store statements carry the direct memory use/def metadata.
    //   - ActualIn/FormalOut are memory uses.
    //   - FormalIn/ActualOut/IntraMSSAPhi are versioned memory defs.
    for (const BasicBlock &bb : F) {
      for (const Instruction &inst : bb) {
        if (const LoadInst *load = dyn_cast<LoadInst>(&inst)) {
          const Value *ptr = load->getPointerOperand();
          if (!isAddressTakenPointer(ptr))
            continue;
          auto loadIt = loadToLoadNode.find(load);
          if (loadIt == loadToLoadNode.end())
            continue;
          auto *loadNode =
              dyn_cast<LoadSVFGNode>(svfg->getNode(loadIt->second));
          if (!loadNode)
            continue;
          const auto region = getCanonicalRegionForPointer(ptr);
          const uint32_t memRegId = region.first;
          const SVFGNodeBS &pts = region.second;
          if (memRegId != 0) {
            loadNode->setMemoryUse(memRegId, 0, pts);
            if (isa<GetElementPtrInst>(ptr) && !pts.empty())
              svfg->setObjectsForValue(ptr, pts);
          }
        }

        if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
          const Value *ptr = store->getPointerOperand();
          if (!isAddressTakenPointer(ptr))
            continue;
          auto storeIt = storeToStoreNode.find(store);
          if (storeIt == storeToStoreNode.end())
            continue;
          auto *storeNode =
              dyn_cast<StoreSVFGNode>(svfg->getNode(storeIt->second));
          if (!storeNode)
            continue;
          const auto region = getCanonicalRegionForPointer(ptr);
          const uint32_t memRegId = region.first;
          const SVFGNodeBS &pts = region.second;
          if (memRegId == 0)
            continue;
          if (isa<GetElementPtrInst>(ptr) && !pts.empty())
            svfg->setObjectsForValue(ptr, pts);
          const uint32_t version = nextVersion(&F, memRegId);
          storeNode->setMemoryDef(memRegId, version, pts);
          svfg->setMSSADef(memRegId, storeNode, version);
        }

        if (const CallBase *call = dyn_cast<CallBase>(&inst)) {
          auto readRegsIt = callReadRegions.find(call);
          auto writeRegsIt = callWriteRegions.find(call);
          if (readRegsIt == callReadRegions.end() &&
              writeRegsIt == callWriteRegions.end())
            continue;

          // Find ICFG node
          const ICFGNode *icfgNode = nullptr;
          for (auto &pair : *icfg) {
            if (IntraBlockNode *blockNode =
                    dyn_cast<IntraBlockNode>(pair.second)) {
              if (blockNode->getBasicBlock() == &bb) {
                icfgNode = blockNode;
                break;
              }
            }
          }

          auto &muVec = callToMuNodes[call];
          auto &chiVec = callToChiNodes[call];
          if (readRegsIt != callReadRegions.end()) {
            for (const auto &entry : readRegsIt->second) {
              const uint32_t memRegId = entry.first;
              const SVFGNodeBS &pts = entry.second;
              const uint32_t actualInId = nextNode();
              auto *actualIn = new ActualInSVFGNode(actualInId, icfgNode, call,
                                                    memRegId, pts);
              svfg->addNode(actualIn);
              svfg->addActualIn(call, actualIn);
              muVec.push_back(actualInId);
            }
          }
          if (writeRegsIt != callWriteRegions.end()) {
            for (const auto &entry : writeRegsIt->second) {
              const uint32_t memRegId = entry.first;
              const SVFGNodeBS &pts = entry.second;
              const uint32_t actualOutId = nextNode();
              const uint32_t actualOutVersion = nextVersion(&F, memRegId);
              auto *actualOut = new ActualOutSVFGNode(
                  actualOutId, findReturnSiteICFGNode(icfg, call), call,
                  memRegId, pts, actualOutVersion);
              svfg->addNode(actualOut);
              svfg->addActualOut(call, actualOut);
              chiVec.push_back(actualOutId);
              svfg->setMSSADef(memRegId, actualOut, actualOutVersion);
            }
          }

          // Memory-transfer intrinsics have no callee body whose FormalIn/
          // FormalOut edges could order their source MU before destination
          // CHI. Add explicit sparse dependencies so the fspta transfer for
          // memcpy/memmove can consume every ActualIn state and re-run when a
          // source fact changes.
          if (const auto *intrinsic = dyn_cast<IntrinsicInst>(call)) {
            if (intrinsic->getIntrinsicID() == Intrinsic::memcpy ||
                intrinsic->getIntrinsicID() == Intrinsic::memmove) {
              for (uint32_t muId : muVec)
                for (uint32_t chiId : chiVec)
                  svfg->addEdge(svfg->getNode(muId), svfg->getNode(chiId),
                                SVFGEdgeK::IntraIndirect);

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
              const Value *destinationBase =
                  underlyingBase(intrinsic->getArgOperand(0));
              const Value *sourceBase =
                  underlyingBase(intrinsic->getArgOperand(1));
              const auto sourceRegion =
                  getCanonicalRegionForPointer(intrinsic->getArgOperand(1));
              std::unordered_set<uint32_t> sourceObjectBases;
              for (uint32_t object : sourceRegion.second) {
                const SVFG::ObjectInfo *info = svfg->getObjectInfo(object);
                sourceObjectBases.insert(
                    info && info->baseObjId != 0 ? info->baseObjId : object);
              }
              for (const auto &[store, storeNodeId] : storeToStoreNode) {
                const bool sameSourceBase =
                    underlyingBase(store->getPointerOperand()) == sourceBase;
                bool sameSourceObject = false;
                if (const auto *storeNode = dyn_cast_or_null<StoreSVFGNode>(
                        svfg->getNode(storeNodeId))) {
                  for (uint32_t object : storeNode->getMemoryPointsTo()) {
                    const SVFG::ObjectInfo *info = svfg->getObjectInfo(object);
                    const uint32_t base =
                        info && info->baseObjId != 0 ? info->baseObjId : object;
                    sameSourceObject |= sourceObjectBases.count(base) != 0;
                  }
                }
                if ((!sameSourceBase && !sameSourceObject) ||
                    !dominance.dominates(store, intrinsic))
                  continue;
                for (uint32_t chiId : chiVec)
                  svfg->addEdge(svfg->getNode(storeNodeId),
                                svfg->getNode(chiId), SVFGEdgeK::IntraIndirect);
              }
              for (const auto &[load, loadNodeId] : loadToLoadNode) {
                if (underlyingBase(load->getPointerOperand()) !=
                    destinationBase)
                  continue;
                const auto loadRegion =
                    getCanonicalRegionForPointer(load->getPointerOperand());
                SVFGNodeBS guard = loadRegion.second;
                if (guard.empty())
                  guard.insert(getOrCreateUnknownObjId());
                for (uint32_t chiId : chiVec)
                  svfg->addEdge(svfg->getNode(chiId), svfg->getNode(loadNodeId),
                                SVFGEdgeK::IntraIndirect, nullptr, guard);
              }
            }
          }

          if (readRegsIt == callReadRegions.end())
            callToMuNodes.erase(call);
          if (writeRegsIt == callWriteRegions.end())
            callToChiNodes.erase(call);
        }
      }
    }
  }

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    auto summaryIt = summaries.find(&F);
    if (summaryIt == summaries.end())
      continue;
    const FunctionMemorySummary &summary = summaryIt->second;
    const ICFGNode *entryICFGNode =
        findICFGNodeForBlock(icfg, &F.getEntryBlock());

    std::unordered_map<uint32_t, SVFGNodeBS> formalInRegs = summary.readGlobals;
    std::unordered_map<uint32_t, SVFGNodeBS> formalOutRegs =
        summary.readGlobals;
    mergeRegions(formalInRegs, summary.writeGlobals);
    mergeRegions(formalOutRegs, summary.writeGlobals);

    auto addFormalArgRegions = [&](unsigned argIdx, MemRegPtsMap &dst) {
      if (argIdx >= F.arg_size())
        return;
      const Argument *arg = F.getArg(argIdx);
      if (!arg || !arg->getType()->isPointerTy())
        return;

      const auto region = getCanonicalRegionForPointer(arg);
      const uint32_t memRegId = region.first;
      const SVFGNodeBS &pts = region.second;
      if (memRegId != 0)
        addRegion(dst, memRegId, pts);
    };

    for (unsigned argIdx : summary.readArgs)
      addFormalArgRegions(argIdx, formalInRegs);
    for (unsigned argIdx : summary.readArgs)
      addFormalArgRegions(argIdx, formalOutRegs);
    for (unsigned argIdx : summary.writeArgs)
      addFormalArgRegions(argIdx, formalInRegs);
    for (unsigned argIdx : summary.writeArgs)
      addFormalArgRegions(argIdx, formalOutRegs);

    auto entryRegIt = funcEntryChiMemRegs.find(&F);
    if (entryRegIt != funcEntryChiMemRegs.end()) {
      for (uint32_t memReg : entryRegIt->second) {
        if (formalInRegs.find(memReg) == formalInRegs.end())
          formalInRegs.emplace(memReg, getPtsForMemReg(memReg));
      }
    }

    for (const auto &entry : formalInRegs) {
      const uint32_t formalInId = nextNode();
      const uint32_t formalInVersion = nextVersion(&F, entry.first);
      auto *formalIn =
          new FormalInSVFGNode(formalInId, entryICFGNode, &F, entry.first,
                               entry.second, formalInVersion);
      svfg->addNode(formalIn);
      svfg->addFormalIn(&F, formalIn);
      funcEntryChi[&F].push_back(formalInId);
      svfg->setMSSADef(entry.first, formalIn, formalInVersion);
    }

    for (const auto &entry : formalOutRegs) {
      const uint32_t formalOutId = nextNode();
      auto *formalOut =
          new FormalOutSVFGNode(formalOutId, findFunctionExitICFGNode(icfg, &F),
                                &F, entry.first, entry.second);
      svfg->addNode(formalOut);
      svfg->addFormalOut(&F, formalOut);
    }
  }
}

void SVFGBuilder::buildMemoryPHINodes() {
  if (!config.buildMSSA)
    return;

  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  auto getDefInfo = [&](SVFGNode *node, uint32_t &memReg, uint32_t &version,
                        SVFGNodeBS &pts) -> bool {
    if (!node)
      return false;
    if (auto *store = dyn_cast<StoreSVFGNode>(node)) {
      memReg = store->getMemoryDefReg();
      version = store->getMemoryDefVersion();
      pts = store->getMemoryPointsTo();
      return memReg != 0;
    }
    if (auto *mem = dyn_cast<MSSASVFGNode>(node)) {
      if (!isMemDefSVFGNode(mem->getNodeKind()) &&
          mem->getNodeKind() != SVFGK::MIntraPhi) {
        return false;
      }
      memReg = mem->getMemReg();
      version = mem->getSSAVersion();
      pts = mem->getDefSVFVars();
      return memReg != 0;
    }
    return false;
  };

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    std::unordered_map<uint32_t, SVFGNode *> formalInByReg;
    std::set<uint32_t> memRegsWithDefs;
    std::unordered_map<const BasicBlock *,
                       std::unordered_map<uint32_t, SVFGNode *>>
        localDefs;

    for (uint32_t formalInId : funcEntryChi[&F]) {
      if (auto *formalIn =
              dyn_cast<FormalInSVFGNode>(svfg->getNode(formalInId))) {
        formalInByReg[formalIn->getMemReg()] = formalIn;
        memRegsWithDefs.insert(formalIn->getMemReg());
      }
    }

    for (const BasicBlock &bb : F) {
      auto &blockDefs = localDefs[&bb];
      for (const Instruction &inst : bb) {
        if (const auto *store = dyn_cast<StoreInst>(&inst)) {
          auto storeIt = storeToStoreNode.find(store);
          if (storeIt == storeToStoreNode.end())
            continue;
          auto *storeNode =
              dyn_cast<StoreSVFGNode>(svfg->getNode(storeIt->second));
          if (!storeNode || storeNode->getMemoryDefReg() == 0)
            continue;
          blockDefs[storeNode->getMemoryDefReg()] = storeNode;
          memRegsWithDefs.insert(storeNode->getMemoryDefReg());
        }

        if (const auto *call = dyn_cast<CallBase>(&inst)) {
          auto chiIt = callToChiNodes.find(call);
          if (chiIt != callToChiNodes.end()) {
            for (uint32_t chiId : chiIt->second) {
              if (auto *actualOut =
                      dyn_cast<ActualOutSVFGNode>(svfg->getNode(chiId))) {
                blockDefs[actualOut->getMemReg()] = actualOut;
                memRegsWithDefs.insert(actualOut->getMemReg());
              }
            }
          }
        }
      }
    }

    using ReachingDefSet = std::set<SVFGNode *>;
    using BlockReachingDefs =
        std::unordered_map<const BasicBlock *,
                           std::unordered_map<uint32_t, ReachingDefSet>>;
    auto computeExitDefs = [&](BlockReachingDefs &out) {
      out.clear();
      std::queue<const BasicBlock *> worklist;
      std::set<const BasicBlock *> inQueue;
      worklist.push(&F.getEntryBlock());
      inQueue.insert(&F.getEntryBlock());

      while (!worklist.empty()) {
        const BasicBlock *bb = worklist.front();
        worklist.pop();
        inQueue.erase(bb);

        std::unordered_map<uint32_t, ReachingDefSet> current;
        if (bb == &F.getEntryBlock()) {
          for (const auto &[memReg, formalIn] : formalInByReg)
            current[memReg].insert(formalIn);
        } else {
          for (const BasicBlock *pred : predecessors(bb)) {
            auto predIt = out.find(pred);
            if (predIt == out.end())
              continue;
            for (const auto &pair : predIt->second) {
              const uint32_t memReg = pair.first;
              auto phiIt = bbToMemPhi[bb].find(memReg);
              if (phiIt != bbToMemPhi[bb].end()) {
                current[memReg] = {svfg->getNode(phiIt->second)};
                continue;
              }
              current[memReg].insert(pair.second.begin(), pair.second.end());
            }
          }
        }

        auto phiMapIt = bbToMemPhi.find(bb);
        if (phiMapIt != bbToMemPhi.end()) {
          for (const auto &phiPair : phiMapIt->second)
            current[phiPair.first] = {svfg->getNode(phiPair.second)};
        }

        auto localIt = localDefs.find(bb);
        if (localIt != localDefs.end()) {
          for (const auto &pair : localIt->second)
            current[pair.first] = {pair.second};
        }

        const bool changed = (out.find(bb) == out.end()) || out[bb] != current;
        if (!changed)
          continue;

        out[bb] = std::move(current);
        for (const BasicBlock *succ : successors(bb)) {
          if (inQueue.insert(succ).second)
            worklist.push(succ);
        }
      }
    };

    bool changed = true;
    while (changed) {
      changed = false;

      BlockReachingDefs exitDefs;
      computeExitDefs(exitDefs);

      for (const BasicBlock &bb : F) {
        const unsigned numPreds = std::distance(pred_begin(&bb), pred_end(&bb));
        if (numPreds < 2)
          continue;

        for (uint32_t memReg : memRegsWithDefs) {
          if (bbToMemPhi[&bb].count(memReg))
            continue;

          std::vector<SVFGNode *> perPredDefs;
          std::set<SVFGNode *> distinctDefs;
          for (const BasicBlock *pred : predecessors(&bb)) {
            ReachingDefSet incomingDefs;
            auto predIt = exitDefs.find(pred);
            if (predIt != exitDefs.end()) {
              auto defIt = predIt->second.find(memReg);
              if (defIt != predIt->second.end())
                incomingDefs = defIt->second;
            }
            if (incomingDefs.empty()) {
              auto entryIt = formalInByReg.find(memReg);
              if (entryIt != formalInByReg.end())
                incomingDefs.insert(entryIt->second);
            }
            SVFGNode *incomingDef = nullptr;
            for (SVFGNode *candidate : incomingDefs)
              if (!incomingDef || candidate->getId() < incomingDef->getId())
                incomingDef = candidate;
            perPredDefs.push_back(incomingDef);
            distinctDefs.insert(incomingDefs.begin(), incomingDefs.end());
          }

          if (distinctDefs.size() < 2)
            continue;

          const uint32_t phiNodeId = createMemoryPHI(memReg, &bb);
          auto *phiNode =
              dyn_cast<IntraMSSAPhiSVFGNode>(svfg->getNode(phiNodeId));
          if (!phiNode)
            continue;

          if (phiNode->getDefSVFVars().empty()) {
            if (SVFGNode *seed = *distinctDefs.begin()) {
              if (auto *seedMem = dyn_cast<MSSASVFGNode>(seed)) {
                if (auto *phiPts =
                        const_cast<SVFGNodeBS *>(phiNode->getPointsTo())) {
                  *phiPts = seedMem->getDefSVFVars();
                }
              }
            }
          }

          uint32_t predIdx = 0;
          for (SVFGNode *incomingDef : perPredDefs) {
            if (!incomingDef) {
              ++predIdx;
              continue;
            }
            uint32_t defReg = 0;
            uint32_t defVersion = 0;
            SVFGNodeBS defPts;
            if (!getDefInfo(incomingDef, defReg, defVersion, defPts)) {
              ++predIdx;
              continue;
            }
            SVFGNodeBS edgePts = intersectPointsToSets(
                defPts, phiNode->getDefSVFVars(), getOrCreateUnknownObjId());
            if (edgePts.empty())
              edgePts = phiNode->getDefSVFVars();
            if (edgePts.empty())
              edgePts = defPts;
            if (edgePts.empty())
              edgePts.insert(getOrCreateUnknownObjId());
            svfg->addEdge(incomingDef, phiNode, SVFGEdgeK::IntraIndirect,
                          nullptr, edgePts);
            phiNode->setOpVer(predIdx, defReg, defVersion);
            ++predIdx;
          }

          changed = true;
        }
      }
    }
  }
}

void SVFGBuilder::buildInterproceduralMemoryPHINodes() {
  // DISABLED – this function used to insert InterMSSAPhiSVFGNode nodes
  // between FormalIn and FormalOut (and between ActualIn and ActualOut),
  // which is INCORRECT:
  //
  //   FormalIn → [InterPhi] → FormalOut
  //
  // FormalIn represents the memory state AT ENTRY; FormalOut represents the
  // state AT EXIT after all stores in the body.  Inserting a synthetic
  // inter-procedural PHI between them would bypass all intra-procedural
  // StoreChi/PHI nodes and create direct def-use edges that skip the actual
  // program flow, producing spurious value-flow paths.
  //
  // The correct inter-procedural memory flow is already established by:
  //   • buildCallEdges:   ActualIn  → FormalIn  (CallAIn edge)
  //   • buildReturnEdges: FormalOut → ActualOut (RetAOut edge)
  //   • connectMemorySSAEdges: stores/loads connected inside each function
  //
  // If you need to model a callee that passes memory through unchanged, that
  // is correctly handled by having no StoreChi in the callee body, which
  // means the reaching-def chain continues from FormalIn through FormalOut
  // without any additional node.  No synthetic PHI is needed.
  return;
}

void SVFGBuilder::connectMemorySSAEdges() {
  if (!config.buildMSSA)
    return;

  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  auto getLoadStmtNode = [&](const llvm::LoadInst *li) -> SVFGNode * {
    if (!li)
      return nullptr;
    auto it = loadToLoadNode.find(li);
    return (it != loadToLoadNode.end()) ? svfg->getNode(it->second) : nullptr;
  };

  auto getStoreStmtNode = [&](const llvm::StoreInst *si) -> SVFGNode * {
    if (!si)
      return nullptr;
    auto it = storeToStoreNode.find(si);
    return (it != storeToStoreNode.end()) ? svfg->getNode(it->second) : nullptr;
  };

  auto getDefInfo = [&](SVFGNode *node, uint32_t &memReg, uint32_t &version,
                        SVFGNodeBS &pts) -> bool {
    if (!node)
      return false;
    if (auto *store = dyn_cast<StoreSVFGNode>(node)) {
      memReg = store->getMemoryDefReg();
      version = store->getMemoryDefVersion();
      pts = store->getMemoryPointsTo();
      return memReg != 0;
    }
    if (auto *mem = dyn_cast<MSSASVFGNode>(node)) {
      if (!isMemDefSVFGNode(mem->getNodeKind()) &&
          mem->getNodeKind() != SVFGK::MIntraPhi)
        return false;
      memReg = mem->getMemReg();
      version = mem->getSSAVersion();
      pts = mem->getDefSVFVars();
      return memReg != 0;
    }
    return false;
  };

  auto buildGuard = [&](const SVFGNodeBS &lhs, const SVFGNodeBS &rhs) {
    SVFGNodeBS pts = intersectPointsToSets(lhs, rhs, getOrCreateUnknownObjId());
    if (!pts.empty())
      return pts;
    if (!rhs.empty())
      return rhs;
    if (!lhs.empty())
      return lhs;
    return SVFGNodeBS{getOrCreateUnknownObjId()};
  };

  std::unordered_map<const Function *, std::unordered_map<uint32_t, SVFGNode *>>
      funcEntryChiMap;

  // First pass: collect function-entry defs.
  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    for (uint32_t formalInId : funcEntryChi[&F]) {
      SVFGNode *formalInNode = svfg->getNode(formalInId);
      if (auto *formalIn = dyn_cast<FormalInSVFGNode>(formalInNode)) {
        uint32_t memReg = formalIn->getMemReg();
        funcEntryChiMap[&F][memReg] = formalInNode;
      }
    }
  }

  // Track last def per memory region per function for exit summary building.
  std::unordered_map<
      const Function *,
      std::unordered_map<const BasicBlock *,
                         std::unordered_map<uint32_t, SVFGNode *>>>
      lastDefAtBlockMap;

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    // Track last def per memory region as we traverse blocks
    std::unordered_map<const BasicBlock *,
                       std::unordered_map<uint32_t, SVFGNode *>>
        &lastDefAtBlock = lastDefAtBlockMap[&F];

    // Initialize entry block with EntryChi nodes
    for (auto &pair : funcEntryChiMap[&F]) {
      lastDefAtBlock[&F.getEntryBlock()][pair.first] = pair.second;
    }

    std::queue<const BasicBlock *> worklist;
    std::set<const BasicBlock *> inQueue;
    worklist.push(&F.getEntryBlock());
    inQueue.insert(&F.getEntryBlock());

    while (!worklist.empty()) {
      const BasicBlock *bb = worklist.front();
      worklist.pop();
      inQueue.erase(bb);

      std::unordered_map<uint32_t, SVFGNode *> lastDef;
      if (bb == &F.getEntryBlock()) {
        for (auto &pair : funcEntryChiMap[&F]) {
          lastDef[pair.first] = pair.second;
        }
      } else {
        for (const BasicBlock *pred : predecessors(bb)) {
          auto predDefsIt = lastDefAtBlock.find(pred);
          if (predDefsIt == lastDefAtBlock.end())
            continue;
          for (auto &pair : predDefsIt->second) {
            uint32_t memReg = pair.first;
            SVFGNode *def = pair.second;
            auto phiIt = bbToMemPhi[bb].find(memReg);
            if (phiIt != bbToMemPhi[bb].end()) {
              lastDef[memReg] = svfg->getNode(phiIt->second);
              continue;
            }
            if (lastDef.find(memReg) == lastDef.end())
              lastDef[memReg] = def;
          }
        }
      }

      for (const Instruction &inst : *bb) {
        if (const LoadInst *load = dyn_cast<LoadInst>(&inst)) {
          auto *loadNode =
              dyn_cast_or_null<LoadSVFGNode>(getLoadStmtNode(load));
          if (!loadNode || loadNode->getMemoryUseReg() == 0)
            continue;
          const uint32_t memReg = loadNode->getMemoryUseReg();
          SVFGNode *reachingDef = nullptr;
          auto phiIt = bbToMemPhi[bb].find(memReg);
          if (phiIt != bbToMemPhi[bb].end()) {
            reachingDef = svfg->getNode(phiIt->second);
          } else {
            auto defIt = lastDef.find(memReg);
            if (defIt != lastDef.end() && defIt->second) {
              reachingDef = defIt->second;
            } else {
              auto entryIt = funcEntryChiMap[&F].find(memReg);
              if (entryIt != funcEntryChiMap[&F].end())
                reachingDef = entryIt->second;
            }
          }

          if (!reachingDef)
            continue;
          uint32_t defReg = 0;
          uint32_t defVersion = 0;
          SVFGNodeBS defPts;
          if (!getDefInfo(reachingDef, defReg, defVersion, defPts))
            continue;
          loadNode->setMemoryUse(memReg, defVersion,
                                 loadNode->getMemoryPointsTo());
          svfg->addEdge(reachingDef, loadNode, SVFGEdgeK::IntraIndirect,
                        nullptr,
                        buildGuard(defPts, loadNode->getMemoryPointsTo()));
        }

        if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
          auto *storeNode = dyn_cast<StoreSVFGNode>(getStoreStmtNode(store));
          if (!storeNode || storeNode->getMemoryDefReg() == 0)
            continue;
          const uint32_t memReg = storeNode->getMemoryDefReg();
          auto prevIt = lastDef.find(memReg);
          if (prevIt != lastDef.end() && prevIt->second &&
              prevIt->second != storeNode) {
            uint32_t prevReg = 0;
            uint32_t prevVersion = 0;
            SVFGNodeBS prevPts;
            if (getDefInfo(prevIt->second, prevReg, prevVersion, prevPts)) {
              svfg->addEdge(
                  prevIt->second, storeNode, SVFGEdgeK::IntraIndirect, nullptr,
                  buildGuard(prevPts, storeNode->getMemoryPointsTo()));
            }
          }
          lastDef[memReg] = storeNode;
        }

        if (const auto *call = dyn_cast<CallBase>(&inst)) {
          auto muIt = callToMuNodes.find(call);
          auto chiIt = callToChiNodes.find(call);
          if (muIt != callToMuNodes.end() || chiIt != callToChiNodes.end()) {
            std::unordered_map<uint32_t, SVFGNode *> actualInByReg;
            std::unordered_map<uint32_t, SVFGNode *> actualOutByReg;

            if (muIt != callToMuNodes.end()) {
              for (uint32_t muId : muIt->second) {
                SVFGNode *muNode = svfg->getNode(muId);
                if (auto *actualIn = dyn_cast<ActualInSVFGNode>(muNode)) {
                  actualInByReg[actualIn->getMemReg()] = muNode;
                }
              }
            }
            if (chiIt != callToChiNodes.end()) {
              for (uint32_t chiId : chiIt->second) {
                SVFGNode *chiNode = svfg->getNode(chiId);
                if (auto *actualOut = dyn_cast<ActualOutSVFGNode>(chiNode)) {
                  actualOutByReg[actualOut->getMemReg()] = chiNode;
                }
              }
            }

            std::set<uint32_t> touchedRegs;
            for (const auto &pair : actualInByReg)
              touchedRegs.insert(pair.first);
            for (const auto &pair : actualOutByReg)
              touchedRegs.insert(pair.first);

            for (uint32_t memReg : touchedRegs) {
              SVFGNode *actualInNode = nullptr;
              SVFGNode *actualOutNode = nullptr;
              auto muNodeIt = actualInByReg.find(memReg);
              if (muNodeIt != actualInByReg.end())
                actualInNode = muNodeIt->second;
              auto chiNodeIt = actualOutByReg.find(memReg);
              if (chiNodeIt != actualOutByReg.end())
                actualOutNode = chiNodeIt->second;

              auto defIt = lastDef.find(memReg);
              SVFGNode *reachingDef =
                  (defIt != lastDef.end()) ? defIt->second : nullptr;

              if (actualInNode && reachingDef) {
                if (auto *actualIn = dyn_cast<ActualInSVFGNode>(actualInNode)) {
                  uint32_t defReg = 0;
                  uint32_t defVersion = 0;
                  SVFGNodeBS defPts;
                  if (getDefInfo(reachingDef, defReg, defVersion, defPts)) {
                    actualIn->setSSAVersion(defVersion);
                    svfg->addEdge(
                        reachingDef, actualInNode, SVFGEdgeK::IntraIndirect,
                        nullptr, buildGuard(defPts, actualIn->getDefSVFVars()));
                  }
                }
              }

              if (actualOutNode)
                lastDef[memReg] = actualOutNode;
            }
          }
        }
      }

      auto &prevOut = lastDefAtBlock[bb];
      const bool changed = prevOut != lastDef;
      if (changed) {
        prevOut = lastDef;
      }

      if (changed) {
        for (const BasicBlock *succ : successors(bb)) {
          if (inQueue.insert(succ).second) {
            worklist.push(succ);
          }
        }
      } else if (bb == &F.getEntryBlock()) {
        for (const BasicBlock *succ : successors(bb)) {
          if (inQueue.insert(succ).second) {
            worklist.push(succ);
          }
        }
      }
    }
  }

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    auto funcDefsIt = lastDefAtBlockMap.find(&F);
    if (funcDefsIt == lastDefAtBlockMap.end())
      continue;
    const auto &lastDefAtBlock = funcDefsIt->second;

    std::vector<const BasicBlock *> returnBlocks;
    for (const BasicBlock &bb : F) {
      if (isa<ReturnInst>(bb.getTerminator()))
        returnBlocks.push_back(&bb);
    }

    for (auto *formalOut : svfg->getFormalOuts(&F)) {
      if (auto *formalOutMem = dyn_cast<FormalOutSVFGNode>(formalOut)) {
        uint32_t memReg = formalOutMem->getMemReg();
        std::set<SVFGNode *> exitDefs;

        if (!returnBlocks.empty()) {
          for (const BasicBlock *retBB : returnBlocks) {
            auto defsIt = lastDefAtBlock.find(retBB);
            if (defsIt != lastDefAtBlock.end()) {
              auto memDefIt = defsIt->second.find(memReg);
              if (memDefIt != defsIt->second.end() && memDefIt->second)
                exitDefs.insert(memDefIt->second);
            }
          }
        }

        if (exitDefs.empty()) {
          auto entryIt = funcEntryChiMap.find(&F);
          if (entryIt != funcEntryChiMap.end()) {
            auto memEntryIt = entryIt->second.find(memReg);
            if (memEntryIt != entryIt->second.end())
              exitDefs.insert(memEntryIt->second);
          }
        }

        if (exitDefs.empty())
          continue;

        SVFGNode *formalOutDef = nullptr;
        if (exitDefs.size() == 1) {
          formalOutDef = *exitDefs.begin();
        } else {
          const uint32_t exitPhiId = nextNode();
          const uint32_t exitPhiVersion = nextVersion(&F, memReg);
          auto *exitPhi = new IntraMSSAPhiSVFGNode(
              exitPhiId, findFunctionExitICFGNode(icfg, &F), memReg,
              exitPhiVersion, formalOutMem->getDefSVFVars());
          svfg->addNode(exitPhi);
          svfg->setMSSADef(memReg, exitPhi, exitPhiVersion);
          uint32_t predIdx = 0;
          for (SVFGNode *exitDef : exitDefs) {
            uint32_t defReg = 0;
            uint32_t defVersion = 0;
            SVFGNodeBS defPts;
            if (!getDefInfo(exitDef, defReg, defVersion, defPts))
              continue;
            exitPhi->setOpVer(predIdx++, defReg, defVersion);
            svfg->addEdge(exitDef, exitPhi, SVFGEdgeK::IntraIndirect, nullptr,
                          buildGuard(defPts, exitPhi->getDefSVFVars()));
          }
          formalOutDef = exitPhi;
        }

        uint32_t defReg = 0;
        uint32_t defVersion = 0;
        SVFGNodeBS defPts;
        if (!getDefInfo(formalOutDef, defReg, defVersion, defPts))
          continue;
        formalOutMem->setSSAVersion(defVersion);
        svfg->addEdge(formalOutDef, formalOutMem, SVFGEdgeK::IntraIndirect,
                      nullptr,
                      buildGuard(defPts, formalOutMem->getDefSVFVars()));
      }
    }
  }
}

void SVFGBuilder::buildInterproceduralEdges() {
  buildCallEdges();
  buildReturnEdges();
}

void SVFGBuilder::buildCallEdges() {
  for (auto &pair : *icfg) {
    auto *blockNode = dyn_cast<IntraBlockNode>(pair.second);
    if (!blockNode || !blockNode->getBasicBlock())
      continue;

    for (const Instruction &inst : *blockNode->getBasicBlock()) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;

      std::vector<const Function *> callees;
      const Function *directCallee = call->getCalledFunction();
      if (directCallee) {
        if (!directCallee->isDeclaration())
          callees.push_back(directCallee);
      } else if (config.usePointerAnalysis && ptaSolverWrapper &&
                 ptaSolverWrapper->solver && config.resolveIndirectCalls) {
        callees = getIndirectCallTargets(call);
        callees = filterCalleesByICFG(icfg, call, callees);
      }

      for (const Function *callee : callees)
        ensureICFGInterEdges(icfg, call, callee);

      for (SVFGNode *actualNode : svfg->getActualParms(call)) {
        auto *actualParm = dyn_cast<ActualParmSVFGNode>(actualNode);
        if (!actualParm)
          continue;
        for (const Function *callee : callees) {
          svfg->markConnectedCallee(call, callee);
          recordRefinedCallEdge(call, callee);

          const unsigned actualIdx = actualParm->getParamIndex();
          const bool isVarArgExtra =
              callee->isVarArg() && actualIdx >= callee->arg_size();

          if (isVarArgExtra) {
            // Connect extra arguments (beyond declared params) to
            // VarArgSVFGNode
            for (SVFGNode *formal : svfg->getFormalParms(callee)) {
              auto *varArgNode = dyn_cast<VarArgSVFGNode>(formal);
              if (!varArgNode)
                continue;
              const bool isDirectEdge =
                  (directCallee && directCallee == callee &&
                   !directCallee->isDeclaration());
              if (SVFGEdge *e = svfg->addEdge(actualParm, varArgNode,
                                              isDirectEdge ? SVFGEdgeK::CallDir
                                                           : SVFGEdgeK::CallInd,
                                              call)) {
                if (!isDirectEdge)
                  vfEdgesAtIndCallSite.insert(e);
              }
            }
          } else {
            // Normal parameter matching
            for (SVFGNode *formal : svfg->getFormalParms(callee)) {
              auto *formalParm = dyn_cast<FormalParmSVFGNode>(formal);
              if (!formalParm)
                continue;
              if (formalParm->getParamIndex() != actualIdx)
                continue;
              // Use only CallDir/CallInd – ParamCall is a duplicate and is
              // removed to avoid confusing DDA clients that pattern-match on
              // edge kind.  (SVF uses a single CallDirVF/CallIndVF per pair.)
              const bool isDirectEdge =
                  (directCallee && directCallee == callee &&
                   !directCallee->isDeclaration());
              if (SVFGEdge *e = svfg->addEdge(actualParm, formalParm,
                                              isDirectEdge ? SVFGEdgeK::CallDir
                                                           : SVFGEdgeK::CallInd,
                                              call)) {
                // Track pre-computed indirect edges for spurious-edge
                // filtering.
                if (!isDirectEdge)
                  vfEdgesAtIndCallSite.insert(e);
              }
            }
          }
        }
      }

      for (SVFGNode *actualNode : svfg->getActualIns(call)) {
        auto *actualIn = dyn_cast<ActualInSVFGNode>(actualNode);
        if (!actualIn)
          continue;
        for (const Function *callee : callees) {
          svfg->markConnectedCallee(call, callee);
          recordRefinedCallEdge(call, callee);
          for (SVFGNode *formal : svfg->getFormalIns(callee)) {
            auto *formalIn = dyn_cast<FormalInSVFGNode>(formal);
            if (!formalIn)
              continue;
            if (!mayAliasMemoryNodes(actualIn, formalIn))
              continue;
            SVFGNodeBS edgePts =
                intersectPointsToSets(actualIn->getDefSVFVars(),
                                      formalIn->getDefSVFVars(), unknownObjId);
            if (edgePts.empty())
              continue;
            if (SVFGEdge *e = svfg->addEdge(
                    actualIn, formalIn, SVFGEdgeK::CallAIn, call, edgePts)) {
              if (!directCallee)
                vfEdgesAtIndCallSite.insert(e);
            }
          }
        }
      }
    }
  }
}

void SVFGBuilder::buildReturnEdges() {
  for (auto &pair : *icfg) {
    auto *blockNode = dyn_cast<IntraBlockNode>(pair.second);
    if (!blockNode || !blockNode->getBasicBlock())
      continue;

    for (const Instruction &inst : *blockNode->getBasicBlock()) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;

      std::vector<const Function *> callees;
      const Function *directCallee = call->getCalledFunction();
      if (directCallee) {
        if (!directCallee->isDeclaration())
          callees.push_back(directCallee);
      } else if (config.usePointerAnalysis && ptaSolverWrapper &&
                 ptaSolverWrapper->solver && config.resolveIndirectCalls) {
        callees = getIndirectCallTargets(call);
        callees = filterCalleesByICFG(icfg, call, callees);
      }

      for (const Function *callee : callees)
        ensureICFGInterEdges(icfg, call, callee);

      for (SVFGNode *actualNode : svfg->getActualRets(call)) {
        auto *actualRet = dyn_cast<ActualRetSVFGNode>(actualNode);
        if (!actualRet)
          continue;
        for (const Function *callee : callees) {
          svfg->markConnectedCallee(call, callee);
          recordRefinedCallEdge(call, callee);
          for (SVFGNode *formal : svfg->getFormalRets(callee)) {
            auto *formalRet = dyn_cast<FormalRetSVFGNode>(formal);
            if (!formalRet)
              continue;
            // Use only RetDir/RetInd – ParamRet is a duplicate (same fix as
            // for ParamCall above).
            const bool isDirectEdge = (directCallee && directCallee == callee &&
                                       !directCallee->isDeclaration());
            if (SVFGEdge *e = svfg->addEdge(formalRet, actualRet,
                                            isDirectEdge ? SVFGEdgeK::RetDir
                                                         : SVFGEdgeK::RetInd,
                                            call)) {
              if (!isDirectEdge)
                vfEdgesAtIndCallSite.insert(e);
            }
          }
        }
      }

      for (SVFGNode *actualNode : svfg->getActualOuts(call)) {
        auto *actualOut = dyn_cast<ActualOutSVFGNode>(actualNode);
        if (!actualOut)
          continue;
        for (const Function *callee : callees) {
          svfg->markConnectedCallee(call, callee);
          recordRefinedCallEdge(call, callee);
          for (SVFGNode *formal : svfg->getFormalOuts(callee)) {
            auto *formalOut = dyn_cast<FormalOutSVFGNode>(formal);
            if (!formalOut)
              continue;
            if (!mayAliasMemoryNodes(formalOut, actualOut))
              continue;
            SVFGNodeBS edgePts =
                intersectPointsToSets(formalOut->getDefSVFVars(),
                                      actualOut->getDefSVFVars(), unknownObjId);
            if (edgePts.empty())
              continue;
            if (SVFGEdge *e = svfg->addEdge(
                    formalOut, actualOut, SVFGEdgeK::RetAOut, call, edgePts)) {
              if (!directCallee)
                vfEdgesAtIndCallSite.insert(e);
            }
          }
        }
      }
    }
  }
}

bool SVFGBuilder::connectCallSiteToCalleeOnTheFly(
    SVFG *g, const CallBase *cs, const Function *callee,
    std::vector<SVFGEdge *> &newEdges) {
  if (!g || !cs || !callee)
    return false;
  if (callee->isDeclaration())
    return false;

  ensureICFGInterEdges(icfg, cs, callee);

  // De-duplicate refinements per (callsite, callee).
  if (!g->markConnectedCallee(cs, callee))
    return false;
  recordRefinedCallEdge(cs, callee);

  const Function *directCallee = cs->getCalledFunction();
  const bool isDirectEdge = (directCallee && directCallee == callee &&
                             !directCallee->isDeclaration());

  bool created = false;

  // ActualParm -> FormalParm (top-level pointers)
  for (SVFGNode *actualNode : g->getActualParms(cs)) {
    auto *actualParm = dyn_cast<ActualParmSVFGNode>(actualNode);
    if (!actualParm)
      continue;

    const unsigned actualIdx = actualParm->getParamIndex();
    const bool isVarArgExtra =
        callee->isVarArg() && actualIdx >= callee->arg_size();

    if (isVarArgExtra) {
      // Connect extra arguments (beyond declared params) to VarArgSVFGNode
      for (SVFGNode *formalNode : g->getFormalParms(callee)) {
        auto *varArgNode = dyn_cast<VarArgSVFGNode>(formalNode);
        if (!varArgNode)
          continue;
        if (SVFGEdge *e = g->addEdge(
                actualParm, varArgNode,
                isDirectEdge ? SVFGEdgeK::CallDir : SVFGEdgeK::CallInd, cs)) {
          newEdges.push_back(e);
          created = true;
          if (!isDirectEdge)
            vfEdgesAtIndCallSite.insert(e);
        }
      }
    } else {
      // Normal parameter matching
      for (SVFGNode *formalNode : g->getFormalParms(callee)) {
        auto *formalParm = dyn_cast<FormalParmSVFGNode>(formalNode);
        if (!formalParm)
          continue;
        if (formalParm->getParamIndex() != actualIdx)
          continue;
        // Emit only CallDir/CallInd – ParamCall is a duplicate (see
        // buildCallEdges).
        if (SVFGEdge *e = g->addEdge(
                actualParm, formalParm,
                isDirectEdge ? SVFGEdgeK::CallDir : SVFGEdgeK::CallInd, cs)) {
          newEdges.push_back(e);
          created = true;
          if (!isDirectEdge)
            vfEdgesAtIndCallSite.insert(e);
        }
      }
    }
  }

  // ActualIn -> FormalIn (memory)
  for (SVFGNode *actualNode : g->getActualIns(cs)) {
    auto *actualIn = dyn_cast<ActualInSVFGNode>(actualNode);
    if (!actualIn)
      continue;
    for (SVFGNode *formalNode : g->getFormalIns(callee)) {
      auto *formalIn = dyn_cast<FormalInSVFGNode>(formalNode);
      if (!formalIn)
        continue;
      if (!mayAliasMemoryNodes(actualIn, formalIn))
        continue;
      SVFGNodeBS edgePts = intersectPointsToSets(
          actualIn->getDefSVFVars(), formalIn->getDefSVFVars(), unknownObjId);
      if (edgePts.empty())
        continue;
      if (SVFGEdge *e =
              g->addEdge(actualIn, formalIn, SVFGEdgeK::CallAIn, cs, edgePts)) {
        newEdges.push_back(e);
        created = true;
        if (!isDirectEdge)
          vfEdgesAtIndCallSite.insert(e);
      }
    }
  }

  // FormalRet -> ActualRet (return values)
  for (SVFGNode *actualNode : g->getActualRets(cs)) {
    auto *actualRet = dyn_cast<ActualRetSVFGNode>(actualNode);
    if (!actualRet)
      continue;
    for (SVFGNode *formalNode : g->getFormalRets(callee)) {
      auto *formalRet = dyn_cast<FormalRetSVFGNode>(formalNode);
      if (!formalRet)
        continue;
      // Emit only RetDir/RetInd – ParamRet is a duplicate (see
      // buildReturnEdges).
      if (SVFGEdge *e = g->addEdge(
              formalRet, actualRet,
              isDirectEdge ? SVFGEdgeK::RetDir : SVFGEdgeK::RetInd, cs)) {
        newEdges.push_back(e);
        created = true;
        if (!isDirectEdge)
          vfEdgesAtIndCallSite.insert(e);
      }
    }
  }

  // FormalOut -> ActualOut (memory)
  for (SVFGNode *actualNode : g->getActualOuts(cs)) {
    auto *actualOut = dyn_cast<ActualOutSVFGNode>(actualNode);
    if (!actualOut)
      continue;
    for (SVFGNode *formalNode : g->getFormalOuts(callee)) {
      auto *formalOut = dyn_cast<FormalOutSVFGNode>(formalNode);
      if (!formalOut)
        continue;
      if (!mayAliasMemoryNodes(formalOut, actualOut))
        continue;
      SVFGNodeBS edgePts = intersectPointsToSets(
          formalOut->getDefSVFVars(), actualOut->getDefSVFVars(), unknownObjId);
      if (edgePts.empty())
        continue;
      if (SVFGEdge *e = g->addEdge(formalOut, actualOut, SVFGEdgeK::RetAOut, cs,
                                   edgePts)) {
        newEdges.push_back(e);
        created = true;
        if (!isDirectEdge)
          vfEdgesAtIndCallSite.insert(e);
      }
    }
  }

  return created;
}

void SVFGBuilder::connectFromGlobalToProgEntry() {
  if (!svfg)
    return;
  ICFG *mutableICFG = const_cast<ICFG *>(icfg);
  const ICFGNode *globalInitNode =
      mutableICFG ? mutableICFG->getGlobalInitICFGNode() : nullptr;
  if (!globalInitNode)
    return;
  std::vector<const Function *> entryFuncs = getRootFunctionsFromICFG();
  if (entryFuncs.empty())
    return;
  if (globalEntryRegions.empty())
    return;

  if (const Function *progEntry = getProgramEntryFunction(icfg)) {
    const auto &formalIns = svfg->getFormalIns(progEntry);
    if (!formalIns.empty() && !svfg->getGlobalStoreNodes().empty()) {
      for (SVFGNode *storeNode : svfg->getGlobalStoreNodes()) {
        auto *store = dyn_cast<StoreSVFGNode>(storeNode);
        if (!store)
          continue;
        SVFGNodeBS storePts = store->getMemoryPointsTo();
        if (storePts.empty())
          storePts.insert(getOrCreateUnknownObjId());
        for (SVFGNode *formalInNode : formalIns) {
          auto *formalIn = dyn_cast_or_null<FormalInSVFGNode>(formalInNode);
          if (!formalIn)
            continue;
          SVFGNodeBS intersectPts = intersectPointsToSets(
              storePts, formalIn->getDefSVFVars(), getOrCreateUnknownObjId());
          if (!intersectPts.empty()) {
            svfg->addEdge(storeNode, formalInNode, SVFGEdgeK::IntraIndirect,
                          nullptr, intersectPts);
          }
        }
      }
      return;
    }
  }

  std::unordered_map<uint32_t, SVFGNode *> memRegToEntryChi;
  for (const auto &entry : globalEntryRegions) {
    const uint32_t memReg = entry.first;
    SVFGNodeBS pts = entry.second;
    if (pts.empty())
      pts.insert(getOrCreateUnknownObjId());
    const uint32_t entryChiId = nextNode();
    auto *entryChi =
        new EntryChiSVFGNode(entryChiId, globalInitNode, nullptr, memReg, pts);
    svfg->addNode(entryChi);
    memRegToEntryChi[memReg] = entryChi;
  }

  for (const Function *entryFunc : entryFuncs) {
    auto entryChiIt = funcEntryChi.find(entryFunc);
    if (entryChiIt == funcEntryChi.end() || entryChiIt->second.empty())
      continue;

    for (uint32_t formalInId : entryChiIt->second) {
      SVFGNode *formalInNode = svfg->getNode(formalInId);
      auto *formalIn = dyn_cast_or_null<FormalInSVFGNode>(formalInNode);
      if (!formalIn)
        continue;
      auto globalIt = memRegToEntryChi.find(formalIn->getMemReg());
      if (globalIt == memRegToEntryChi.end())
        continue;

      SVFGNode *entryChiNode = globalIt->second;
      SVFGNodeBS entryPts = formalIn->getDefSVFVars();
      if (entryPts.empty())
        entryPts.insert(getOrCreateUnknownObjId());
      SVFGNodeBS intersectPts = intersectPointsToSets(
          entryChiNode->getDefSVFVars(), entryPts, getOrCreateUnknownObjId());
      if (!intersectPts.empty()) {
        svfg->addEdge(entryChiNode, formalInNode, SVFGEdgeK::IntraIndirect,
                      nullptr, intersectPts);
      }
    }
  }
}
