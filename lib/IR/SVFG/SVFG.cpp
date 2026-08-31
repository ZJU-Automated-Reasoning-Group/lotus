//===- SVFG.cpp -- SVFG Implementation --------------------------------------//
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

#include "IR/SVFG/SVFG.h"

#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"
#include "IR/SVFG/SVFGSerializer.h"

#include <queue>

#include <llvm/Support/Casting.h>

using namespace lotus::analysis;
using namespace llvm;

// Forward declarations for stat helpers defined later in this file.
static void adjustNodeStat(SVFGStat &stat, SVFGNode *node, int delta);
static void adjustEdgeStat(SVFGStat &stat, SVFGEdge *edge, int delta);

const llvm::Function *SVFGNode::getFunction() const {
  if (icfgNode) {
    return icfgNode->getFunction();
  }
  return nullptr;
}

std::string SVFGNode::toString() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  os << "SVFGNode ID: " << getId()
     << " Kind: " << static_cast<uint32_t>(getNodeKind());
  if (const auto *val = getValue()) {
    os << " Value: " << val->getName().str();
  }
  return os.str();
}

std::string SVFGEdge::toString() const {
  std::string result;
  switch (kind) {
  case SVFGEdgeK::IntraCopy:
    result = "IntraCopy";
    break;
  case SVFGEdgeK::IntraDirect:
    result = "IntraDirect";
    break;
  case SVFGEdgeK::IntraLoad:
    result = "IntraLoad";
    break;
  case SVFGEdgeK::IntraStore:
    result = "IntraStore";
    break;
  case SVFGEdgeK::IntraGep:
    result = "IntraGep";
    break;
  case SVFGEdgeK::IntraPhi:
    result = "IntraPhi";
    break;
  case SVFGEdgeK::IntraCmp:
    result = "IntraCmp";
    break;
  case SVFGEdgeK::IntraBranch:
    result = "IntraBranch";
    break;
  case SVFGEdgeK::IntraMu:
    result = "IntraMu";
    break;
  case SVFGEdgeK::IntraChi:
    result = "IntraChi";
    break;
  case SVFGEdgeK::CallMu:
    result = "CallMu";
    break;
  case SVFGEdgeK::CallChi:
    result = "CallChi";
    break;
  case SVFGEdgeK::RetMu:
    result = "RetMu";
    break;
  case SVFGEdgeK::EntryChi:
    result = "EntryChi";
    break;
  case SVFGEdgeK::CallDir:
    result = "CallDir";
    break;
  case SVFGEdgeK::CallInd:
    result = "CallInd";
    break;
  case SVFGEdgeK::CallAIn:
    result = "CallAIn";
    break;
  case SVFGEdgeK::CallFIn:
    result = "CallFIn";
    break;
  case SVFGEdgeK::ParamCall:
    result = "ParamCall";
    break;
  case SVFGEdgeK::RetDir:
    result = "RetDir";
    break;
  case SVFGEdgeK::RetInd:
    result = "RetInd";
    break;
  case SVFGEdgeK::RetAOut:
    result = "RetAOut";
    break;
  case SVFGEdgeK::RetFOut:
    result = "RetFOut";
    break;
  case SVFGEdgeK::ParamRet:
    result = "ParamRet";
    break;
  case SVFGEdgeK::IntraIndirect:
    result = "IntraIndirect";
    break;
  case SVFGEdgeK::ThreadMHPIndirectVF:
    result = "ThreadMHPIndirectVF";
    break;
  case SVFGEdgeK::Variant:
    result = "Variant";
    break;
  default:
    result = "Unknown";
    break;
  }
  return result;
}

