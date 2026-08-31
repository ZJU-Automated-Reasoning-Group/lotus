//===- SVFGBuilder.cpp -- SVFG Builder Implementation
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

#include "IR/SVFG/SVFGBuilder.h"

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Graph/ConstraintGraph/CGObjNode.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/DeepPropagation.h"
#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>

#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

// Define DEBUG_TYPE after including AserPTA headers to avoid redefinition
// warning
#ifdef DEBUG_TYPE
#undef DEBUG_TYPE
#endif
#define DEBUG_TYPE "SVFGBuilder"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PartialUpdateSolver.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/SolverBase.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/WavePropagation.h"

#include <llvm/IR/IntrinsicInst.h>

#undef DEBUG_TYPE

using namespace lotus::analysis;
using namespace llvm;
using namespace aser;

// Type aliases for AserPTA solver configurations
template <typename ctx> using FSModel = DefaultLangModel<ctx, FSMemModel<ctx>>;

using CIWaveSolver = WavePropagation<FSModel<NoCtx>>;
using CIDeepSolver = DeepPropagation<FSModel<NoCtx>>;
using CIBasicSolver = PartialUpdateSolver<FSModel<NoCtx>>;

// Type alias for Object type
using FSObjectTy = FSObject<NoCtx>;
using ObjNodeTy = CGObjNode<FSMemModel<NoCtx>>;
using LangModelTy = FSModel<NoCtx>;
using LMT = LangModelTrait<LangModelTy>;

namespace {

static bool isConcreteHeapAllocationSite(const Value *val) {
  const auto *inst = dyn_cast_or_null<Instruction>(val);
  if (!inst)
    return false;
  return !PointerMayBeCaptured(inst, /*ReturnCaptures=*/true,
                               /*StoreCaptures=*/true);
}

static SVFG::ObjectInfo inferObjectInfoFromValue(const Value *val) {
  SVFG::ObjectInfo info;
  if (!val)
    return info;
  if (const auto *F = dyn_cast<Function>(val)) {
    info.isFunction = true;
    info.isSingleton = true;
    return info;
  }
  if (const auto *GV = dyn_cast<GlobalVariable>(val)) {
    info.isGlobal = true;
    info.isConstant = GV->isConstant();
    info.isSingleton = true;
  } else if (isa<GlobalValue>(val)) {
    info.isGlobal = true;
    info.isSingleton = true;
  }
  if (const auto *inst = dyn_cast<Instruction>(val)) {
    if (isa<AllocaInst>(inst)) {
      info.isStack = true;
      info.isSingleton =
          inst->getFunction()->getName() == "main" &&
          inst->getParent() == &inst->getFunction()->getEntryBlock();
    }
    if (isAllocationFn(inst, nullptr)) {
      info.isHeap = true;
      info.isConcreteHeap = isConcreteHeapAllocationSite(inst);
      info.isSingleton =
          inst->getFunction()->getName() == "main" &&
          inst->getParent() == &inst->getFunction()->getEntryBlock();
    }
  }
  if (isa<Constant>(val) && !isa<GlobalValue>(val))
    info.isConstant = true;
  if (val->getType()->isPointerTy()) {
    if (const Type *elemTy = val->getType()->getPointerElementType())
      info.isArray = elemTy->isArrayTy();
  }
  return info;
}

struct NormalizedGepOffset {
  uint64_t offset = 0;
  bool traversesArray = false;
  bool valid = false;
};

static NormalizedGepOffset getNormalizedGepOffset(const GEPOperator *gep,
                                                  const DataLayout &layout) {
  NormalizedGepOffset result;
  if (!gep)
    return result;

  Type *current = gep->getSourceElementType();
  bool firstIndex = true;
  for (const Use &indexUse : gep->indices()) {
    const auto *index = dyn_cast<ConstantInt>(indexUse.get());
    if (firstIndex) {
      firstIndex = false;
      if (!index) {
        result.traversesArray = true;
      } else if (!current->isArrayTy() && !index->isZero()) {
        result.offset +=
            index->getZExtValue() * layout.getTypeAllocSize(current);
      }
      continue;
    }
    if (auto *structure = dyn_cast<StructType>(current)) {
      if (!index || index->getZExtValue() >= structure->getNumElements())
        return result;
      const unsigned field = static_cast<unsigned>(index->getZExtValue());
      result.offset += layout.getStructLayout(structure)->getElementOffset(
          field);
      current = structure->getElementType(field);
      continue;
    }
    if (auto *array = dyn_cast<ArrayType>(current)) {
      result.traversesArray = true;
      current = array->getElementType();
      continue;
    }
    if (auto *vector = dyn_cast<VectorType>(current)) {
      result.traversesArray = true;
      current = vector->getElementType();
      continue;
    }
    return result;
  }
  result.valid = true;
  return result;
}

static NormalizedGepOffset getNormalizedGepOffset(
    const GetElementPtrInst *gep) {
  if (!gep || !gep->getModule())
    return {};
  return getNormalizedGepOffset(cast<GEPOperator>(gep),
                                gep->getModule()->getDataLayout());
}

} // namespace

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

// Implement SolverWrapper::destroy()
void SVFGBuilder::SolverWrapper::destroy() {
  if (solver) {
    switch (kind) {
    case SolverKind::Wave:
      delete static_cast<CIWaveSolver *>(solver);
      break;
    case SolverKind::Deep:
      delete static_cast<CIDeepSolver *>(solver);
      break;
    case SolverKind::Basic:
      delete static_cast<CIBasicSolver *>(solver);
      break;
    }
    solver = nullptr;
  }
}

SVFGBuilder::~SVFGBuilder() = default;

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

void SVFGBuilder::initializeIndirectCallReverseIndex() {
  if (!svfg || !config.usePointerAnalysis || !ptaSolverWrapper ||
      !ptaSolverWrapper->solver || !icfg)
    return;

  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  for (const Function &F : *M) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        const auto *call = dyn_cast<CallBase>(&I);
        if (!call || call->getCalledFunction())
          continue;

        std::vector<const Function *> callees =
            filterCalleesByICFG(icfg, call, getIndirectCallTargets(call));
        for (const Function *callee : callees) {
          if (!callee || callee->isDeclaration())
            continue;
          svfg->addCalleeToIndCallSite(callee, call);
        }
      }
    }
  }
}

std::size_t SVFGBuilder::connectPreAnalysisIndirectCalls(SVFG *graph) {
  if (!graph || !config.usePointerAnalysis || !ptaSolverWrapper ||
      !ptaSolverWrapper->solver || !icfg)
    return 0;

  const Module *module = getModuleFromICFG(icfg);
  if (!module)
    return 0;

  std::size_t connected = 0;
  for (const Function &function : *module) {
    if (function.isDeclaration() || function.isIntrinsic())
      continue;
    for (const BasicBlock &block : function) {
      for (const Instruction &instruction : block) {
        const auto *call = dyn_cast<CallBase>(&instruction);
        if (!call || call->getCalledFunction())
          continue;

        for (const Function *callee :
             getIndirectCallTargets(call)) {
          if (!callee || callee->isDeclaration() || callee->isIntrinsic())
            continue;
          std::vector<SVFGEdge *> newEdges;
          if (connectCallSiteToCalleeOnTheFly(graph, call, callee, newEdges))
            ++connected;
        }
      }
    }
  }
  return connected;
}

