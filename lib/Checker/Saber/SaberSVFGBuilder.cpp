//===- SaberSVFGBuilder.cpp -- SVFG builder in Saber-------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
// rmDerefDirSVFGEdges, rmIncomingEdgeForSUStore, AddExtActualParmSVFGNodes
// aligned with SVF semantics using Lotus SVFG APIs.
//
//===----------------------------------------------------------------------===//

#include "Checker/Saber/SaberSVFGBuilder.h"

#include "Checker/Saber/SaberCheckerAPI.h"
#include "Checker/Saber/SaberCondAllocator.h"
#include "Checker/Saber/SaberOptions.h"
#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>

using namespace lotus::analysis;
using namespace llvm;

namespace {

using FuncSet = std::unordered_set<const Function *>;
using FuncGraph = std::unordered_map<const Function *, FuncSet>;

static void addDefinedFunction(FuncGraph &graph, const Function *F) {
  if (F && !F->isDeclaration())
    graph.emplace(F, FuncSet{});
}

static void appendUniqueDefinedCallee(std::vector<const Function *> &targets,
                                      const Function *F) {
  if (!F || F->isDeclaration())
    return;
  if (std::find(targets.begin(), targets.end(), F) == targets.end())
    targets.push_back(F);
}

static const Function *getDirectCallee(const CallBase *call) {
  if (!call)
    return nullptr;
  if (const Function *callee = call->getCalledFunction())
    return callee;
  const Value *called = call->getCalledOperand();
  if (!called)
    return nullptr;
  return dyn_cast<Function>(called->stripPointerCasts());
}

static const ICFGNode *getCallSiteICFGNode(const SVFG *g, const CallBase *call) {
  if (!g || !call)
    return nullptr;
  if (const ICFG *icfg = g->getICFG()) {
    return const_cast<ICFG *>(icfg)->getIntraBlockNode(call->getParent());
  }
  return nullptr;
}

static ActualParmSVFGNode *findActualParmNode(SVFG *g, const CallBase *cs,
                                              unsigned idx) {
  if (!g || !cs)
    return nullptr;
  const auto &parms = g->getActualParms(cs);
  for (SVFGNode *n : parms) {
    auto *ap = dyn_cast<ActualParmSVFGNode>(n);
    if (ap && ap->getParamIndex() == idx)
      return ap;
  }
  return nullptr;
}

static void buildResolvedCallGraph(
    const Module *M, FuncGraph &graph,
    const std::function<std::vector<const Function *>(const CallBase *)>
        &resolveIndirectTargets) {
  if (!M)
    return;

  for (const Function &F : *M)
    addDefinedFunction(graph, &F);

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;
    auto it = graph.find(&F);
    if (it == graph.end())
      continue;

    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        const auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;
        const Function *callee = getDirectCallee(CB);
        if (callee) {
          if (!callee->isDeclaration())
            it->second.insert(callee);
          continue;
        }
        for (const Function *target : resolveIndirectTargets(CB)) {
          if (target && !target->isDeclaration())
            it->second.insert(target);
        }
      }
    }
  }
}

static void buildResolvedCallGraph(const LTCallGraph *callGraph,
                                   FuncGraph &graph) {
  if (!callGraph)
    return;

  for (const auto &entry : *callGraph)
    addDefinedFunction(graph, entry.first);

  for (const auto &entry : *callGraph) {
    const Function *caller = entry.first;
    const auto *callerNode = entry.second.get();
    if (!caller || !callerNode)
      continue;

    auto git = graph.find(caller);
    if (git == graph.end())
      continue;

    for (const auto &record : *callerNode) {
      const auto *calleeNode = record.second;
      const Function *callee = calleeNode ? calleeNode->getFunction() : nullptr;
      if (callee && !callee->isDeclaration())
        git->second.insert(callee);
    }
  }
}