void SVFG::addNode(SVFGNode *node) {
  nodeMap[node->getId()] = node;
  if (node->getId() >= nextNodeId) {
    nextNodeId = node->getId() + 1;
  }
  if (node->hasValueId())
    valueIdToDefNodeMap.emplace(node->getValueId(), node->getId());
  if (node->isMemNode() && node->getMemReg() != 0 &&
      isMemDefSVFGNode(node->getNodeKind())) {
    setMSSADef(node->getMemReg(), node, node->getSSAVersion());
  }

  // Maintain instruction indices for MemorySSA nodes.
  if (auto *mu = dyn_cast<LoadMuSVFGNode>(node)) {
    if (const llvm::LoadInst *li = mu->getLoadInst())
      loadInstToMuMap[li].insert(mu);
  } else if (auto *chi = dyn_cast<StoreChiSVFGNode>(node)) {
    if (const llvm::StoreInst *si = chi->getStoreInst())
      storeInstToChiMap[si].insert(chi);
  }

  updateStat(node);
}

SVFGEdge *SVFG::addEdge(SVFGNode *src, SVFGNode *dst, SVFGEdgeK kind,
                        const llvm::CallBase *callSite,
                        const SVFGNodeBS &pointsTo, std::string callSiteDebug) {
  if (!src || !dst)
    return nullptr;

  if (callSiteDebug.empty() && callSite) {
    llvm::raw_string_ostream os(callSiteDebug);
    os << callSite->getFunction()->getName();
    if (const Function *callee = callSite->getCalledFunction()) {
      os << "->" << callee->getName();
    } else {
      os << "->ind";
    }
    if (const DebugLoc &dl = callSite->getDebugLoc()) {
      os << "@" << dl.getLine() << ":" << dl.getCol();
    }
  }

  for (auto *existing : src->getOutEdges()) {
    if (existing->getDstNode() == dst && existing->getEdgeKind() == kind &&
        existing->getCallSite() == callSite) {
      existing->addPointsTo(pointsTo);
      // Opportunistically fill in the debug string if it was missing.
      if (existing->getCallSiteDebug().empty() && !callSiteDebug.empty()) {
        existing->setCallSiteDebug(callSiteDebug);
      }
      return existing;
    }
  }

  SVFGEdge *edge = new SVFGEdge(src, dst, kind, SVFGEdge::EdgeWeight::One,
                                callSite, std::move(callSiteDebug), pointsTo);
  src->addOutEdge(edge);
  dst->addInEdge(edge);
  updateStat(edge);
  return edge;
}

void SVFG::removeEdge(SVFGEdge *edge) {
  if (!edge)
    return;

  adjustEdgeStat(stat, edge, -1);

  SVFGNode *src = edge->getSrcNode();
  SVFGNode *dst = edge->getDstNode();

  if (src)
    src->removeOutEdge(edge);
  if (dst)
    dst->removeInEdge(edge);

  delete edge;
}

void SVFG::setObjectValue(uint32_t objId, const llvm::Value *v) {
  if (objId == 0 || !v)
    return;
  objIdToValue[objId] = v;
  valueToObjIds[v].insert(objId);
  // Preserve the first reverse mapping to avoid clobbering a canonical base
  // object with a later refined alias to the same abstract object.
  if (valueToObjId.find(v) == valueToObjId.end())
    valueToObjId[v] = objId;
}

const llvm::Value *SVFG::getObjectValue(uint32_t objId) const {
  auto it = objIdToValue.find(objId);
  return (it != objIdToValue.end()) ? it->second : nullptr;
}

uint32_t SVFG::getObjectId(const llvm::Value *v) const {
  if (!v)
    return 0;
  auto it = valueToObjId.find(v);
  return (it != valueToObjId.end()) ? it->second : 0;
}

SVFGNode *SVFG::getCanonicalDefNodeForDDAId(uint32_t ddaId) const {
  if (SVFGNode *valueNode = getValueIdNode(ddaId))
    return valueNode;

  const llvm::Value *objValue = getObjectValue(ddaId);
  if (!objValue)
    return nullptr;

  if (SVFGNode *valueNode = getValueNode(objValue))
    return valueNode;

  if (const auto *inst = llvm::dyn_cast<llvm::Instruction>(objValue))
    return getDef(inst);

  return nullptr;
}