SVFG *SVFGBuilder::build(const ICFG *icfg) { return build(icfg, config); }

SVFG *SVFGBuilder::build(const ICFG *icfg, const SVFGBuilderConfig &cfg) {
  config = cfg;
  initialize(icfg);

  if (config.usePointerAnalysis) {
    runPointerAnalysis();
  }

  prepareMemoryRegionPartitioning();

  initializeIndirectCallReverseIndex();
  buildNodes();
  buildEdges();

  if (config.buildMSSA) {
    buildMemorySSA();
    buildMemoryPHINodes();
    buildInterproceduralMemoryPHINodes();
    connectMemorySSAEdges();
  }

  buildInterproceduralEdges();

  if (config.includeGlobals) {
    connectFromGlobalToProgEntry();
  }

  lastBuiltSVFG = svfg.get();
  return svfg.release();
}

void SVFGBuilder::initialize(const ICFG *cfg) {
  icfg = cfg;
  svfg = std::make_unique<SVFG>();
  svfg->setICFG(icfg);
  const Module *M = getModuleFromICFG(icfg);
  if (M)
    svfg->initializeRefinedCallGraph(*const_cast<Module *>(M));
  lastBuiltSVFG = nullptr;
  nextNodeId = 0;
  nextMemRegId = 1;
  nextValueId = kValueIdBase;
  // Clean up previous solver wrapper if exists
  ptaSolverWrapper.reset();

  valueToNode.clear();
  valueToValueId.clear();
  formalRetValueIds.clear();
  varArgValueIds.clear();
  allocaToMemReg.clear();
  globalToMemReg.clear();
  heapAllocToMemReg.clear();
  ptrValToMemReg.clear();
  loadToLoadNode.clear();
  storeToStoreNode.clear();
  memRegVerToNode.clear();
  funcEntryChi.clear();
  funcExitMu.clear();
  csActualIn.clear();
  csActualOut.clear();
  callToMuNodes.clear();
  callToChiNodes.clear();
  ptaObjectToObjId.clear();
  unknownObjId = 0;
  nextObjId = kObjIdBase;
  objIdToMemReg.clear();
  memRegToObjId.clear();
  memRegToPts.clear();
  ptsToMemReg.clear();
  memoryRegionPartitioner.reset(config.memoryPartition);
  memRegVersion.clear();
  bbToMemPhi.clear();
  argToMemRegs.clear();
  previousPTSets.clear();
  funcEntryChiMemRegs.clear();
  globalEntryRegions.clear();
  vfEdgesAtIndCallSite.clear();
}

void SVFGBuilder::recordRefinedCallEdge(const CallBase *call,
                                        const Function *callee) {
  if (!svfg || !call || !callee)
    return;
  (void)svfg->markConnectedCallee(call, callee);
}

void SVFGBuilder::runPointerAnalysis() {
  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  // Create and run AserPTA solver based on configuration
  // Use type-safe wrapper for storage
  switch (config.solverType) {
  case SVFGBuilderConfig::SolverType::Andersen:
  case SVFGBuilderConfig::SolverType::PartialUpdate: {
    auto *solver = new CIBasicSolver();
    solver->analyze(const_cast<Module *>(M));
    ptaSolverWrapper = std::make_unique<SolverWrapper>(
        SolverWrapper::SolverKind::Basic, solver);
    break;
  }
  case SVFGBuilderConfig::SolverType::WavePropagation: {
    auto *solver = new CIWaveSolver();
    solver->analyze(const_cast<Module *>(M));
    ptaSolverWrapper = std::make_unique<SolverWrapper>(
        SolverWrapper::SolverKind::Wave, solver);
    break;
  }
  case SVFGBuilderConfig::SolverType::DeepPropagation: {
    auto *solver = new CIDeepSolver();
    solver->analyze(const_cast<Module *>(M));
    ptaSolverWrapper = std::make_unique<SolverWrapper>(
        SolverWrapper::SolverKind::Deep, solver);
    break;
  }
  default: {
    auto *solver = new CIWaveSolver();
    solver->analyze(const_cast<Module *>(M));
    ptaSolverWrapper = std::make_unique<SolverWrapper>(
        SolverWrapper::SolverKind::Wave, solver);
    break;
  }
  }
}

void SVFGBuilder::prepareMemoryRegionPartitioning() {
  const Module *module = getModuleFromICFG(icfg);
  if (!module) {
    memoryRegionPartitioner.freeze();
    return;
  }

  auto observe = [&](const Value *pointer, const Function *scope) {
    if (!pointer || !pointer->getType()->isPointerTy())
      return;
    const SVFGNodeBS objects = getObjectIdsForValue(pointer);
    if (!objects.empty())
      memoryRegionPartitioner.observe(scope, objects);
  };

  for (const GlobalVariable &global : module->globals())
    observe(&global, nullptr);

  for (const Function &function : *module) {
    if (function.isDeclaration())
      continue;
    for (const Argument &argument : function.args())
      observe(&argument, &function);
    for (const BasicBlock &block : function) {
      for (const Instruction &instruction : block) {
        if (const auto *load = dyn_cast<LoadInst>(&instruction))
          observe(load->getPointerOperand(), &function);
        else if (const auto *store = dyn_cast<StoreInst>(&instruction))
          observe(store->getPointerOperand(), &function);

        if (instruction.getType()->isPointerTy())
          observe(&instruction, &function);
        if (const auto *call = dyn_cast<CallBase>(&instruction))
          for (const Use &argument : call->args())
            observe(argument.get(), &function);
      }
    }
  }

  if (unknownObjId != 0)
    memoryRegionPartitioner.setUnknownObject(unknownObjId);
  memoryRegionPartitioner.freeze();
}
uint32_t SVFGBuilder::getOrCreateNode(const Value *val) {
  auto it = valueToNode.find(val);
  if (it != valueToNode.end()) {
    return it->second;
  }

  uint32_t id = nextNode();
  valueToNode[val] = id;
  if (svfg)
    svfg->setValueNode(val, id);
  return id;
}

uint32_t SVFGBuilder::getOrCreateValueId(const Value *val) {
  if (!val)
    return 0;
  auto it = valueToValueId.find(val);
  if (it != valueToValueId.end())
    return it->second;
  const uint32_t id = nextValueId++;
  valueToValueId.emplace(val, id);
  return id;
}

uint32_t SVFGBuilder::getOrCreateFormalRetValueId(const Function *F) {
  if (!F)
    return 0;
  auto it = formalRetValueIds.find(F);
  if (it != formalRetValueIds.end())
    return it->second;
  const uint32_t id = nextValueId++;
  formalRetValueIds.emplace(F, id);
  return id;
}

uint32_t SVFGBuilder::getOrCreateVarArgValueId(const Function *F) {
  if (!F)
    return 0;
  auto it = varArgValueIds.find(F);
  if (it != varArgValueIds.end())
    return it->second;
  const uint32_t id = nextValueId++;
  varArgValueIds.emplace(F, id);
  return id;
}

uint32_t SVFGBuilder::getOrCreateMemReg(const AllocaInst *alloca) {
  auto it = allocaToMemReg.find(alloca);
  if (it != allocaToMemReg.end()) {
    return it->second;
  }

  uint32_t memRegId = nextMemRegId++;
  allocaToMemReg[alloca] = memRegId;
  return memRegId;
}