static std::unordered_set<const Function *>
computeRecursiveFunctions(const FuncGraph &graph) {
  std::unordered_map<const Function *, int> index;
  std::unordered_map<const Function *, int> lowlink;
  std::unordered_set<const Function *> onStack;
  std::vector<const Function *> stack;
  std::unordered_set<const Function *> recursive;
  int nextIndex = 0;

  std::function<void(const Function *)> strongConnect = [&](const Function *v) {
    index[v] = nextIndex;
    lowlink[v] = nextIndex;
    ++nextIndex;
    stack.push_back(v);
    onStack.insert(v);

    auto git = graph.find(v);
    if (git != graph.end()) {
      for (const Function *w : git->second) {
        if (!w)
          continue;
        if (index.find(w) == index.end()) {
          strongConnect(w);
          lowlink[v] = std::min(lowlink[v], lowlink[w]);
        } else if (onStack.count(w)) {
          lowlink[v] = std::min(lowlink[v], index[w]);
        }
      }
    }

    if (lowlink[v] != index[v])
      return;

    std::vector<const Function *> scc;
    while (!stack.empty()) {
      const Function *w = stack.back();
      stack.pop_back();
      onStack.erase(w);
      scc.push_back(w);
      if (w == v)
        break;
    }

    if (scc.size() > 1) {
      recursive.insert(scc.begin(), scc.end());
      return;
    }

    const Function *f = scc.front();
    auto it = graph.find(f);
    if (it != graph.end() && it->second.count(f))
      recursive.insert(f);
  };

  for (const auto &kv : graph) {
    const Function *F = kv.first;
    if (index.find(F) == index.end())
      strongConnect(F);
  }

  return recursive;
}

static const Function *getOwningFunctionForObjectValue(const Value *value) {
  if (!value)
    return nullptr;

  if (const auto *inst = dyn_cast<Instruction>(value))
    return inst->getFunction();
  if (const auto *arg = dyn_cast<Argument>(value))
    return arg->getParent();
  if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
    if (ce->isCast())
      return getOwningFunctionForObjectValue(ce->getOperand(0));
    if (ce->getOpcode() == Instruction::GetElementPtr)
      return getOwningFunctionForObjectValue(ce->getOperand(0));
  }

  return nullptr;
}

} // namespace

SVFG *SaberSVFGBuilder::buildSVFG(const ICFG *icfg) {
  SVFGBuilderConfig cfg;
  cfg.resolveIndirectCalls = true;
  cfg.buildMSSA = true;
  SVFG *svfg = build(icfg, cfg);
  currentSVFG_ = svfg;

  if (svfg) {
    collectGlobals();
    rmDerefDirSVFGEdges();
    rmIncomingEdgeForSUStore();
    AddExtActualParmSVFGNodes();
  }

  return svfg;
}

std::unique_ptr<SVFG>
SaberSVFGBuilder::buildCompatSVFGForSaber(std::unique_ptr<SVFG> graph) {
  if (!graph)
    return nullptr;

  auto optimized = std::make_unique<SVFGOPT>();
  if (!optimized->adoptAndOptimize(std::move(graph)))
    return nullptr;

  currentSVFG_ = optimized.get();
  // Rebuild sink-specific callsite parameter nodes that generic SVFGOPT
  // intentionally discards but SABER relies on for per-call sink identity.
  AddExtActualParmSVFGNodes();
  return optimized;
}

std::unique_ptr<SVFG> SaberSVFGBuilder::buildForSaber(const ICFG *icfg,
                                                      bool fullSVFG) {
  SVFGBuilderConfig cfg;
  cfg.resolveIndirectCalls = true;
  cfg.buildMSSA = true;

  std::unique_ptr<SVFG> built(build(icfg, cfg));
  if (!built)
    return nullptr;

  if (fullSVFG) {
    currentSVFG_ = built.get();
    return built;
  }

  // SABER defaults to a compact compatibility graph, but it must preserve
  // per-call sink modeling rather than inheriting generic SVFGOPT semantics.
  return buildCompatSVFGForSaber(std::move(built));
}

void SaberSVFGBuilder::reset() {
  globs.clear();
  globSVFGNodes.clear();
  currentSVFG_ = nullptr;
  recursiveFunctionsCache_.clear();
  recursiveFunctionsReady_ = false;
  module_ = nullptr;
  saberCondAllocator = nullptr;
}

static const Value *getPointerOperandForStmt(const SVFGNode *node) {
  const Instruction *inst = node->getInstruction();
  if (!inst)
    return nullptr;
  if (const auto *si = dyn_cast<StoreInst>(inst))
    return si->getPointerOperand();
  if (const auto *li = dyn_cast<LoadInst>(inst))
    return li->getPointerOperand();
  return nullptr;
}