uint32_t SVFG::getCallSiteId(const llvm::CallBase *cs,
                             const llvm::Function *callee) const {
  if (!cs || !callee)
    return 0;
  if (!hasConnectedCallee(cs, callee))
    return 0;
  CallSiteCalleeKey key{cs, callee};
  auto it = callSiteCalleeToId.find(key);
  return (it != callSiteCalleeToId.end()) ? it->second : 0;
}

void SVFG::initializeRefinedCallGraph(Module &M) {
  refinedCallGraph = std::make_unique<LTCallGraph>(M);
}

void SVFG::removeNode(SVFGNode *node) {
  if (!node)
    return;

  const uint32_t nodeId = node->getId();
  const llvm::Value *value = node->getValue();
  const llvm::Instruction *inst = node->getInstruction();
  const llvm::Function *fun = node->getFunction();

  for (auto it = mssaVerToNodeMap.begin(); it != mssaVerToNodeMap.end();) {
    if (it->second == node) {
      it = mssaVerToNodeMap.erase(it);
    } else {
      ++it;
    }
  }

  if (value) {
    auto it = valueToNodeMap.find(value);
    if (it != valueToNodeMap.end() && it->second == nodeId) {
      valueToNodeMap.erase(it);
    }
  }

  if (node->hasValueId()) {
    auto it = valueIdToDefNodeMap.find(node->getValueId());
    if (it != valueIdToDefNodeMap.end() && it->second == nodeId)
      valueIdToDefNodeMap.erase(it);
  }

  if (inst) {
    auto it = instToDefMap.find(inst);
    if (it != instToDefMap.end() && it->second == nodeId) {
      instToDefMap.erase(it);
    }
  }

  nodeFunctionDebug.erase(nodeId);
  nodeCallSiteDebug.erase(nodeId);
  globalStoreNodes.erase(node);

  if (auto *mu = dyn_cast<LoadMuSVFGNode>(node)) {
    if (const llvm::LoadInst *li = mu->getLoadInst()) {
      auto muIt = loadInstToMuMap.find(li);
      if (muIt != loadInstToMuMap.end()) {
        muIt->second.erase(node);
        if (muIt->second.empty())
          loadInstToMuMap.erase(muIt);
      }
    }
  } else if (auto *chi = dyn_cast<StoreChiSVFGNode>(node)) {
    if (const llvm::StoreInst *si = chi->getStoreInst()) {
      auto chiIt = storeInstToChiMap.find(si);
      if (chiIt != storeInstToChiMap.end()) {
        chiIt->second.erase(node);
        if (chiIt->second.empty())
          storeInstToChiMap.erase(chiIt);
      }
    }
  }

  if (auto *actualIn = dyn_cast<ActualInSVFGNode>(node)) {
    const llvm::CallBase *cs = actualIn->getCallSite();
    if (cs) {
      auto mapIt = callSiteToActualInMap.find(cs);
      if (mapIt != callSiteToActualInMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          callSiteToActualInMap.erase(mapIt);
      }
    }
  } else if (auto *actualOut = dyn_cast<ActualOutSVFGNode>(node)) {
    const llvm::CallBase *cs = actualOut->getCallSite();
    if (cs) {
      auto mapIt = callSiteToActualOutMap.find(cs);
      if (mapIt != callSiteToActualOutMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          callSiteToActualOutMap.erase(mapIt);
      }
    }
  } else if (auto *actualParm = dyn_cast<ActualParmSVFGNode>(node)) {
    const llvm::CallBase *cs = actualParm->getCallSite();
    if (cs) {
      auto mapIt = callSiteToActualParmMap.find(cs);
      if (mapIt != callSiteToActualParmMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          callSiteToActualParmMap.erase(mapIt);
      }
    }
  } else if (auto *actualRet = dyn_cast<ActualRetSVFGNode>(node)) {
    const llvm::CallBase *cs = actualRet->getCallSite();
    if (cs) {
      auto mapIt = callSiteToActualRetMap.find(cs);
      if (mapIt != callSiteToActualRetMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          callSiteToActualRetMap.erase(mapIt);
      }
    }
  } else if (isa<FormalInSVFGNode>(node)) {
    if (fun) {
      auto mapIt = funcToFormalInMap.find(fun);
      if (mapIt != funcToFormalInMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          funcToFormalInMap.erase(mapIt);
      }
    }
  } else if (isa<FormalOutSVFGNode>(node)) {
    if (fun) {
      auto mapIt = funcToFormalOutMap.find(fun);
      if (mapIt != funcToFormalOutMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          funcToFormalOutMap.erase(mapIt);
      }
    }
  } else if (isa<FormalParmSVFGNode>(node)) {
    if (fun) {
      auto mapIt = funcToFormalParmMap.find(fun);
      if (mapIt != funcToFormalParmMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          funcToFormalParmMap.erase(mapIt);
      }
    }
  } else if (isa<FormalRetSVFGNode>(node)) {
    if (fun) {
      auto mapIt = funcToFormalRetMap.find(fun);
      if (mapIt != funcToFormalRetMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          funcToFormalRetMap.erase(mapIt);
      }
    }
  }

  // Remove incident edges before deleting the node.
  // Use a set to avoid double-removing self-loop edges.
  std::unordered_set<SVFGEdge *> incident;
  auto outEdges = node->getOutEdges();
  incident.insert(outEdges.begin(), outEdges.end());
  auto inEdges = node->getInEdges();
  incident.insert(inEdges.begin(), inEdges.end());
  for (SVFGEdge *edge : incident) {
    removeEdge(edge);
  }

  adjustNodeStat(stat, node, -1);

  nodeMap.erase(nodeId);
  nodesForUpdate.erase(node);
  delete node;
}