uint32_t SVFGBuilder::getOrCreateMemReg(const GlobalVariable *global) {
  auto it = globalToMemReg.find(global);
  if (it != globalToMemReg.end()) {
    return it->second;
  }

  uint32_t memRegId = nextMemRegId++;
  globalToMemReg[global] = memRegId;
  return memRegId;
}

uint32_t SVFGBuilder::getOrCreateMemReg(const Instruction *heapAlloc) {
  auto it = heapAllocToMemReg.find(heapAlloc);
  if (it != heapAllocToMemReg.end()) {
    return it->second;
  }

  uint32_t memRegId = nextMemRegId++;
  heapAllocToMemReg[heapAlloc] = memRegId;
  return memRegId;
}

uint32_t SVFGBuilder::getOrCreateMemReg(const Value *ptrVal) {
  if (!ptrVal)
    return 0;

  if (const auto *alloca = dyn_cast<AllocaInst>(ptrVal)) {
    return getOrCreateMemReg(alloca);
  }

  if (const auto *gv = dyn_cast<GlobalVariable>(ptrVal)) {
    return getOrCreateMemReg(gv);
  }

  if (const auto *arg = dyn_cast<Argument>(ptrVal)) {
    auto argIt = argToMemRegs.find(arg);
    if (argIt != argToMemRegs.end() && argIt->second.size() == 1) {
      return argIt->second.front();
    }
  }

  if (const auto *inst = dyn_cast<Instruction>(ptrVal)) {
    if (isHeapAllocation(inst)) {
      return getOrCreateMemReg(inst);
    }
  }

  auto it = ptrValToMemReg.find(ptrVal);
  if (it != ptrValToMemReg.end()) {
    return it->second;
  }

  const uint32_t memRegId = nextMemRegId++;
  ptrValToMemReg[ptrVal] = memRegId;
  return memRegId;
}

const Function *SVFGBuilder::getMemoryRegionScope(const Value *value) {
  if (const auto *instruction = dyn_cast_or_null<Instruction>(value))
    return instruction->getFunction();
  if (const auto *argument = dyn_cast_or_null<Argument>(value))
    return argument->getParent();
  return nullptr;
}

uint32_t SVFGBuilder::getOrCreateMemRegForPointsTo(const SVFGNodeBS &pts,
                                                   const Function *scope) {
  if (pts.empty()) {
    // Callers should avoid using a points-to derived region for empty/unknown.
    return 0;
  }

  const SVFGNodeBS canonical = memoryRegionPartitioner.canonicalize(scope, pts);
  auto it = ptsToMemReg.find(canonical);
  if (it != ptsToMemReg.end()) {
    return it->second;
  }

  const uint32_t memRegId = nextMemRegId++;
  ptsToMemReg.emplace(canonical, memRegId);
  memRegToPts.emplace(memRegId, canonical);
  return memRegId;
}

uint32_t SVFGBuilder::getOrCreateMemRegForObject(uint32_t objId) {
  if (objId == 0)
    return 0;
  auto it = objIdToMemReg.find(objId);
  if (it != objIdToMemReg.end())
    return it->second;
  const uint32_t memRegId = getOrCreateMemRegForPointsTo(SVFGNodeBS{objId});
  objIdToMemReg.emplace(objId, memRegId);
  memRegToObjId.emplace(memRegId, objId);
  return memRegId;
}

SVFGNodeBS SVFGBuilder::convertPTAObjectsToObjIDs(
    const std::vector<const void *> &ptaObjects, bool keepFunctions) {
  SVFGNodeBS result;

  if (config.memModelType ==
      SVFGBuilderConfig::MemModelType::FieldInsensitive) {
    if (!ptaObjects.empty()) {
      if (unknownObjId == 0) {
        unknownObjId = nextObjId++;
        if (svfg) {
          svfg->setObjectDebug(unknownObjId, "FI_ANY_OBJECT");
          SVFG::ObjectInfo info;
          info.isFieldInsensitive = true;
          info.isUnknown = true;
          svfg->setObjectInfo(unknownObjId, info);
        }
      }
      result.insert(unknownObjId);
    }
    return result;
  }

  auto buildInfoForObject = [&](const FSObjectTy *obj) -> SVFG::ObjectInfo {
    SVFG::ObjectInfo info;
    if (!obj)
      return info;
    info.isFunction = obj->isFunction();
    info.isHeap = obj->isHeapObj();
    if (info.isHeap)
      info.isConcreteHeap = isConcreteHeapAllocationSite(obj->getValue());
    info.isStack = obj->isStackObj();
    info.isGlobal = obj->isGlobalObj();
    info.isFieldInsensitive = obj->isFIObject();
    const Value *val = obj->getValue();
    if (val) {
      info.isSingleton = inferObjectInfoFromValue(val).isSingleton;
      if (const auto *gv = dyn_cast<GlobalVariable>(val))
        info.isConstant = gv->isConstant();
      else
        info.isConstant = isa<Constant>(val);
    }
    if (val && val->getType()->isPointerTy()) {
      if (const Type *elemTy = val->getType()->getPointerElementType()) {
        info.isArray = elemTy->isArrayTy();
      }
    }
    return info;
  };

  for (const void *v : ptaObjects) {
    const FSObjectTy *obj = static_cast<const FSObjectTy *>(v);
    if (!obj)
      continue;

    if (!keepFunctions && obj->isFunction()) {
      continue;
    }

    // Check cache.
    auto cachedIt = ptaObjectToObjId.find(v);
    if (cachedIt != ptaObjectToObjId.end()) {
      const uint32_t objId = cachedIt->second;
      result.insert(objId);
      if (objIdToPTAObject.find(objId) == objIdToPTAObject.end())
        objIdToPTAObject.emplace(objId, v);
      if (svfg) {
        svfg->updateObjectInfo(objId, buildInfoForObject(obj));
        if (const Value *val = obj->getValue())
          svfg->setObjectValue(objId, val);
      }
      continue;
    }

    const uint32_t objId = nextObjId++;
    ptaObjectToObjId.emplace(v, objId);
    objIdToPTAObject.emplace(objId, v);
    result.insert(objId);

    // Register stable debug label and objId <-> Value* for DDA.
    if (svfg) {
      SVFG::ObjectInfo info = buildInfoForObject(obj);
      if (info.baseObjId == 0)
        info.baseObjId = objId;
      svfg->setObjectDebug(objId, obj->toString(false));
      if (const Value *val = obj->getValue())
        svfg->setObjectValue(objId, val);
      svfg->setObjectInfo(objId, info);
    }
  }

  return result;
}

uint32_t
SVFGBuilder::getOrCreateCanonicalObjectIdForValue(const Value *v,
                                                  SVFG::ObjectInfo info) {
  if (!svfg || !v)
    return 0;
  if (const uint32_t existing = svfg->getObjectId(v))
    return existing;

  if (config.usePointerAnalysis && ptaSolverWrapper &&
      ptaSolverWrapper->solver && v->getType()->isPointerTy()) {
    SVFGNodeBS canonicalIds = getObjectIdsForValue(v);
    if (canonicalIds.size() == 1) {
      const uint32_t objId = *canonicalIds.begin();
      if (info.baseObjId == 0)
        info.baseObjId = objId;
      svfg->updateObjectInfo(objId, info);
      svfg->setObjectValue(objId, v);
      (void)getOrCreateMemRegForObject(objId);
      return objId;
    }
  }

  const uint32_t objId = nextObjId++;
  if (info.baseObjId == 0)
    info.baseObjId = objId;
  svfg->setObjectInfo(objId, info);
  svfg->setObjectValue(objId, v);
  if (const auto *F = dyn_cast<Function>(v))
    svfg->setObjectDebug(objId, ("FUN:" + F->getName()).str());
  else if (const auto *GV = dyn_cast<GlobalValue>(v))
    svfg->setObjectDebug(objId, ("GV:" + GV->getName()).str());
  else if (v->hasName())
    svfg->setObjectDebug(objId, ("OBJ:" + v->getName()).str());
  else
    svfg->setObjectDebug(objId, "OBJ");
  (void)getOrCreateMemRegForObject(objId);
  return objId;
}