void SaberSVFGBuilder::rmDerefDirSVFGEdges() {
  if (!currentSVFG_)
    return;
  SVFG *g = currentSVFG_;
  for (auto it = g->begin(), et = g->end(); it != et; ++it) {
    SVFGNode *node = it->second;
    if (!node)
      continue;
    SVFGK k = node->getNodeKind();
    if (k != SVFGK::Store && k != SVFGK::Load)
      continue;

    const Value *ptr = getPointerOperandForStmt(node);
    if (!ptr)
      continue;
    SVFGNode *defNode = g->getValueNode(ptr);
    if (!defNode)
      continue;

    // Match SVF SABER semantics: remove only the direct def->deref edge.
    if (SVFGEdge *edge =
            g->getIntraVFGEdge(defNode, node, SVFGEdgeK::IntraDirect)) {
      g->removeEdge(edge);
    }
    if (accessGlobal(ptr))
      globSVFGNodes.insert(node);
  }
}

void SaberSVFGBuilder::rmIncomingEdgeForSUStore() {
  if (!currentSVFG_ || !saberCondAllocator)
    return;
  SVFG *g = currentSVFG_;
  auto &removed = saberCondAllocator->getRemovedSUVFEdges();
  for (auto it = g->begin(), et = g->end(); it != et; ++it) {
    SVFGNode *node = it->second;
    if (!node || node->getNodeKind() != SVFGK::Store)
      continue;
    uint32_t singleton = 0;
    if (!isStrongUpdate(node, singleton))
      continue;

    std::vector<SVFGEdge *> toRemove;
    for (SVFGEdge *e : node->getInEdges()) {
      if (isIndirectVFGEdge(e->getEdgeKind()))
        toRemove.push_back(e);
    }
    for (SVFGEdge *edge : toRemove) {
      SVFGNode *src = edge->getSrcNode();
      if (src && src->getNodeKind() == SVFGK::Store)
        removed[src].insert(node);
      g->removeEdge(edge);
    }
  }
}

void SaberSVFGBuilder::AddExtActualParmSVFGNodes() {
  if (!currentSVFG_ || !module_)
    return;
  SVFG *g = currentSVFG_;
  SaberCheckerAPI *api = SaberCheckerAPI::getCheckerAPI();
  for (Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *cs = dyn_cast<CallBase>(&I);
        if (!cs)
          continue;
        bool sinkLikeCallee = false;
        if (const Function *callee = getDirectCallee(cs)) {
          sinkLikeCallee = api->isMemDealloc(callee->getName().str()) ||
                           api->isFClose(callee->getName().str());
        } else {
          for (const Function *c : getIndirectCallTargets(cs)) {
            if (c && (api->isMemDealloc(c->getName().str()) ||
                      api->isFClose(c->getName().str()))) {
              sinkLikeCallee = true;
              break;
            }
          }
        }
        if (!sinkLikeCallee)
          continue;
        unsigned idx = 0;
        for (auto it = cs->arg_begin(), et = cs->arg_end(); it != et;
             ++it, ++idx) {
          Value *arg = it->get();
          if (!arg->getType()->isPointerTy())
            continue;
          SVFGNode *defNode = g->getValueNode(arg);
          if (!defNode) {
            if (const auto *inst = dyn_cast<Instruction>(arg))
              defNode = g->getDef(inst);
          }
          if (!defNode)
            continue;

          ActualParmSVFGNode *actualParmNode = findActualParmNode(g, cs, idx);
          if (!actualParmNode) {
            uint32_t nodeId = g->getNextNodeId();
            actualParmNode = new ActualParmSVFGNode(
                nodeId, getCallSiteICFGNode(g, cs), cs, idx, arg);
            if (defNode->hasValueId())
              actualParmNode->setValueId(defNode->getValueId());
            g->addNode(actualParmNode);
            g->addActualParm(cs, actualParmNode);
          }

          if (SVFGEdge *copyEdge =
                  g->getIntraVFGEdge(defNode, actualParmNode, SVFGEdgeK::IntraCopy)) {
            g->removeEdge(copyEdge);
          }
          if (!g->getIntraVFGEdge(defNode, actualParmNode,
                                  SVFGEdgeK::IntraDirect)) {
            g->addEdge(defNode, actualParmNode, SVFGEdgeK::IntraDirect);
          }
        }
      }
    }
  }
}

static uint32_t getBaseObjId(const SVFG *g, uint32_t objId) {
  if (!g || objId == 0)
    return 0;
  if (const auto *info = g->getObjectInfo(objId)) {
    if (info->baseObjId != 0)
      return info->baseObjId;
  }
  return objId;
}