static void adjustNodeStat(SVFGStat &stat, SVFGNode *node, int delta) {
  if (!node)
    return;
  stat.numNodes += delta;
  switch (node->getNodeKind()) {
  case SVFGK::Addr:
    stat.numAddrNodes += delta;
    break;
  case SVFGK::Copy:
    stat.numCopyNodes += delta;
    break;
  case SVFGK::Load:
    stat.numLoadNodes += delta;
    break;
  case SVFGK::Store:
    stat.numStoreNodes += delta;
    break;
  case SVFGK::Gep:
    stat.numGepNodes += delta;
    break;
  case SVFGK::Phi:
  case SVFGK::IntraPhi:
  case SVFGK::InterPhi:
    stat.numPhiNodes += delta;
    break;
  case SVFGK::FormalIn:
  case SVFGK::FormalOut:
  case SVFGK::ActualIn:
  case SVFGK::ActualOut:
  case SVFGK::MPhi:
  case SVFGK::MIntraPhi:
  case SVFGK::MInterPhi:
  case SVFGK::LoadMu:
  case SVFGK::StoreChi:
  case SVFGK::CallMu:
  case SVFGK::CallChi:
  case SVFGK::RetMu:
  case SVFGK::EntryChi:
    stat.numMemNodes += delta;
    break;
  case SVFGK::FormalParm:
  case SVFGK::ActualParm:
  case SVFGK::FormalRet:
  case SVFGK::ActualRet:
    stat.numParamNodes += delta;
    break;
  default:
    break;
  }
}

static void adjustEdgeStat(SVFGStat &stat, SVFGEdge *edge, int delta) {
  if (!edge)
    return;
  stat.numEdges += delta;
  if (isCallVFGEdge(edge->getEdgeKind()))
    stat.numCallEdges += delta;
  else if (isRetVFGEdge(edge->getEdgeKind()))
    stat.numRetEdges += delta;
  else if (isIntraVFGEdge(edge->getEdgeKind()))
    stat.numIntraEdges += delta;
}

void SVFG::updateStat(SVFGNode *node) { adjustNodeStat(stat, node, +1); }
void SVFG::updateStat(SVFGEdge *edge) { adjustEdgeStat(stat, edge, +1); }