uint32_t SVFGBuilder::getCanonicalBaseObjId(uint32_t objId) const {
  SVFG *graph = getActiveSVFG();
  if (objId == 0 || !graph)
    return objId;
  if (const auto *info = graph->getObjectInfo(objId)) {
    if (info->baseObjId != 0)
      return info->baseObjId;
  }
  return objId;
}

SVFGNodeBS SVFGBuilder::getObjectIdsForValue(const Value *ptr) {
  SVFGNodeBS result;
  SVFG *graph = getActiveSVFG();
  if (!ptr)
    return result;
  if (graph && isa<GetElementPtrInst>(ptr)) {
    const SVFGNodeBS &mapped = graph->getObjectIds(ptr);
    if (!mapped.empty())
      return mapped;
  }
  if (config.usePointerAnalysis && ptaSolverWrapper &&
      ptaSolverWrapper->solver && ptr->getType()->isPointerTy()) {
    std::vector<const void *> ptsVoid = getPointsToSet(ptr);
    result = convertPTAObjectsToObjIDs(ptsVoid, true);
    auto isCanonicalObjectValue = [&](const Value *v) {
      if (!v)
        return false;
      if (isa<Function, GlobalValue, GetElementPtrInst, AllocaInst>(v))
        return true;
      if (const auto *inst = dyn_cast<Instruction>(v))
        return isHeapAllocation(inst);
      return false;
    };
    if (result.size() == 1 && graph && isCanonicalObjectValue(ptr)) {
      const uint32_t objId = *result.begin();
      graph->setObjectValue(objId, ptr);
      graph->updateObjectInfo(objId, inferObjectInfoFromValue(ptr));
    }
  }
  if (result.empty() && graph) {
    const uint32_t objId = graph->getObjectId(ptr);
    if (objId != 0)
      result.insert(objId);
  }
  if (graph)
    for (uint32_t objId : result)
      graph->addObjectForValue(ptr, objId);
  return result;
}

uint32_t SVFGBuilder::getGepObjectId(uint32_t baseObjId,
                                     const GetElementPtrInst *gep) {
  if (!gep || baseObjId == 0)
    return 0;
  SVFG *graph = getActiveSVFG();
  const uint32_t canonicalBaseObjId = getCanonicalBaseObjId(baseObjId);

  auto queryAserFieldObject = [&]() -> uint32_t {
    if (!config.usePointerAnalysis || !ptaSolverWrapper ||
        !ptaSolverWrapper->solver ||
        config.memModelType ==
            SVFGBuilderConfig::MemModelType::FieldInsensitive)
      return 0;
    auto baseIt = objIdToPTAObject.find(baseObjId);
    if (baseIt == objIdToPTAObject.end())
      baseIt = objIdToPTAObject.find(canonicalBaseObjId);
    if (baseIt == objIdToPTAObject.end())
      return 0;
    const auto *baseObject = static_cast<const FSObjectTy *>(baseIt->second);
    ObjNodeTy *objectNode = baseObject ? baseObject->getObjNode() : nullptr;
    if (!objectNode)
      return 0;
    CGNodeBase<NoCtx> *fieldNode = nullptr;
    switch (ptaSolverWrapper->kind) {
    case SolverWrapper::SolverKind::Wave:
      fieldNode = LMT::indexObjectForClients(
          static_cast<CIWaveSolver *>(ptaSolverWrapper->solver)
              ->getLangModelForClients(),
          objectNode, gep);
      break;
    case SolverWrapper::SolverKind::Deep:
      fieldNode = LMT::indexObjectForClients(
          static_cast<CIDeepSolver *>(ptaSolverWrapper->solver)
              ->getLangModelForClients(),
          objectNode, gep);
      break;
    case SolverWrapper::SolverKind::Basic:
      fieldNode = LMT::indexObjectForClients(
          static_cast<CIBasicSolver *>(ptaSolverWrapper->solver)
              ->getLangModelForClients(),
          objectNode, gep);
      break;
    }
    auto *fieldObjectNode = static_cast<ObjNodeTy *>(fieldNode);
    const FSObjectTy *fieldObject =
        fieldObjectNode ? fieldObjectNode->getObject() : nullptr;
    if (!fieldObject)
      return 0;
    auto cached = ptaObjectToObjId.find(fieldObject);
    if (cached != ptaObjectToObjId.end())
      return cached->second;
    const SVFGNodeBS ids = convertPTAObjectsToObjIDs({fieldObject}, true);
    return ids.empty() ? 0 : *ids.begin();
  };

  const NormalizedGepOffset normalized = getNormalizedGepOffset(gep);
  if (graph && normalized.valid) {
    graph->setGepAccess(gep, normalized.offset, normalized.traversesArray);
    uint64_t baseOffset = 0;
    if (const SVFG::ObjectInfo *baseInfo = graph->getObjectInfo(baseObjId))
      if (baseInfo->hasFieldOffset)
        baseOffset = baseInfo->fieldOffset;
    const uint64_t totalOffset = baseOffset + normalized.offset;
    if (totalOffset == 0) {
      if (normalized.traversesArray) {
        SVFG::ObjectInfo arrayInfo;
        arrayInfo.isArray = true;
        graph->updateObjectInfo(canonicalBaseObjId, arrayInfo);
      }
      return canonicalBaseObjId;
    }
    if (const uint32_t existing =
            graph->getOffsetObject(canonicalBaseObjId, totalOffset)) {
      if (normalized.traversesArray) {
        SVFG::ObjectInfo arrayInfo;
        arrayInfo.isArray = true;
        graph->updateObjectInfo(existing, arrayInfo);
      }
      return existing;
    }

    uint32_t fieldObjId = queryAserFieldObject();
    if (fieldObjId == 0)
      fieldObjId = nextObjId++;
    SVFG::ObjectInfo info;
    if (const SVFG::ObjectInfo *baseInfo =
            graph->getObjectInfo(canonicalBaseObjId))
      info = *baseInfo;
    info.baseObjId = canonicalBaseObjId;
    info.fieldOffset = totalOffset;
    info.hasFieldOffset = true;
    info.isArray = info.isArray || normalized.traversesArray;
    graph->setObjectInfo(fieldObjId, info);
    graph->setObjectValue(fieldObjId, gep);
    graph->setObjectBase(fieldObjId, canonicalBaseObjId);
    graph->setObjectOffset(fieldObjId, totalOffset);
    graph->setObjectDebug(
        fieldObjId, "FIELD(" + std::to_string(canonicalBaseObjId) + "," +
                        std::to_string(totalOffset) + ")");
    graph->setOffsetObject(canonicalBaseObjId, totalOffset, fieldObjId);
    return fieldObjId;
  }

  // Early return if PTA is unavailable
  if (!config.usePointerAnalysis || !ptaSolverWrapper ||
      !ptaSolverWrapper->solver)
    return baseObjId;

  // Check field-insensitivity markers (heap objects with unknown size, large
  // structs)
  if (graph && graph->isUnknownObject(baseObjId))
    return baseObjId;
  if (graph && graph->isFieldInsensitiveObject(baseObjId))
    return getOrCreateFIObjId(canonicalBaseObjId);

  // Field-insensitive memory model bypasses field analysis
  if (config.memModelType == SVFGBuilderConfig::MemModelType::FieldInsensitive)
    return baseObjId;

  // GEP with non-constant indices falls back to base object (array element
  // tracking)
  bool hasConstantIndices = true;
  for (auto &idx : gep->indices()) {
    if (!isa<ConstantInt>(idx)) {
      hasConstantIndices = false;
      break;
    }
  }
  if (!hasConstantIndices) {
    // Arrays: treat as field-insensitive when indices are symbolic.
    return getOrCreateFIObjId(canonicalBaseObjId);
  }

  auto it = objIdToPTAObject.find(canonicalBaseObjId);
  if (it == objIdToPTAObject.end())
    return baseObjId;

  const FSObjectTy *baseObj = static_cast<const FSObjectTy *>(it->second);
  if (!baseObj)
    return baseObjId;

  ObjNodeTy *objNode = baseObj->getObjNode();
  if (!objNode)
    return 0;

  CGNodeBase<NoCtx> *fieldNode = nullptr;
  switch (ptaSolverWrapper->kind) {
  case SolverWrapper::SolverKind::Wave: {
    auto *solver = static_cast<CIWaveSolver *>(ptaSolverWrapper->solver);
    if (solver)
      fieldNode = LMT::indexObjectForClients(solver->getLangModelForClients(),
                                             objNode, gep);
    break;
  }
  case SolverWrapper::SolverKind::Deep: {
    auto *solver = static_cast<CIDeepSolver *>(ptaSolverWrapper->solver);
    if (solver)
      fieldNode = LMT::indexObjectForClients(solver->getLangModelForClients(),
                                             objNode, gep);
    break;
  }
  case SolverWrapper::SolverKind::Basic: {
    auto *solver = static_cast<CIBasicSolver *>(ptaSolverWrapper->solver);
    if (solver)
      fieldNode = LMT::indexObjectForClients(solver->getLangModelForClients(),
                                             objNode, gep);
    break;
  }
  }
  if (!fieldNode)
    return 0;
  auto *fieldObjNode = static_cast<ObjNodeTy *>(fieldNode);
  if (!fieldObjNode)
    return 0;
  const FSObjectTy *fieldObj = fieldObjNode->getObject();
  if (!fieldObj)
    return 0;

  auto cached = ptaObjectToObjId.find(fieldObj);
  if (cached != ptaObjectToObjId.end()) {
    if (graph) {
      graph->setObjectValue(cached->second, gep);
      graph->setObjectBase(cached->second, canonicalBaseObjId);
    }
    return cached->second;
  }

  // Create a new objId for the field object if needed.
  SVFGNodeBS tmp = convertPTAObjectsToObjIDs({fieldObj}, true);
  if (tmp.empty())
    return 0;
  const uint32_t objId = *tmp.begin();
  if (graph) {
    graph->setObjectValue(objId, gep);
    graph->setObjectBase(objId, canonicalBaseObjId);
  }
  return objId;
}

