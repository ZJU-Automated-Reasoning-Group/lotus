//===- SVFGBuilderNodes.cpp -- SVFG Node Building Implementation
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
// This file contains node building methods for SVFGBuilder
//
//===----------------------------------------------------------------------===//

#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFGBuilder.h"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

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

static const ICFGNode *getFunctionEntryICFGNode(const ICFG *icfg,
                                                const Function *F) {
  if (!icfg || !F || F->isDeclaration())
    return nullptr;
  return const_cast<ICFG *>(icfg)->getFunEntryICFGNode(F);
}

static const ICFGNode *getFunctionExitICFGNode(const ICFG *icfg,
                                               const Function *F) {
  if (!icfg || !F || F->isDeclaration())
    return nullptr;
  return const_cast<ICFG *>(icfg)->getFunExitICFGNode(F);
}

static const ICFGNode *getReturnSiteICFGNode(const ICFG *icfg,
                                             const CallBase *call) {
  if (!icfg || !call)
    return nullptr;
  return const_cast<ICFG *>(icfg)->getRetICFGNode(call);
}

void SVFGBuilder::buildNodes() {
  if (const Module *M = getModuleFromICFG(icfg)) {
    for (const Function &F : *M) {
      (void)getOrCreateCanonicalObjectIdForValue(
          &F, SVFG::ObjectInfo{false, false, false, true, false, false, false,
                               false, 0});
    }
    for (const GlobalVariable &GV : M->globals()) {
      SVFG::ObjectInfo info;
      info.isGlobal = true;
      info.isConstant = GV.isConstant();
      (void)getOrCreateCanonicalObjectIdForValue(&GV, info);
    }
  }
  buildTopLevelNodes();
  buildAddressTakenNodes();
  buildFormalParmNodes();
  buildActualParmNodes();
  buildFormalRetNodes();
  buildActualRetNodes();
  refreshStmtPointerNodeIds();
}