SVFGNodeSet SVFG::getPreds(SVFGNode *node) const {
  SVFGNodeSet result;
  if (!node)
    return result;

  SVFGNodeSet visited;
  visited.insert(node); // mark start as visited so we don't re-enqueue it
  std::queue<SVFGNode *> worklist;
  worklist.push(node);

  while (!worklist.empty()) {
    SVFGNode *current = worklist.front();
    worklist.pop();

    for (auto *edge : current->getInEdges()) {
      SVFGNode *pred = edge->getSrcNode();
      if (pred == node)
        continue; // skip back-edges to the start node
      if (visited.insert(pred).second) {
        result.insert(pred);
        worklist.push(pred);
      }
    }
  }

  return result;
}

SVFGNodeSet SVFG::getSuccs(SVFGNode *node) const {
  SVFGNodeSet result;
  if (!node)
    return result;

  SVFGNodeSet visited;
  visited.insert(node);
  std::queue<SVFGNode *> worklist;
  worklist.push(node);

  while (!worklist.empty()) {
    SVFGNode *current = worklist.front();
    worklist.pop();

    for (auto *edge : current->getOutEdges()) {
      SVFGNode *succ = edge->getDstNode();
      if (succ == node)
        continue; // skip back-edges to the start node
      if (visited.insert(succ).second) {
        result.insert(succ);
        worklist.push(succ);
      }
    }
  }

  return result;
}

bool SVFG::hasPath(SVFGNode *src, SVFGNode *dst) const {
  if (!src || !dst || src == dst)
    return src == dst;

  SVFGNodeSet visited;
  std::queue<SVFGNode *> worklist;
  worklist.push(src);
  visited.insert(src);

  while (!worklist.empty()) {
    SVFGNode *current = worklist.front();
    worklist.pop();

    if (current == dst)
      return true;

    for (auto *edge : current->getOutEdges()) {
      SVFGNode *succ = edge->getDstNode();
      if (visited.insert(succ).second) {
        worklist.push(succ);
      }
    }
  }

  return false;
}

SVFGEdge *SVFG::getIntraVFGEdge(const SVFGNode *src, const SVFGNode *dst,
                                SVFGEdgeK kind) const {
  if (!src || !dst)
    return nullptr;
  for (SVFGEdge *edge : dst->getInEdges()) {
    if (edge->getSrcNode() == src && edge->getEdgeKind() == kind)
      return edge;
  }
  return nullptr;
}

SVFGNode *SVFG::getLHSTopLevPtr(SVFGNode *node) const {
  if (!node)
    return nullptr;
  // For stmt/phi/param nodes the LHS (the value defined) is the node itself.
  if (node->isStmtNode() || node->isPhiNode() || node->isParamNode())
    return node;
  // For memory SSA nodes the "LHS" is the node (it defines a memory version).
  if (node->isMemNode())
    return node;
  return node;
}

const llvm::CallBase *SVFG::isCallSiteRetSVFGNode(const SVFGNode *n) const {
  if (!n)
    return nullptr;
  if (const auto *ar = llvm::dyn_cast_or_null<ActualRetSVFGNode>(n))
    return ar->getCallSite();
  if (const auto *phi = llvm::dyn_cast_or_null<InterPhiSVFGNode>(n))
    return phi->isActualRetPHI() ? phi->getCallSite() : nullptr;
  if (const auto *ao = llvm::dyn_cast_or_null<ActualOutSVFGNode>(n))
    return ao->getCallSite();
  if (n->getNodeKind() == SVFGK::MInterPhi)
    return n->getCallSite();
  return nullptr;
}

const llvm::Function *SVFG::isFunEntrySVFGNode(const SVFGNode *n) const {
  if (!n)
    return nullptr;
  if (const auto *fp = llvm::dyn_cast_or_null<FormalParmSVFGNode>(n))
    return fp->getFunction();
  if (const auto *phi = llvm::dyn_cast_or_null<InterPhiSVFGNode>(n))
    return phi->isFormalParmPHI() ? phi->getFunction() : nullptr;
  if (const auto *fi = llvm::dyn_cast_or_null<FormalInSVFGNode>(n))
    return fi->getFunction();
  if (n->getNodeKind() == SVFGK::MInterPhi)
    return n->getFunction();
  return nullptr;
}