uint32_t SVFGBuilder::getOrCreateFIObjId(uint32_t baseObjId) {
  if (baseObjId == 0)
    return 0;
  SVFG *graph = getActiveSVFG();
  if (graph && graph->isUnknownObject(baseObjId))
    return baseObjId;

  const uint32_t canonicalBaseObjId = getCanonicalBaseObjId(baseObjId);
  auto it = baseObjToFIObjId.find(canonicalBaseObjId);
  if (it != baseObjToFIObjId.end())
    return it->second;
  if (graph && graph->isFieldInsensitiveObject(baseObjId) &&
      canonicalBaseObjId != baseObjId) {
    baseObjToFIObjId[canonicalBaseObjId] = baseObjId;
    return baseObjId;
  }
  const uint32_t fiObjId = nextObjId++;
  baseObjToFIObjId[canonicalBaseObjId] = fiObjId;
  if (graph) {
    SVFG::ObjectInfo baseUpdate;
    baseUpdate.isFieldInsensitive = true;
    baseUpdate.baseObjId = canonicalBaseObjId;
    graph->updateObjectInfo(canonicalBaseObjId, baseUpdate);

    SVFG::ObjectInfo info;
    if (const auto *baseInfo = graph->getObjectInfo(canonicalBaseObjId))
      info = *baseInfo;
    info.isFieldInsensitive = true;
    info.baseObjId = canonicalBaseObjId;
    graph->setObjectInfo(fiObjId, info);
    const Value *val = graph->getObjectValue(canonicalBaseObjId);
    if (val)
      graph->setObjectValue(fiObjId, val);
    std::string label = "FI_OBJ(" + std::to_string(canonicalBaseObjId) + ")";
    graph->setObjectDebug(fiObjId, std::move(label));
  }
  return fiObjId;
}

uint32_t SVFGBuilder::getOrCreateUnknownObjId() {
  if (unknownObjId != 0)
    return unknownObjId;
  unknownObjId = nextObjId++;
  if (SVFG *graph = getActiveSVFG()) {
    graph->setObjectDebug(unknownObjId, "ANY_OBJECT");
    SVFG::ObjectInfo info;
    info.isUnknown = true;
    graph->setObjectInfo(unknownObjId, info);
  }
  return unknownObjId;
}

uint32_t SVFGBuilder::getUnknownObjId() { return getOrCreateUnknownObjId(); }

std::vector<const void *> SVFGBuilder::getPointsToSet(const Value *ptr) {
  std::vector<const FSObjectTy *> ptsResult;
  std::vector<const void *> result;

  if (!config.usePointerAnalysis || !ptaSolverWrapper ||
      !ptaSolverWrapper->solver) {
    // Conservative fallback: return all known memory regions
    // This is a sound but imprecise approximation
    for (auto &p : allocaToMemReg) {
      // Note: We can't create actual FSObject pointers here without PTA
      // So we return empty set, which will be handled conservatively by callers
    }
    for (auto &p : heapAllocToMemReg) {
      // Same for heap allocations
    }
    return result;
  }

  // Use AserPTA to get points-to set
  switch (ptaSolverWrapper->kind) {
  case SolverWrapper::SolverKind::Wave: {
    auto *solver = static_cast<CIWaveSolver *>(ptaSolverWrapper->solver);
    if (solver)
      solver->getPointsTo(nullptr, ptr, ptsResult);
    break;
  }
  case SolverWrapper::SolverKind::Deep: {
    auto *solver = static_cast<CIDeepSolver *>(ptaSolverWrapper->solver);
    if (solver)
      solver->getPointsTo(nullptr, ptr, ptsResult);
    break;
  }
  case SolverWrapper::SolverKind::Basic: {
    auto *solver = static_cast<CIBasicSolver *>(ptaSolverWrapper->solver);
    if (solver)
      solver->getPointsTo(nullptr, ptr, ptsResult);
    break;
  }
  }

  // Convert to void* vector for opaque interface.
  for (const auto *obj : ptsResult) {
    result.push_back(static_cast<const void *>(obj));
  }

  if (result.empty() && ptr->getType()->isPointerTy()) {
    const Value *base = ptr->stripPointerCasts();
    const bool isKnownAddressTaken =
        isa<AllocaInst>(base) || isa<GlobalVariable>(base) ||
        (isa<Instruction>(base) && isHeapAllocation(cast<Instruction>(base)));
    if (isKnownAddressTaken) {
      // Return empty — callers check for empty and create unknownObjId nodes.
      // (The unknownObjId sentinel is created lazily by
      // getOrCreateUnknownObjId.) Returning empty here is correct: callers
      // already handle the empty case by calling getOrCreateUnknownObjId(). The
      // old bug was that the no-PTA fallback returned empty AND callers did
      // nothing with it. Now that callers handle empty correctly (Bug #5 was in
      // the no-PTA branch), we just return empty and let callers do the right
      // thing.
    }
  }

  return result;
}