static void collectBaseObjectFields(const SVFG *g, uint32_t baseId,
                                    std::set<uint32_t> &out) {
  if (!g || baseId == 0)
    return;
  out.insert(baseId);
  for (const auto &entry : g->getObjectDebugMap()) {
    uint32_t objId = entry.first;
    const auto *info = g->getObjectInfo(objId);
    if (!info)
      continue;
    if (info->baseObjId == baseId)
      out.insert(objId);
  }
}

static bool isExternalRetObject(const SVFG *g, uint32_t objId) {
  if (!g || objId == 0)
    return false;
  const llvm::Value *v = g->getObjectValue(objId);
  const auto *inst = llvm::dyn_cast<llvm::Instruction>(v);
  if (!inst)
    return false;
  const auto *cs = llvm::dyn_cast<llvm::CallBase>(inst);
  if (!cs)
    return false;
  const llvm::Function *callee = cs->getCalledFunction();
  return callee && callee->isDeclaration();
}

SVFGNodeBS SaberSVFGBuilder::getPointsToOfObject(uint32_t objId) {
  SVFGNodeBS result;
  if (!currentSVFG_ || objId == 0)
    return result;
  const llvm::Value *v = currentSVFG_->getObjectValue(objId);
  if (!v || !v->getType()->isPointerTy())
    return result;
  // When collectExtRetGlobals is false, do not follow points-to from
  // external-return objects (SVF saber-collect-extret-globals: avoids pulling
  // in all malloc-return objects as globals).
  if (!SaberOptions::collectExtRetGlobals()) {
    if (currentSVFG_->isFieldInsensitiveObject(objId) &&
        isExternalRetObject(currentSVFG_, objId))
      return result;
  }
  result = getObjectIdsForValue(v);
  return result;
}

std::set<uint32_t> &
SaberSVFGBuilder::CollectPtsChain(uint32_t id, NodeToPTSSMap &cachedPtsMap) {
  uint32_t baseId = getBaseObjId(currentSVFG_, id);
  auto it = cachedPtsMap.find(baseId);
  if (it != cachedPtsMap.end())
    return it->second;

  std::set<uint32_t> &pts = cachedPtsMap[baseId];
  if (!SaberOptions::collectExtRetGlobals()) {
    if (currentSVFG_ && currentSVFG_->isFieldInsensitiveObject(baseId) &&
        isExternalRetObject(currentSVFG_, baseId))
      return pts;
  }

  collectBaseObjectFields(currentSVFG_, baseId, pts);
  WorkList worklist;
  for (uint32_t objId : pts)
    worklist.push_back(objId);

  while (!worklist.empty()) {
    uint32_t objId = worklist.front();
    worklist.pop_front();
    SVFGNodeBS directPts = getPointsToOfObject(objId);
    for (uint32_t o : directPts) {
      std::set<uint32_t> &sub = CollectPtsChain(o, cachedPtsMap);
      for (uint32_t subId : sub) {
        if (pts.insert(subId).second)
          worklist.push_back(subId);
      }
    }
  }
  return pts;
}

void SaberSVFGBuilder::collectGlobals() {
  globs.clear();
  globSVFGNodes.clear();
  if (!currentSVFG_)
    return;

  std::set<uint32_t> seedGlobals;
  for (const auto &entry : currentSVFG_->getObjectDebugMap()) {
    uint32_t objId = entry.first;
    if (currentSVFG_->isGlobalObject(objId))
      seedGlobals.insert(objId);
  }

  // Also seed from global LLVM values (in case they are not yet in the graph).
  if (module_) {
    for (const llvm::GlobalVariable &g : module_->globals()) {
      uint32_t objId = currentSVFG_->getObjectId(&g);
      if (objId != 0 && currentSVFG_->isGlobalObject(objId))
        seedGlobals.insert(objId);
    }
  }

  NodeToPTSSMap cachedPtsMap;
  for (uint32_t id : seedGlobals) {
    std::set<uint32_t> &chain = CollectPtsChain(id, cachedPtsMap);
    globs.insert(chain.begin(), chain.end());
  }
}

void SaberSVFGBuilder::recomputeGlobalSVFGNodes() {
  globSVFGNodes.clear();
  if (!currentSVFG_)
    return;

  for (auto it = currentSVFG_->begin(), et = currentSVFG_->end(); it != et;
       ++it) {
    SVFGNode *node = it->second;
    if (!node)
      continue;
    const SVFGK k = node->getNodeKind();
    if (k == SVFGK::EntryChi) {
      bool touchesGlobal = false;
      for (uint32_t objId : node->getDefSVFVars()) {
        if (globs.count(objId) != 0) {
          touchesGlobal = true;
          break;
        }
      }
      if (touchesGlobal)
        globSVFGNodes.insert(node);
      continue;
    }
    if (k != SVFGK::Store && k != SVFGK::Load)
      continue;
    const Value *ptr = getPointerOperandForStmt(node);
    if (ptr && accessGlobal(ptr))
      globSVFGNodes.insert(node);
  }
}