void SVFG::getInterVFEdgesForIndirectCallSite(
    const llvm::CallBase *cs, const llvm::Function *callee,
    std::vector<SVFGEdge *> &edges) const {
  if (!cs || !callee)
    return;

  std::unordered_set<SVFGEdge *> seen;
  auto pushEdge = [&](SVFGEdge *edge) {
    if (edge && seen.insert(edge).second)
      edges.push_back(edge);
  };

  auto findInterEdge = [&](SVFGNode *src, SVFGNode *dst,
                           std::initializer_list<SVFGEdgeK> kinds) {
    if (!src || !dst)
      return;
    for (SVFGEdge *edge : src->getOutEdges()) {
      if (!edge || edge->getDstNode() != dst || edge->getCallSite() != cs)
        continue;
      if (std::find(kinds.begin(), kinds.end(), edge->getEdgeKind()) !=
          kinds.end()) {
        pushEdge(edge);
      }
    }
  };

  for (SVFGNode *actualParmNode : getActualParms(cs)) {
    auto *actualParm = dyn_cast<ActualParmSVFGNode>(actualParmNode);
    if (!actualParm)
      continue;
    const unsigned actualIdx = actualParm->getParamIndex();
    const bool isVarArgExtra =
        callee->isVarArg() && actualIdx >= callee->arg_size();
    for (SVFGNode *formalParmNode : getFormalParms(callee)) {
      if (isVarArgExtra) {
        if (!isa<VarArgSVFGNode>(formalParmNode))
          continue;
      } else {
        auto *formalParm = dyn_cast<FormalParmSVFGNode>(formalParmNode);
        if (!formalParm || formalParm->getParamIndex() != actualIdx)
          continue;
      }
      findInterEdge(
          actualParmNode, formalParmNode,
          {SVFGEdgeK::CallDir, SVFGEdgeK::CallInd, SVFGEdgeK::ParamCall});
    }
  }

  for (SVFGNode *formalRetNode : getFormalRets(callee)) {
    for (SVFGNode *actualRetNode : getActualRets(cs)) {
      findInterEdge(
          formalRetNode, actualRetNode,
          {SVFGEdgeK::RetDir, SVFGEdgeK::RetInd, SVFGEdgeK::ParamRet});
    }
  }

  for (SVFGNode *actualInNode : getActualIns(cs)) {
    for (SVFGEdge *edge : actualInNode->getOutEdges()) {
      if (!edge || edge->getCallSite() != cs)
        continue;
      if (edge->getEdgeKind() != SVFGEdgeK::CallAIn &&
          edge->getEdgeKind() != SVFGEdgeK::CallFIn)
        continue;
      if (edge->getDstNode() && edge->getDstNode()->getFunction() == callee)
        pushEdge(edge);
    }
  }

  for (SVFGNode *actualOutNode : getActualOuts(cs)) {
    for (SVFGEdge *edge : actualOutNode->getInEdges()) {
      if (!edge || edge->getCallSite() != cs)
        continue;
      if (edge->getEdgeKind() != SVFGEdgeK::RetAOut &&
          edge->getEdgeKind() != SVFGEdgeK::RetFOut)
        continue;
      if (edge->getSrcNode() && edge->getSrcNode()->getFunction() == callee)
        pushEdge(edge);
    }
  }
}

void SVFG::dump(const std::string &filename, bool simple) const {
  (void)SVFGSerializer::writeDot(*this, filename, simple);
}

bool SVFG::writeToFile(const std::string &filename) const {
  return SVFGSerializer::writeText(*this, filename);
}

bool SVFG::readFromFile(const std::string &filename) {
  return SVFGSerializer::readText(*this, filename);
}