std::vector<const Function *>
SVFGBuilder::getIndirectCallTargets(const CallBase *call) {
  std::vector<const Function *> targets;

  if (!config.usePointerAnalysis || !ptaSolverWrapper ||
      !ptaSolverWrapper->solver)
    return targets;

  if (const Function *directCallee =
          call ? call->getCalledFunction() : nullptr) {
    if (!directCallee->isDeclaration())
      targets.push_back(directCallee);
    return targets;
  }
  if (call) {
    if (const Value *called = call->getCalledOperand()) {
      if (const Function *directTarget =
              dyn_cast<Function>(called->stripPointerCasts())) {
        if (!directTarget->isDeclaration())
          targets.push_back(directTarget);
        return targets;
      }
    }
  }

  auto addUniqueTarget = [&](const Function *F) {
    if (!F)
      return;
    if (std::find(targets.begin(), targets.end(), F) == targets.end())
      targets.push_back(F);
  };

  auto collectResolvedTargets = [&](auto *solver) {
    if (!solver || !call)
      return;
    if (const auto *indCS =
            solver->getInDirectCallSite(nullptr, cast<Instruction>(call))) {
      for (const Function *F : indCS->getResolvedTarget())
        addUniqueTarget(F);
    }
  };

  switch (ptaSolverWrapper->kind) {
  case SolverWrapper::SolverKind::Wave:
    collectResolvedTargets(
        static_cast<CIWaveSolver *>(ptaSolverWrapper->solver));
    break;
  case SolverWrapper::SolverKind::Deep:
    collectResolvedTargets(
        static_cast<CIDeepSolver *>(ptaSolverWrapper->solver));
    break;
  case SolverWrapper::SolverKind::Basic:
    collectResolvedTargets(
        static_cast<CIBasicSolver *>(ptaSolverWrapper->solver));
    break;
  }

  if (targets.empty()) {
    const Value *calledVal = call->getCalledOperand();
    if (!calledVal || !calledVal->getType()->isPointerTy())
      return targets;

    // Get points-to set of the called value
    std::vector<const void *> ptsVoid = getPointsToSet(calledVal);
    std::vector<const FSObjectTy *> pts;
    pts.reserve(ptsVoid.size());
    for (const void *v : ptsVoid) {
      pts.push_back(static_cast<const FSObjectTy *>(v));
    }

    // Filter for Function pointers
    // In AserPTA, function objects have getValue() that returns the Function*.
    for (const FSObjectTy *obj : pts) {
      if (!obj)
        continue;

      const Value *val = obj->getValue();
      if (!val)
        continue;

      if (const Function *F = dyn_cast<Function>(val))
        addUniqueTarget(F);

      if (val->getType()->isPointerTy()) {
        if (dyn_cast<FunctionType>(val->getType()->getPointerElementType())) {
          if (const Function *F = dyn_cast<Function>(val->stripPointerCasts()))
            addUniqueTarget(F);
        }
      }
    }

    // Conservative field-sensitive fallback for struct/array-held function
    // pointers.
    if (const LoadInst *load = dyn_cast<LoadInst>(calledVal)) {
      const Value *srcPtr = load->getPointerOperand();
      std::vector<const void *> srcPtsVoid = getPointsToSet(srcPtr);
      for (const void *v : srcPtsVoid) {
        const FSObjectTy *srcObj = static_cast<const FSObjectTy *>(v);
        if (!srcObj)
          continue;

        const Value *srcVal = srcObj->getValue();
        if (srcVal && srcVal->getType()->isPointerTy()) {
          if (const Function *F =
                  dyn_cast<Function>(srcVal->stripPointerCasts()))
            addUniqueTarget(F);
        }
      }
    }

    if (targets.empty()) {
      if (const Function *F =
              dyn_cast<Function>(calledVal->stripPointerCasts()))
        addUniqueTarget(F);
    }
  }

  // Recover field-initialized function pointers when the auxiliary solver
  // loses the aggregate field object. Match the source aggregate type and
  // normalized byte offset, rather than just the final field number.
  if (targets.empty() && call) {
    const auto *calledLoad = dyn_cast<LoadInst>(call->getCalledOperand());
    const auto *callGep =
        calledLoad ? dyn_cast<GEPOperator>(calledLoad->getPointerOperand())
                   : nullptr;
    const NormalizedGepOffset callAccess = callGep
                                               ? getNormalizedGepOffset(
                                                     callGep,
                                                     call->getModule()
                                                         ->getDataLayout())
                                               : NormalizedGepOffset{};
    if (callGep && callAccess.valid) {
      const Module *module = call->getModule();
      Type *aggregateType = callGep->getSourceElementType();
      for (const GlobalVariable &global : module->globals()) {
        if (!global.hasInitializer() || global.getValueType() != aggregateType)
          continue;
        const auto *aggregate = dyn_cast<ConstantStruct>(global.getInitializer());
        auto *structure = dyn_cast<StructType>(aggregateType);
        if (!aggregate || !structure)
          continue;
        const StructLayout *layout = module->getDataLayout().getStructLayout(
            structure);
        for (unsigned field = 0; field < aggregate->getNumOperands(); ++field) {
          if (layout->getElementOffset(field) != callAccess.offset)
            continue;
          const auto *target = dyn_cast<Function>(
              aggregate->getOperand(field)->stripPointerCasts());
          if (target && !target->isDeclaration() &&
              target->getFunctionType() == call->getFunctionType())
            addUniqueTarget(target);
        }
      }
      for (const Function &function : *module) {
        for (const BasicBlock &block : function) {
          for (const Instruction &instruction : block) {
            const auto *store = dyn_cast<StoreInst>(&instruction);
            const auto *storeGep =
                store ? dyn_cast<GEPOperator>(store->getPointerOperand())
                      : nullptr;
            const NormalizedGepOffset storeAccess =
                getNormalizedGepOffset(storeGep, module->getDataLayout());
            if (!storeGep || !storeAccess.valid ||
                storeGep->getSourceElementType() != aggregateType ||
                storeAccess.offset != callAccess.offset)
              continue;
            const auto *target = dyn_cast<Function>(
                store->getValueOperand()->stripPointerCasts());
            if (target && !target->isDeclaration() &&
                target->getFunctionType() == call->getFunctionType())
              addUniqueTarget(target);
          }
        }
      }
    }
  }

  if (svfg && call && !call->getCalledFunction()) {
    for (const Function *target : targets)
      svfg->addCalleeToIndCallSite(target, call);
  }

  return targets;
}