void SVFGBuilder::buildTopLevelNodes() {
  // Use a single NullPtr node for the module (ConstantPointerNull is uniqued).
  uint32_t nullPtrNodeId = std::numeric_limits<uint32_t>::max();

  auto ensureBaseObjIdForValue = [&](const Value *v,
                                     SVFG::ObjectInfo info) -> uint32_t {
    return getOrCreateCanonicalObjectIdForValue(v, info);
  };

  auto ensureAddrNodeForConstPtr = [&](const Value *v,
                                       IntraBlockNode *at) -> uint32_t {
    if (!v)
      return std::numeric_limits<uint32_t>::max();
    auto it = valueToNode.find(v);
    if (it != valueToNode.end())
      return it->second;
    const bool isConstPtrTarget =
        isa<Function>(v) || isa<GlobalVariable>(v) || isa<GlobalAlias>(v);
    if (!isConstPtrTarget)
      return std::numeric_limits<uint32_t>::max();

    // Also register a base object ID for this constant so DDA can seed
    // points-to sets even when PTA queries return empty (e.g., in minimal IR
    // snippets).
    SVFG::ObjectInfo info;
    info.isFunction = isa<Function>(v);
    info.isGlobal = isa<GlobalValue>(v) && !isa<Function>(v);
    uint32_t baseObjId = ensureBaseObjIdForValue(v, info);

    const uint32_t nodeId = nextNode();
    auto *addrNode = new AddrSVFGNode(nodeId, at, v);
    addrNode->setValueId(getOrCreateValueId(v));
    // Set object ID on AddrSVFGNode (mirrors SVF's getPAGSrcNodeID).
    if (baseObjId != 0)
      addrNode->setObjectId(baseObjId);
    svfg->addNode(addrNode);
    valueToNode.emplace(v, nodeId);
    svfg->setValueNode(v, nodeId);
    return nodeId;
  };

  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      // Always scan instruction operands for pointer constants/nulls so they
      // get stable Addr/Null nodes even when the instruction itself does not
      // produce a pointer result (e.g. `call void @caller(..., @fp)`).
      for (const Use &op : inst.operands()) {
        const Value *opVal = op.get();
        if (!isa<ConstantPointerNull>(opVal)) {
          if (opVal && opVal->getType()->isPointerTy()) {
            const Value *canon = opVal->stripPointerCasts();
            const uint32_t canonId =
                ensureAddrNodeForConstPtr(canon, blockNode);
            if (canonId != std::numeric_limits<uint32_t>::max()) {
              valueToNode.emplace(opVal, canonId);
              svfg->setValueNode(opVal, canonId);
            }
          }
          continue;
        }
        if (nullPtrNodeId == std::numeric_limits<uint32_t>::max()) {
          nullPtrNodeId = nextNode();
          auto *nullNode = new NullPtrSVFGNode(nullPtrNodeId, blockNode);
          svfg->addNode(nullNode);
        }
        valueToNode.emplace(opVal, nullPtrNodeId);
        svfg->setValueNode(opVal, nullPtrNodeId);
      }

      const bool isStore = isa<StoreInst>(&inst);
      const bool isPhi = isa<PHINode>(&inst);
      const bool isSelect = isa<SelectInst>(&inst);
      const bool isCmp = isa<CmpInst>(&inst);
      const bool isBranch = isa<BranchInst>(&inst);
      const bool isBinary = isa<BinaryOperator>(&inst);
      const bool isCall = isa<CallBase>(&inst);
      // UnaryOperator covers fneg; CastInst covers bitcast/trunc/zext/inttoptr
      // etc.  Both can carry pointer-type results, so they must be considered.
      const bool isUnary = isa<UnaryOperator>(&inst) || isa<CastInst>(&inst);
      const bool hasPointerResult = inst.getType()->isPointerTy();
      if (!hasPointerResult && !isStore && !isCmp && !isBranch && !isBinary &&
          !isUnary)
        continue;

      if (isa<AllocaInst>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *addrNode = new AddrSVFGNode(nodeId, blockNode, &inst);
        addrNode->setValueId(getOrCreateValueId(&inst));
        svfg->addNode(addrNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
        getOrCreateMemReg(cast<AllocaInst>(&inst));
        SVFG::ObjectInfo info;
        info.isStack = true;
        uint32_t baseObjId = ensureBaseObjIdForValue(&inst, info);
        // Set object ID on AddrSVFGNode (mirrors SVF's getPAGSrcNodeID).
        // Use the base object ID which is also used for edge guard population.
        if (baseObjId != 0)
          addrNode->setObjectId(baseObjId);
        if (config.usePointerAnalysis)
          (void)getObjectIdsForValue(&inst);
      } else if (isHeapAllocation(&inst)) {
        uint32_t nodeId = nextNode();
        auto *addrNode = new AddrSVFGNode(nodeId, blockNode, &inst);
        addrNode->setValueId(getOrCreateValueId(&inst));
        svfg->addNode(addrNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
        (void)getOrCreateMemReg(&inst);
        SVFG::ObjectInfo info;
        info.isHeap = true;
        uint32_t baseObjId = ensureBaseObjIdForValue(&inst, info);
        // Set object ID on AddrSVFGNode (mirrors SVF's getPAGSrcNodeID).
        if (baseObjId != 0)
          addrNode->setObjectId(baseObjId);
        if (config.usePointerAnalysis)
          (void)getObjectIdsForValue(&inst);
      } else if (const LoadInst *load = dyn_cast<LoadInst>(&inst)) {
        auto ptrIt = valueToNode.find(load->getPointerOperand());
        uint32_t ptrNodeId = (ptrIt != valueToNode.end())
                                 ? ptrIt->second
                                 : std::numeric_limits<uint32_t>::max();
        uint32_t nodeId = nextNode();
        auto *loadNode = new LoadSVFGNode(nodeId, blockNode, &inst, ptrNodeId);
        loadNode->setValueId(getOrCreateValueId(&inst));
        svfg->addNode(loadNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
        loadToLoadNode[load] = nodeId;
      } else if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
        auto ptrIt = valueToNode.find(store->getPointerOperand());
        uint32_t ptrNodeId = (ptrIt != valueToNode.end())
                                 ? ptrIt->second
                                 : std::numeric_limits<uint32_t>::max();
        uint32_t nodeId = nextNode();
        auto *storeNode =
            new StoreSVFGNode(nodeId, blockNode, &inst, ptrNodeId);
        svfg->addNode(storeNode);
        svfg->setDef(&inst, nodeId);
        storeToStoreNode[store] = nodeId;

        // Track ordinary stores to globals for clients such as SABER. These are
        // no longer used to seed program entry; global initialization is modeled
        // via the synthetic ICFG global-init node.
        const Value *ptrOp = store->getPointerOperand();
        if (isa<GlobalVariable>(ptrOp) ||
            (isa<GetElementPtrInst>(ptrOp) &&
             isa<GlobalVariable>(
                 cast<GetElementPtrInst>(ptrOp)->getPointerOperand()))) {
          svfg->addGlobalStoreNode(storeNode);
        }
      } else if (isa<GetElementPtrInst>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *gepNode = new GepSVFGNode(nodeId, blockNode, &inst);
        gepNode->setValueId(getOrCreateValueId(&inst));
        svfg->addNode(gepNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else if (isa<BinaryOperator>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *binaryNode = new BinaryOpSVFGNode(nodeId, blockNode, &inst);
        binaryNode->setValueId(getOrCreateValueId(&inst));
        // Record operand values (matching SVF's OPVers).
        for (unsigned i = 0, e = inst.getNumOperands(); i < e; ++i) {
          binaryNode->setOpVer(i, inst.getOperand(i));
        }
        svfg->addNode(binaryNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else if (isa<CmpInst>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *cmpNode = new CmpSVFGNode(nodeId, blockNode, &inst);
        cmpNode->setValueId(getOrCreateValueId(&inst));
        // Record operand values (matching SVF's OPVers).
        for (unsigned i = 0, e = inst.getNumOperands(); i < e; ++i) {
          cmpNode->setOpVer(i, inst.getOperand(i));
        }
        svfg->addNode(cmpNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else if (isa<BranchInst>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *branchNode = new BranchSVFGNode(nodeId, blockNode, &inst);
        svfg->addNode(branchNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else if (isa<ConstantPointerNull>(&inst)) {
        // Create null pointer node for null constant instructions
        uint32_t nodeId = nextNode();
        auto *nullNode = new NullPtrSVFGNode(nodeId, blockNode);
        svfg->addNode(nullNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else if (isPhi || isSelect) {
        uint32_t nodeId = nextNode();
        auto *phiNode = new IntraPhiSVFGNode(nodeId, blockNode, &inst);
        phiNode->setValueId(getOrCreateValueId(&inst));
        if (const auto *phi = dyn_cast<PHINode>(&inst)) {
          // Record incoming values as operands (matching SVF's OPVers).
          for (unsigned i = 0, e = phi->getNumIncomingValues(); i < e; ++i) {
            phiNode->setOpVer(i, phi->getIncomingValue(i));
          }
        } else {
          // Model `select` as a PHI-like merge, matching upstream VFG handling.
          for (unsigned i = 0, e = inst.getNumOperands(); i < e; ++i) {
            phiNode->setOpVer(i, inst.getOperand(i));
          }
        }
        svfg->addNode(phiNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else if (isCall && hasPointerResult) {
        // Calls produce value-flow through ActualRet/FormalRet; keep a result
        // anchor node for the SSA value, but do not model the call as a generic
        // operand-copy instruction.
        uint32_t nodeId = nextNode();
        auto *callResultNode = new CopySVFGNode(nodeId, blockNode, &inst);
        callResultNode->setValueId(getOrCreateValueId(&inst));
        svfg->addNode(callResultNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else if (isa<CastInst>(&inst) || isa<UnaryOperator>(&inst)) {
        // Mirrors SVF's UnaryOpVFGNode (bitcast, trunc, zext, sext, fpext,
        // inttoptr, ptrtoint, addrspacecast, fneg …).
        uint32_t nodeId = nextNode();
        auto *unaryNode = new UnaryOpSVFGNode(nodeId, blockNode, &inst);
        unaryNode->setValueId(getOrCreateValueId(&inst));
        // Record single source operand at position 0 (matching SVF's OPVers).
        unaryNode->setOpVer(0, inst.getOperand(0));
        svfg->addNode(unaryNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else {
        // Generic copy/move for remaining instructions with pointer results.
        uint32_t nodeId = nextNode();
        auto *copyNode = new CopySVFGNode(nodeId, blockNode, &inst);
        copyNode->setValueId(getOrCreateValueId(&inst));
        svfg->addNode(copyNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      }
    }
  }
}

void SVFGBuilder::buildAddressTakenNodes() {
  if (!config.buildMSSA)
    return;

  // Handle address-taken variables using AserPTA
  std::unordered_set<const AllocaInst *> processedAllocas;
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      for (const Use &op : inst.operands()) {
        const Value *opVal = op.get();
        if (const AllocaInst *alloca = dyn_cast<AllocaInst>(opVal)) {
          if (!processedAllocas.insert(alloca).second)
            continue;

          bool addressTaken = false;
          for (const Use &use : alloca->uses()) {
            if (!isa<LoadInst>(use.getUser())) {
              addressTaken = true;
              break;
            }
          }

          if (addressTaken) {
            std::vector<const void *> ptsVoid = getPointsToSet(alloca);
            SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);

            const Function *entryFunc = alloca->getParent()->getParent();
            if (objIds.empty()) {
              const uint32_t memReg = getOrCreateMemReg(alloca);
              funcEntryChiMemRegs[entryFunc].insert(memReg);
            } else {
              funcEntryChiMemRegs[entryFunc].insert(
                  getOrCreateMemRegForPointsTo(objIds));
            }
          }
        }
      }
    }
  }

  // Handle globals (both pointer and non-pointer types if address-taken)
  const Module *M = getModuleFromICFG(icfg);
  if (M && config.includeGlobals) {
    for (const GlobalVariable &gv : M->globals()) {
      bool addressTaken = false;
      for (const User *user : gv.users()) {
        if (isa<Instruction>(user)) {
          addressTaken = true;
          break;
        }
        if (const ConstantExpr *ce = dyn_cast<ConstantExpr>(user)) {
          if (ce->getOpcode() == Instruction::GetElementPtr ||
              ce->getOpcode() == Instruction::BitCast ||
              ce->getOpcode() == Instruction::AddrSpaceCast) {
            addressTaken = true;
            break;
          }
        }
      }

      if (addressTaken) {
        std::vector<const void *> ptsVoid = getPointsToSet(&gv);
        SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);

        if (objIds.empty()) {
          const uint32_t memReg = getOrCreateMemReg(&gv);
          globalEntryRegions[memReg] = SVFGNodeBS{getOrCreateUnknownObjId()};
        } else {
          const uint32_t memReg = getOrCreateMemRegForPointsTo(objIds);
          globalEntryRegions[memReg] = objIds;
        }

        for (const Function *entryFunc : getRootFunctionsFromICFG()) {
          if (objIds.empty()) {
            const uint32_t memReg = getOrCreateMemReg(&gv);
            funcEntryChiMemRegs[entryFunc].insert(memReg);
          } else {
            funcEntryChiMemRegs[entryFunc].insert(
                getOrCreateMemRegForPointsTo(objIds));
          }
        }
      }
    }
  }
}

void SVFGBuilder::buildFormalParmNodes() {
  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    unsigned idx = 0;
    for (const auto *argIt = F.arg_begin(); argIt != F.arg_end();
         ++argIt, ++idx) {
      const Argument *arg = &*argIt;
      if (!arg->getType()->isPointerTy())
        continue;

      uint32_t nodeId = nextNode();
      auto *formalParm = new FormalParmSVFGNode(
          nodeId, getFunctionEntryICFGNode(icfg, &F), &F, idx, arg);
      formalParm->setValueId(getOrCreateValueId(arg));
      svfg->addNode(formalParm);
      svfg->addFormalParm(&F, formalParm);
      valueToNode[arg] = nodeId;
      svfg->setValueNode(arg, nodeId);

      if (!config.buildMSSA)
        continue;

      std::vector<const void *> ptsVoid = getPointsToSet(arg);
      SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);

      auto &memRegsForArg = argToMemRegs[arg];
      if (objIds.empty()) {
        // Keep the argument-to-region mapping even when MemorySSA nodes are
        // synthesized later from actual ref/mod summaries.
        memRegsForArg.push_back(getOrCreateMemReg(arg));
        continue;
      }

      memRegsForArg.push_back(getOrCreateMemRegForPointsTo(objIds));
    }

    // Create VarArgSVFGNode for variadic functions
    if (F.isVarArg()) {
      uint32_t varArgNodeId = nextNode();
      auto *varArgNode = new VarArgSVFGNode(
          varArgNodeId, getFunctionEntryICFGNode(icfg, &F), &F);
      varArgNode->setValueId(getOrCreateVarArgValueId(&F));
      svfg->addNode(varArgNode);
      svfg->addFormalParm(&F, varArgNode); // Treat as a formal parameter
      // Note: valueToNode mapping is not needed here since vararg has no
      // corresponding LLVM Argument. Call sites connect their extra args
      // directly to this node via ActualParm → VarArg edges.
    }
  }
}

void SVFGBuilder::buildActualParmNodes() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      const CallBase *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;

      // Register (funPtrNodeId -> callsites) for SVF-style on-the-fly
      // indirect-call refinement. Use the called operand's SVFG node id as the
      // "funPtr" key.
      if (!call->getCalledFunction()) {
        const Value *calledOp = call->getCalledOperand();
        if (calledOp)
          calledOp = calledOp->stripPointerCasts();
        uint32_t funPtrNodeId = std::numeric_limits<uint32_t>::max();
        if (calledOp) {
          if (SVFGNode *vn = svfg->getValueNode(calledOp)) {
            funPtrNodeId = vn->hasValueId() ? vn->getValueId() : vn->getId();
          } else if (const auto *ci = dyn_cast<Instruction>(calledOp)) {
            if (SVFGNode *dn = svfg->getDef(ci))
              funPtrNodeId = dn->hasValueId() ? dn->getValueId() : dn->getId();
          }
        }
        if (funPtrNodeId != std::numeric_limits<uint32_t>::max())
          svfg->addIndCallSite(funPtrNodeId, call);
      }

      // Create ActualParm nodes (one per pointer argument).
      unsigned idx = 0;
      const unsigned numArgs = call->arg_size();
      for (unsigned i = 0; i < numArgs; ++i, ++idx) {
        const Value *argVal = call->getArgOperand(i);
        if (!argVal->getType()->isPointerTy())
          continue;

        uint32_t nodeId = nextNode();
        auto *actualParm =
            new ActualParmSVFGNode(nodeId, blockNode, call, idx, argVal);
        actualParm->setValueId(getOrCreateValueId(argVal));
        svfg->addNode(actualParm);
        svfg->addActualParm(call, actualParm);
        auto argNodeIt = valueToNode.find(argVal);
        if (argNodeIt != valueToNode.end()) {
          if (SVFGNode *argNode = svfg->getNode(argNodeIt->second)) {
            svfg->addEdge(argNode, actualParm, SVFGEdgeK::IntraCopy);
          }
        }
      }

      if (!config.buildMSSA)
        continue;
    }
  }
}

void SVFGBuilder::buildFormalRetNodes() {
  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    // Check if function returns a pointer type
    if (!F.getReturnType()->isPointerTy())
      continue;

    const llvm::Value *formalRetAnchor = nullptr;
    for (const BasicBlock &bb : F) {
      if (const auto *ret = dyn_cast<ReturnInst>(bb.getTerminator())) {
        const Value *retVal = ret->getReturnValue();
        if (retVal && retVal->getType()->isPointerTy()) {
          formalRetAnchor = retVal;
          break;
        }
      }
    }

    // Create formal return node (one per function, not per return statement)
    uint32_t nodeId = nextNode();
    auto *formalRet = new FormalRetSVFGNode(
        nodeId, getFunctionExitICFGNode(icfg, &F), &F, formalRetAnchor);
    formalRet->setValueId(getOrCreateFormalRetValueId(&F));
    svfg->addNode(formalRet);
    svfg->addFormalRet(&F, formalRet);

    // Connect all return values to the formal return node
    for (const BasicBlock &bb : F) {
      if (const ReturnInst *ret = dyn_cast<ReturnInst>(bb.getTerminator())) {
        const Value *retVal = ret->getReturnValue();
        if (!retVal || !retVal->getType()->isPointerTy())
          continue;

        auto retValIt = valueToNode.find(retVal);
        if (retValIt != valueToNode.end()) {
          SVFGNode *retValNode = svfg->getNode(retValIt->second);
          if (retValNode) {
            svfg->addEdge(retValNode, formalRet, SVFGEdgeK::IntraCopy);
          }
        }
      }
    }
  }
}

void SVFGBuilder::buildActualRetNodes() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      const CallBase *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;

      // Handle call return value (if call result is used)
      if (call->getType()->isPointerTy()) {
        if (const Function *directCallee = call->getCalledFunction()) {
          if (directCallee->isDeclaration())
            continue;
        }

        uint32_t nodeId = nextNode();
        auto *actualRet =
            new ActualRetSVFGNode(nodeId, getReturnSiteICFGNode(icfg, call), call);
        actualRet->setValueId(getOrCreateValueId(call));
        svfg->addNode(actualRet);
        svfg->addActualRet(call, actualRet);

        // Bridge interprocedural return flow into the call SSA value.
        auto callValueIt = valueToNode.find(call);
        if (callValueIt != valueToNode.end()) {
          if (SVFGNode *callValueNode = svfg->getNode(callValueIt->second)) {
            svfg->addEdge(actualRet, callValueNode, SVFGEdgeK::IntraCopy);
          }
        }
      }
    }
  }
}

void SVFGBuilder::refreshStmtPointerNodeIds() {
  if (!svfg)
    return;

  for (const auto &entry : loadToLoadNode) {
    const LoadInst *load = entry.first;
    auto *loadNode = dyn_cast_or_null<LoadSVFGNode>(svfg->getNode(entry.second));
    if (!loadNode)
      continue;
    const Value *ptr = load->getPointerOperand();
    auto it = valueToNode.find(ptr);
    if (it != valueToNode.end())
      loadNode->setLoadFromPtr(it->second);
  }

  for (const auto &entry : storeToStoreNode) {
    const StoreInst *store = entry.first;
    auto *storeNode =
        dyn_cast_or_null<StoreSVFGNode>(svfg->getNode(entry.second));
    if (!storeNode)
      continue;
    const Value *ptr = store->getPointerOperand();
    auto it = valueToNode.find(ptr);
    if (it != valueToNode.end())
      storeNode->setStoreToPtr(it->second);
  }
}