bool SaberSVFGBuilder::accessGlobal(const SVFGNode *node) {
  if (!node || !currentSVFG_)
    return false;
  if (node->getNodeKind() == SVFGK::EntryChi) {
    for (uint32_t objId : node->getDefSVFVars()) {
      if (globs.count(objId) != 0)
        return true;
    }
    return false;
  }
  const Value *ptr = getPointerOperandForStmt(node);
  if (ptr)
    return accessGlobal(ptr);
  const Value *v = node->getValue();
  return v && accessGlobal(v);
}

bool SaberSVFGBuilder::accessGlobal(const llvm::Value *ptr) {
  if (!ptr || !currentSVFG_)
    return false;
  if (const auto *gv = llvm::dyn_cast<llvm::GlobalValue>(ptr)) {
    uint32_t objId = currentSVFG_->getObjectId(gv);
    if (objId != 0 && globs.count(objId))
      return true;
  }
  SVFGNodeBS objIds = getObjectIdsForValue(ptr);
  for (uint32_t objId : objIds) {
    if (globs.count(objId))
      return true;
  }
  return false;
}

bool SaberSVFGBuilder::isStrongUpdate(const SVFGNode *node,
                                      uint32_t &singleton) {
  if (!node || !currentSVFG_)
    return false;

  if (node->getNodeKind() != SVFGK::Store)
    return false;

  const Value *storePtr = getPointerOperandForStmt(node);
  if (!storePtr)
    return false;

  SVFGNodeBS objIds = getObjectIdsForValue(storePtr);
  if (objIds.size() != 1)
    return false;

  uint32_t objId = *objIds.begin();
  if (!currentSVFG_->isHeapObject(objId) &&
      !currentSVFG_->isArrayObject(objId)) {
    if (const auto *info = currentSVFG_->getObjectInfo(objId)) {
      if (!info->isFieldInsensitive) {
        // Match SVF SABER: avoid SU for local variables in recursive functions.
        // SVF checks PTA::isLocalVarInRecursiveFun(singleton). In Lotus we
        // conservatively approximate via a module-level call graph built from
        // direct calls, already-materialized indirect callees, and current PTA
        // targets for unresolved indirect calls.
        if (!recursiveFunctionsReady_) {
          recursiveFunctionsReady_ = true;
          recursiveFunctionsCache_.clear();
          FuncGraph callGraph;
          if (module_) {
            buildResolvedCallGraph(module_, callGraph,
                                   [this](const CallBase *call) {
                                     std::vector<const Function *> targets;
                                     if (currentSVFG_) {
                                       for (const Function *callee :
                                            currentSVFG_->getConnectedCallees(
                                                call)) {
                                         appendUniqueDefinedCallee(targets,
                                                                   callee);
                                       }
                                     }
                                     for (const Function *callee :
                                          this->getIndirectCallTargets(call)) {
                                       appendUniqueDefinedCallee(targets,
                                                                 callee);
                                     }
                                     return targets;
                                   });
          } else if (currentSVFG_ && currentSVFG_->getRefinedCallGraph()) {
            buildResolvedCallGraph(currentSVFG_->getRefinedCallGraph(),
                                   callGraph);
          }
          recursiveFunctionsCache_ = computeRecursiveFunctions(callGraph);
        }
        if (currentSVFG_->isStackObject(objId)) {
          const Value *objVal = currentSVFG_->getObjectValue(objId);
          const Function *owningFun = getOwningFunctionForObjectValue(objVal);
          if (!owningFun) {
            if (const auto *objInfo = currentSVFG_->getObjectInfo(objId)) {
              if (objInfo->baseObjId != 0 && objInfo->baseObjId != objId) {
                owningFun = getOwningFunctionForObjectValue(
                    currentSVFG_->getObjectValue(objInfo->baseObjId));
              }
            }
          }
          if (!owningFun)
            return false;
          if (owningFun && recursiveFunctionsCache_.count(owningFun))
            return false;
        }
        singleton = objId;
        return true;
      }
    }
  }

  return false;
}