bool SVFGBuilder::isHeapAllocation(const Instruction *inst) const {
  if (!inst)
    return false;

  if (const CallBase *call = dyn_cast<CallBase>(inst)) {
    if (const Value *calledOperand = call->getCalledOperand()) {
      if (isAllocationFn(calledOperand, nullptr))
        return true;
    }
    if (const Function *callee = call->getCalledFunction()) {
      if (isAllocationFn(callee, nullptr))
        return true;
      StringRef name = callee->getName();
      if (name == "malloc" || name == "calloc" || name == "aligned_alloc" ||
          name == "valloc" || name == "memalign" || name == "_Znwm" ||
          name == "_Znam" || name == "_Znwj" || name == "_Znaj")
        return true;
    }
  }

  return false;
}

bool SVFGBuilder::isAddressTakenPointer(const Value *ptr) const {
  if (!ptr || !ptr->getType()->isPointerTy())
    return false;

  const Value *base = ptr->stripPointerCasts();
  while (const auto *gep = dyn_cast<GEPOperator>(base))
    base = gep->getPointerOperand()->stripPointerCasts();
  if (isa<AllocaInst>(base) || isa<GlobalVariable>(base))
    return true;
  if (const auto *arg = dyn_cast<Argument>(base)) {
    auto it = argToMemRegs.find(arg);
    return it != argToMemRegs.end() && !it->second.empty();
  }
  if (const auto *inst = dyn_cast<Instruction>(base)) {
    if (isHeapAllocation(inst))
      return true;
  }

  // Without pointer analysis, be conservative for memory SSA: treat pointers
  // appearing in memory operations as address-taken so DDA has a sound
  // (wildcard-guarded) memory value-flow to traverse.
  if (!config.usePointerAnalysis || !ptaSolverWrapper ||
      !ptaSolverWrapper->solver)
    return true;

  // A preliminary PTA empty set is not proof that a load/store operand is not
  // memory-backed. In particular, the auxiliary AserPTA can miss values
  // loaded through spilled parameters or global ConstantExpr fields. Keep the
  // access and let MemorySSA assign a wildcard region when no object is known;
  // dropping it here would make exhaustive flow-sensitive analysis unsound.
  return true;
}

bool SVFGBuilder::mayAliasMemoryNodes(const MSSASVFGNode *lhs,
                                      const MSSASVFGNode *rhs) const {
  if (!lhs || !rhs)
    return false;

  const SVFGNodeBS lhsPts = lhs->getDefSVFVars();
  const SVFGNodeBS rhsPts = rhs->getDefSVFVars();

  // Conservative: if either set is unknown/empty, keep the edge.
  if (lhsPts.empty() || rhsPts.empty())
    return true;

  // Unknown object is a wildcard and may alias anything.
  if (unknownObjId != 0 &&
      (lhsPts.count(unknownObjId) != 0 || rhsPts.count(unknownObjId) != 0))
    return true;

  for (uint32_t id : lhsPts) {
    if (rhsPts.count(id))
      return true;
  }
  return false;
}

uint32_t SVFGBuilder::createMemRegVerNode(uint32_t memReg, uint32_t version,
                                          const ICFGNode *icfgNode) {
  MemRegVer mrv{memReg, version};
  auto it = memRegVerToNode.find(mrv);
  if (it != memRegVerToNode.end()) {
    return it->second;
  }

  uint32_t nodeId = nextNode();
  SVFGNodeBS pts;

  // Get points-to information from the memory region
  // Find the alloca or heap allocation that created this region
  const AllocaInst *alloca = nullptr;
  for (auto &p : allocaToMemReg) {
    if (p.second == memReg) {
      alloca = p.first;
      break;
    }
  }

  if (alloca) {
    std::vector<const void *> ptsVoid = getPointsToSet(alloca);
    // Convert PTA objects to SVFG node IDs
    pts = convertPTAObjectsToObjIDs(ptsVoid);
  }

  auto *phiNode =
      new IntraMSSAPhiSVFGNode(nodeId, icfgNode, memReg, version, pts);
  svfg->addNode(phiNode);
  memRegVerToNode[mrv] = nodeId;

  return nodeId;
}

uint32_t SVFGBuilder::createMemoryPHI(uint32_t memReg, const BasicBlock *bb) {
  // Create a memory PHI node at a control flow merge point
  // This is called when multiple definitions of a memory region reach a basic
  // block

  // Check if PHI already exists for this memory region at this block
  auto phiIt = bbToMemPhi[bb].find(memReg);
  if (phiIt != bbToMemPhi[bb].end()) {
    return phiIt->second;
  }

  uint32_t nodeId = nextNode();
  const Function *F = bb->getParent();
  // Use per-function versioning to avoid collisions across functions
  uint32_t version = nextVersion(F, memReg);
  SVFGNodeBS pts;

  // Get points-to set from the function entry def for this memory region.
  for (uint32_t formalInId : funcEntryChi[F]) {
    SVFGNode *formalInNode = svfg->getNode(formalInId);
    if (auto *formalIn = dyn_cast<FormalInSVFGNode>(formalInNode)) {
      if (formalIn->getMemReg() == memReg) {
        pts = formalIn->getDefSVFVars();
        break;
      }
    }
  }

  // Find ICFG node for this basic block
  const ICFGNode *icfgNode = nullptr;
  for (auto &pair : *icfg) {
    if (IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(pair.second)) {
      if (blockNode->getBasicBlock() == bb) {
        icfgNode = blockNode;
        break;
      }
    }
  }

  auto *phiNode =
      new IntraMSSAPhiSVFGNode(nodeId, icfgNode, memReg, version, pts);
  svfg->addNode(phiNode);
  bbToMemPhi[bb][memReg] = nodeId;

  return nodeId;
}

bool SVFGBuilder::updateSVFG(SVFG *existingSVFG) {
  if (!existingSVFG || !icfg) {
    return false;
  }

  // For semantic fidelity we rebuild the graph and atomically swap contents.
  std::unique_ptr<SVFG> rebuilt(build(icfg, config));
  if (!rebuilt) {
    return false;
  }
  existingSVFG->swapWith(*rebuilt);
  lastBuiltSVFG = existingSVFG;
  return true;
}

void SVFGBuilder::updateMemorySSAEdges(SVFG *svfg) {
  if (!svfg || !config.buildMSSA) {
    return;
  }

  // Keep update behavior semantically equivalent to the full builder.
  (void)updateSVFG(svfg);
}

bool SVFGBuilder::mayModifyMemory(const Function *F) {
  std::unordered_set<const Function *> visited;
  return mayModifyMemory(F, visited);
}

bool SVFGBuilder::mayReadMemory(const Function *F) {
  std::unordered_set<const Function *> visited;
  return mayReadMemory(F, visited);
}