void SVFG::printStat() const {
  llvm::errs() << "=== SVFG Statistics ===\n";
  llvm::errs() << "Total Nodes: " << stat.numNodes << "\n";
  llvm::errs() << "  Addr Nodes: " << stat.numAddrNodes << "\n";
  llvm::errs() << "  Copy Nodes: " << stat.numCopyNodes << "\n";
  llvm::errs() << "  Load Nodes: " << stat.numLoadNodes << "\n";
  llvm::errs() << "  Store Nodes: " << stat.numStoreNodes << "\n";
  llvm::errs() << "  Gep Nodes: " << stat.numGepNodes << "\n";
  llvm::errs() << "  Phi Nodes: " << stat.numPhiNodes << "\n";
  llvm::errs() << "  Memory Nodes: " << stat.numMemNodes << "\n";
  llvm::errs() << "  Param Nodes: " << stat.numParamNodes << "\n";
  llvm::errs() << "Total Edges: " << stat.numEdges << "\n";
  llvm::errs() << "  Intra Edges: " << stat.numIntraEdges << "\n";
  llvm::errs() << "  Call Edges: " << stat.numCallEdges << "\n";
  llvm::errs() << "  Ret Edges: " << stat.numRetEdges << "\n";
}

void SVFG::swapWith(SVFG &other) {
  using std::swap;
  swap(nodeMap, other.nodeMap);
  swap(instToDefMap, other.instToDefMap);
  swap(valueToNodeMap, other.valueToNodeMap);
  swap(valueIdToDefNodeMap, other.valueIdToDefNodeMap);
  swap(mssaVerToNodeMap, other.mssaVerToNodeMap);
  swap(callSiteToActualInMap, other.callSiteToActualInMap);
  swap(callSiteToActualOutMap, other.callSiteToActualOutMap);
  swap(callSiteToActualParmMap, other.callSiteToActualParmMap);
  swap(callSiteToActualRetMap, other.callSiteToActualRetMap);
  swap(funcToFormalInMap, other.funcToFormalInMap);
  swap(funcToFormalOutMap, other.funcToFormalOutMap);
  swap(funcToFormalParmMap, other.funcToFormalParmMap);
  swap(funcToFormalRetMap, other.funcToFormalRetMap);
  swap(loadInstToMuMap, other.loadInstToMuMap);
  swap(storeInstToChiMap, other.storeInstToChiMap);
  swap(icfg, other.icfg);
  swap(nextNodeId, other.nextNodeId);
  swap(stat, other.stat);
  swap(objectDebug, other.objectDebug);
  swap(objIdToValue, other.objIdToValue);
  swap(valueToObjId, other.valueToObjId);
  swap(valueToObjIds, other.valueToObjIds);
  swap(gepBaseToFieldObject, other.gepBaseToFieldObject);
  swap(baseOffsetToFieldObject, other.baseOffsetToFieldObject);
  swap(gepAccessInfo, other.gepAccessInfo);
  swap(objIdToInfo, other.objIdToInfo);
  swap(nodeFunctionDebug, other.nodeFunctionDebug);
  swap(nodeCallSiteDebug, other.nodeCallSiteDebug);
  swap(nodesForUpdate, other.nodesForUpdate);
  swap(funPtrToIndCallSites, other.funPtrToIndCallSites);
  swap(callSiteToConnectedCallees, other.callSiteToConnectedCallees);
  swap(calleeToIndCallSites, other.calleeToIndCallSites);
  swap(callSiteCalleeToId, other.callSiteCalleeToId);
  swap(globalStoreNodes, other.globalStoreNodes);
  swap(nextCallSiteId, other.nextCallSiteId);
  swap(refinedCallGraph, other.refinedCallGraph);
}

void SVFG::markForUpdate(SVFGNode *node) {
  if (node) {
    nodesForUpdate.insert(node);
  }
}

SVFGNodeSet SVFG::getNodesForUpdate() const { return nodesForUpdate; }

void SVFG::clearUpdateMarkers() { nodesForUpdate.clear(); }