bool SVFGBuilder::mayReadMemory(const Function *F,
                                std::unordered_set<const Function *> &visited) {
  if (!F)
    return true;
  if (!visited.insert(F).second) {
    // Break recursion cycles conservatively.
    return true;
  }

  if (F->doesNotAccessMemory() || F->hasFnAttribute(Attribute::ReadNone))
    return false;
  if (F->hasFnAttribute(Attribute::WriteOnly))
    return false;

  if (F->isDeclaration()) {
    return true;
  }

  for (const BasicBlock &bb : *F) {
    for (const Instruction &inst : bb) {
      if (isa<LoadInst>(&inst) || isa<AtomicRMWInst>(&inst) ||
          isa<AtomicCmpXchgInst>(&inst)) {
        return true;
      }

      if (const auto *call = dyn_cast<CallBase>(&inst)) {
        if (const auto *intrinsic = dyn_cast<IntrinsicInst>(call)) {
          switch (intrinsic->getIntrinsicID()) {
          case Intrinsic::dbg_value:
          case Intrinsic::dbg_declare:
          case Intrinsic::dbg_label:
          case Intrinsic::lifetime_start:
          case Intrinsic::lifetime_end:
          case Intrinsic::invariant_start:
          case Intrinsic::invariant_end:
          case Intrinsic::memset:
            continue;
          default:
            return true;
          }
        }

        const Function *callee = call->getCalledFunction();
        if (!callee)
          return true;
        if (mayReadMemory(callee, visited))
          return true;
      }
    }
  }

  return false;
}

bool SVFGBuilder::mayModifyMemory(
    const Function *F, std::unordered_set<const Function *> &visited) {
  if (!F)
    return true; // Conservative: assume unknown functions modify memory
  if (!visited.insert(F).second) {
    // Break recursion cycles conservatively.
    return true;
  }

  // External/declaration functions: check LLVM attributes
  if (F->isDeclaration()) {
    // Check LLVM function attributes for memory behavior
    if (F->onlyReadsMemory()) {
      return false; // Known to only read memory
    }
    if (F->doesNotAccessMemory()) {
      return false; // Known to not access memory
    }
    // Conservative: assume external functions may modify memory
    // unless explicitly marked otherwise
    return true;
  }

  // For defined functions, check if they have any store instructions
  // or calls that might modify memory
  for (const BasicBlock &bb : *F) {
    for (const Instruction &inst : bb) {
      // Check for store instructions
      if (isa<StoreInst>(&inst)) {
        return true;
      }

      // Check for atomic operations that modify memory
      if (isa<AtomicRMWInst>(&inst) || isa<AtomicCmpXchgInst>(&inst)) {
        return true;
      }

      // Check for calls that might modify memory
      if (const CallBase *call = dyn_cast<CallBase>(&inst)) {
        // Skip LLVM intrinsics that don't modify memory
        if (const IntrinsicInst *intrinsic = dyn_cast<IntrinsicInst>(call)) {
          Intrinsic::ID id = intrinsic->getIntrinsicID();
          switch (id) {
          case Intrinsic::dbg_value:
          case Intrinsic::dbg_declare:
          case Intrinsic::dbg_label:
          case Intrinsic::lifetime_start:
          case Intrinsic::lifetime_end:
          case Intrinsic::invariant_start:
          case Intrinsic::invariant_end:
            continue; // These don't modify memory
          default:
            // Other intrinsics might modify memory
            return true;
          }
        }

        const Function *callee = call->getCalledFunction();
        if (!callee) {
          // Indirect call - conservative: assume it might modify
          return true;
        }

        if (callee->isDeclaration()) {
          // External function - check attributes
          if (callee->onlyReadsMemory() || callee->doesNotAccessMemory()) {
            continue; // Known to not modify memory
          }
          // Conservative: assume it might modify
          return true;
        }

        // Recursive check: if callee modifies memory, so does caller
        if (mayModifyMemory(callee, visited)) {
          return true;
        }
      }
    }
  }

  return false;
}

std::vector<const Function *> SVFGBuilder::getRootFunctionsFromICFG() const {
  std::vector<const Function *> roots;
  if (!icfg)
    return roots;
  const GlobalInitBlockNode *globalInit =
      const_cast<ICFG *>(icfg)->getGlobalInitICFGNode();
  if (!globalInit)
    return roots;
  std::unordered_set<const Function *> seen;
  for (const ICFGEdge *edge : globalInit->getOutEdges()) {
    const ICFGNode *dst = edge ? edge->getDstNode() : nullptr;
    const auto *entry = dyn_cast_or_null<FunEntryBlockNode>(dst);
    const Function *F = entry ? entry->getFunction() : nullptr;
    if (F && seen.insert(F).second)
      roots.push_back(F);
  }
  return roots;
}

static bool isAliasValidationCall(const CallBase *call) {
  const Function *callee = call ? call->getCalledFunction() : nullptr;
  if (!callee)
    return false;
  const StringRef name = callee->getName();
  return name == "__aser_alias__" || name == "__aser_no_alias__" ||
         name.startswith("EXPECTEDFAIL_MAYALIAS") ||
         name.startswith("EXPECTEDFAIL_NOALIAS");
}

bool SVFGBuilder::callMayReadMemory(const CallBase *call) {
  if (!call)
    return true;
  if (isAliasValidationCall(call))
    return false;

  if (call->doesNotAccessMemory() || call->hasFnAttr(Attribute::ReadNone))
    return false;
  if (call->hasFnAttr(Attribute::WriteOnly))
    return false;

  if (const Function *callee = call->getCalledFunction()) {
    if (callee->doesNotAccessMemory() ||
        callee->hasFnAttribute(Attribute::ReadNone))
      return false;
    if (callee->hasFnAttribute(Attribute::WriteOnly))
      return false;
    if (!callee->isDeclaration())
      return mayReadMemory(callee);
  }

  return true;
}

bool SVFGBuilder::callMayModifyMemory(const CallBase *call) {
  if (!call)
    return true;
  if (isAliasValidationCall(call))
    return false;

  if (call->doesNotAccessMemory() || call->onlyReadsMemory() ||
      call->hasFnAttr(Attribute::ReadNone) ||
      call->hasFnAttr(Attribute::ReadOnly)) {
    return false;
  }

  if (const Function *callee = call->getCalledFunction()) {
    if (callee->doesNotAccessMemory() || callee->onlyReadsMemory() ||
        callee->hasFnAttribute(Attribute::ReadNone) ||
        callee->hasFnAttribute(Attribute::ReadOnly)) {
      return false;
    }
    if (!callee->isDeclaration())
      return mayModifyMemory(callee);
  }

  return true;
}

bool SVFGBuilder::callArgMayReadMemory(const CallBase *call,
                                       unsigned argNo) const {
  if (!call)
    return true;
  if (call->paramHasAttr(argNo, Attribute::ReadNone) ||
      call->paramHasAttr(argNo, Attribute::WriteOnly)) {
    return false;
  }
  return true;
}

bool SVFGBuilder::callArgMayModifyMemory(const CallBase *call,
                                         unsigned argNo) const {
  if (!call)
    return true;
  if (call->paramHasAttr(argNo, Attribute::ReadNone) ||
      call->paramHasAttr(argNo, Attribute::ReadOnly)) {
    return false;
  }
  return true;
}

uint32_t SVFGBuilder::nextVersion(const Function *F, uint32_t memReg) {
  uint32_t &version = memRegVersion[F][memReg];
  const uint32_t current = version;

  if (version != std::numeric_limits<uint32_t>::max()) {
    ++version;
  }

  // Optional cap: callers can request a finite cap; default is unbounded.
  if (config.maxSSAVersion != std::numeric_limits<uint32_t>::max() &&
      version > config.maxSSAVersion) {
    version = config.maxSSAVersion;
  }

  return current;
}
