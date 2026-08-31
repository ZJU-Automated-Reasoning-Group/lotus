#include "IR/SVFG/SVFGSerializer.h"

#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace lotus::analysis;
using namespace llvm;

static constexpr const char *kHeaderV2 = "SVFG-TEXT-V2";
static constexpr const char *kHeaderV3 = "SVFG-TEXT-V3";
static constexpr const char *kHeaderV4 = "SVFG-TEXT-V4";
static constexpr const char *kHeaderV5 = "SVFG-TEXT-V5";
static constexpr const char *kHeaderV6 = "SVFG-TEXT-V6";
static constexpr const char *kHeaderV7 = "SVFG-TEXT-V7";

namespace {

enum class AnchorKind : uint32_t {
  None = 0,
  Function = 1,
  GlobalValue = 2,
  Argument = 3,
  Instruction = 4,
};

struct Anchor {
  AnchorKind kind = AnchorKind::None;
  std::string symbolName;
  std::string functionName;
  uint32_t bbIndex = 0;
  uint32_t instIndex = 0;
  uint32_t argIndex = 0;
};

struct ParsedObjectInfo {
  uint32_t objId = 0;
  uint32_t flags = 0;
  uint32_t baseObjId = 0;
  Anchor valueAnchor;
};

struct ParsedIndCallSite {
  uint32_t funPtrNodeId = 0;
  Anchor callAnchor;
};

struct ParsedNode {
  uint32_t id = 0;
  SVFGK kind = SVFGK::Dummy;
  uint32_t memReg = 0;
  uint32_t version = 0;
  uint32_t aux0 = 0;
  uint32_t aux1 = 0;
  SVFGNodeBS pts;
};

struct ParsedEdge {
  uint32_t src = 0;
  uint32_t dst = 0;
  uint32_t kind = 0;
  uint32_t weight = static_cast<uint32_t>(SVFGEdge::EdgeWeight::One);
  SVFGNodeBS pts;
  Anchor callAnchor;
  std::string callSiteDebug;
};

struct ParsedNodeMeta {
  Anchor valueAnchor;
  Anchor instAnchor;
  Anchor functionAnchor;
  Anchor callAnchor;
  std::vector<std::pair<uint32_t, Anchor>> valueOperands;
  std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> mssaOperands;
};

static const Module *getModuleFromICFG(const ICFG *icfg) {
  if (!icfg)
    return nullptr;
  for (const auto &pair : *icfg) {
    const ICFGNode *node = pair.second;
    if (!node)
      continue;
    if (const Function *F = node->getFunction())
      return F->getParent();
  }
  return nullptr;
}

static void writeAnchor(std::ostream &out, const Anchor &anchor) {
  out << static_cast<uint32_t>(anchor.kind) << " "
      << std::quoted(anchor.symbolName) << " "
      << std::quoted(anchor.functionName) << " " << anchor.bbIndex << " "
      << anchor.instIndex << " " << anchor.argIndex;
}

static bool readAnchor(std::istream &in, Anchor &anchor) {
  uint32_t kind = 0;
  if (!(in >> kind >> std::quoted(anchor.symbolName) >>
        std::quoted(anchor.functionName) >> anchor.bbIndex >>
        anchor.instIndex >> anchor.argIndex)) {
    return false;
  }
  anchor.kind = static_cast<AnchorKind>(kind);
  return true;
}

static const BasicBlock *getBasicBlockByIndex(const Function *F,
                                              uint32_t index) {
  if (!F)
    return nullptr;
  uint32_t current = 0;
  for (const BasicBlock &BB : *F) {
    if (current == index)
      return &BB;
    ++current;
  }
  return nullptr;
}

static const Instruction *getInstructionByIndex(const BasicBlock *BB,
                                                uint32_t index) {
  if (!BB)
    return nullptr;
  uint32_t current = 0;
  for (const Instruction &I : *BB) {
    if (current == index)
      return &I;
    ++current;
  }
  return nullptr;
}

static Anchor getAnchorForValue(const Value *V) {
  Anchor anchor;
  if (!V)
    return anchor;

  if (const auto *F = dyn_cast<Function>(V)) {
    anchor.kind = AnchorKind::Function;
    anchor.symbolName = F->getName().str();
    return anchor;
  }

  if (const auto *GV = dyn_cast<GlobalValue>(V)) {
    anchor.kind = AnchorKind::GlobalValue;
    anchor.symbolName = GV->getName().str();
    return anchor;
  }

  if (const auto *Arg = dyn_cast<Argument>(V)) {
    anchor.kind = AnchorKind::Argument;
    if (const Function *F = Arg->getParent()) {
      anchor.functionName = F->getName().str();
      anchor.argIndex = Arg->getArgNo();
    }
    return anchor;
  }

  if (const auto *Inst = dyn_cast<Instruction>(V)) {
    anchor.kind = AnchorKind::Instruction;
    if (const Function *F = Inst->getFunction())
      anchor.functionName = F->getName().str();
    const BasicBlock *BB = Inst->getParent();
    if (!BB)
      return anchor;

    uint32_t bbIndex = 0;
    for (const BasicBlock &Candidate : *Inst->getFunction()) {
      if (&Candidate == BB)
        break;
      ++bbIndex;
    }
    anchor.bbIndex = bbIndex;

    uint32_t instIndex = 0;
    for (const Instruction &Candidate : *BB) {
      if (&Candidate == Inst)
        break;
      ++instIndex;
    }
    anchor.instIndex = instIndex;
    return anchor;
  }

  return anchor;
}

static const Value *resolveValueAnchor(const Module *M, const Anchor &anchor) {
  if (!M)
    return nullptr;
  switch (anchor.kind) {
  case AnchorKind::None:
    return nullptr;
  case AnchorKind::Function:
    return M->getFunction(anchor.symbolName);
  case AnchorKind::GlobalValue:
    return M->getNamedValue(anchor.symbolName);
  case AnchorKind::Argument: {
    const Function *F = M->getFunction(anchor.functionName);
    if (!F)
      return nullptr;
    if (anchor.argIndex >= F->arg_size())
      return nullptr;
    const auto *argIt = F->arg_begin();
    std::advance(argIt, anchor.argIndex);
    return &*argIt;
  }
  case AnchorKind::Instruction: {
    const Function *F = M->getFunction(anchor.functionName);
    const BasicBlock *BB = getBasicBlockByIndex(F, anchor.bbIndex);
    return getInstructionByIndex(BB, anchor.instIndex);
  }
  }
  return nullptr;
}

static const CallBase *resolveCallAnchor(const Module *M,
                                         const Anchor &anchor) {
  return dyn_cast_or_null<CallBase>(resolveValueAnchor(M, anchor));
}

static SVFGEdgeK canonicalizeEdgeKind(SVFGEdgeK kind,
                                      const CallBase *callSite) {
  switch (kind) {
  case SVFGEdgeK::ParamCall:
    return (callSite && callSite->getCalledFunction()) ? SVFGEdgeK::CallDir
                                                       : SVFGEdgeK::CallInd;
  case SVFGEdgeK::ParamRet:
    return (callSite && callSite->getCalledFunction()) ? SVFGEdgeK::RetDir
                                                       : SVFGEdgeK::RetInd;
  case SVFGEdgeK::CallFIn:
    return SVFGEdgeK::CallAIn;
  case SVFGEdgeK::RetFOut:
    return SVFGEdgeK::RetAOut;
  default:
    return kind;
  }
}

static const ICFGNode *resolveICFGNode(const SVFG &graph, const Anchor &anchor,
                                       const Function *fallbackFunction) {
  const ICFG *icfg = graph.getICFG();
  if (!icfg)
    return nullptr;

  if (anchor.kind == AnchorKind::Instruction) {
    const Value *V = resolveValueAnchor(getModuleFromICFG(icfg), anchor);
    const auto *I = dyn_cast_or_null<Instruction>(V);
    if (I)
      return const_cast<ICFG *>(icfg)->getIntraBlockNode(I->getParent());
  }

  if (fallbackFunction && !fallbackFunction->isDeclaration())
    return const_cast<ICFG *>(icfg)->getFunEntryICFGNode(fallbackFunction);

  return nullptr;
}

static uint32_t packObjectInfoFlags(const SVFG::ObjectInfo &info) {
  uint32_t flags = 0;
  flags |= info.isHeap ? (1u << 0) : 0;
  flags |= info.isConcreteHeap ? (1u << 1) : 0;
  flags |= info.isStack ? (1u << 2) : 0;
  flags |= info.isGlobal ? (1u << 3) : 0;
  flags |= info.isFunction ? (1u << 4) : 0;
  flags |= info.isConstant ? (1u << 5) : 0;
  flags |= info.isFieldInsensitive ? (1u << 6) : 0;
  flags |= info.isArray ? (1u << 7) : 0;
  flags |= info.isUnknown ? (1u << 8) : 0;
  flags |= info.isSingleton ? (1u << 9) : 0;
  return flags;
}

static SVFG::ObjectInfo unpackObjectInfoFlags(uint32_t flags,
                                              uint32_t baseObjId) {
  SVFG::ObjectInfo info;
  info.isHeap = (flags & (1u << 0)) != 0;
  info.isConcreteHeap = (flags & (1u << 1)) != 0;
  info.isStack = (flags & (1u << 2)) != 0;
  info.isGlobal = (flags & (1u << 3)) != 0;
  info.isFunction = (flags & (1u << 4)) != 0;
  info.isConstant = (flags & (1u << 5)) != 0;
  info.isFieldInsensitive = (flags & (1u << 6)) != 0;
  info.isArray = (flags & (1u << 7)) != 0;
  info.isUnknown = (flags & (1u << 8)) != 0;
  info.isSingleton = (flags & (1u << 9)) != 0;
  info.baseObjId = baseObjId;
  return info;
}

static Anchor getNodeValueAnchor(const SVFGNode *node) {
  if (!node)
    return {};
  if (const Value *V = node->getValue())
    return getAnchorForValue(V);
  if (const auto *formalParm = dyn_cast<FormalParmSVFGNode>(node)) {
    if (const Function *F = formalParm->getFunction()) {
      if (formalParm->getParamIndex() < F->arg_size()) {
        const auto *argIt = F->arg_begin();
        std::advance(argIt, formalParm->getParamIndex());
        return getAnchorForValue(&*argIt);
      }
    }
  }
  return {};
}

static Anchor getNodeInstructionAnchor(const SVFGNode *node) {
  if (!node)
    return {};
  if (const Instruction *I = node->getInstruction())
    return getAnchorForValue(I);
  if (const auto *loadMu = dyn_cast<LoadMuSVFGNode>(node))
    return getAnchorForValue(loadMu->getLoadInst());
  if (const auto *storeChi = dyn_cast<StoreChiSVFGNode>(node))
    return getAnchorForValue(storeChi->getStoreInst());
  return {};
}

static Anchor getNodeFunctionAnchor(const SVFGNode *node) {
  if (!node)
    return {};
  if (const Function *F = node->getFunction())
    return getAnchorForValue(F);
  return {};
}

static Anchor getNodeCallAnchor(const SVFGNode *node) {
  if (!node)
    return {};
  if (const CallBase *callSite = node->getCallSite())
    return getAnchorForValue(callSite);
  return {};
}

static void writeNodeMetaAnchor(std::ostream &out, char tag, uint32_t nodeId,
                                const Anchor &anchor) {
  if (anchor.kind == AnchorKind::None)
    return;
  out << tag << " " << nodeId << " ";
  writeAnchor(out, anchor);
  out << "\n";
}

static void registerNodeBindings(SVFG &graph, SVFGNode *node) {
  if (!node)
    return;

  if (const Value *V = node->getValue()) {
    switch (node->getNodeKind()) {
    case SVFGK::Addr:
    case SVFGK::Copy:
    case SVFGK::Load:
    case SVFGK::Gep:
    case SVFGK::BinaryOp:
    case SVFGK::UnaryOp:
    case SVFGK::Cmp:
    case SVFGK::Phi:
    case SVFGK::IntraPhi:
    case SVFGK::InterPhi:
    case SVFGK::FormalParm:
      graph.setValueNode(V, node->getId());
      break;
    default:
      break;
    }
  }

  if ((node->isStmtNode() || node->isPhiNode()) && node->getInstruction())
    graph.setDef(node->getInstruction(), node->getId());

  if (auto *formalParm = dyn_cast<FormalParmSVFGNode>(node)) {
    if (const Function *F = formalParm->getFunction()) {
      graph.addFormalParm(F, formalParm);
      if (formalParm->getParamIndex() < F->arg_size()) {
        const auto *argIt = F->arg_begin();
        std::advance(argIt, formalParm->getParamIndex());
        graph.setValueNode(&*argIt, node->getId());
      }
    }
  } else if (auto *formalRet = dyn_cast<FormalRetSVFGNode>(node)) {
    if (const Function *F = formalRet->getFunction())
      graph.addFormalRet(F, formalRet);
  } else if (auto *varArg = dyn_cast<VarArgSVFGNode>(node)) {
    if (const Function *F = varArg->getFunction())
      graph.addFormalParm(F, varArg);
  } else if (auto *formalIn = dyn_cast<FormalInSVFGNode>(node)) {
    if (const Function *F = formalIn->getFunction())
      graph.addFormalIn(F, formalIn);
  } else if (auto *formalOut = dyn_cast<FormalOutSVFGNode>(node)) {
    if (const Function *F = formalOut->getFunction())
      graph.addFormalOut(F, formalOut);
  } else if (auto *actualParm = dyn_cast<ActualParmSVFGNode>(node)) {
    if (const CallBase *cs = actualParm->getCallSite())
      graph.addActualParm(cs, actualParm);
  } else if (auto *actualRet = dyn_cast<ActualRetSVFGNode>(node)) {
    if (const CallBase *cs = actualRet->getCallSite())
      graph.addActualRet(cs, actualRet);
  } else if (auto *actualIn = dyn_cast<ActualInSVFGNode>(node)) {
    if (const CallBase *cs = actualIn->getCallSite())
      graph.addActualIn(cs, actualIn);
  } else if (auto *actualOut = dyn_cast<ActualOutSVFGNode>(node)) {
    if (const CallBase *cs = actualOut->getCallSite())
      graph.addActualOut(cs, actualOut);
  }
}

static void rebuildCallsiteConnectivity(SVFG &graph) {
  for (const auto &pair : graph) {
    const SVFGNode *src = pair.second;
    if (!src)
      continue;
    for (SVFGEdge *edge : src->getOutEdges()) {
      if (!edge || !edge->getCallSite())
        continue;
      const Function *callee = nullptr;
      if (isCallVFGEdge(edge->getEdgeKind()))
        callee =
            edge->getDstNode() ? edge->getDstNode()->getFunction() : nullptr;
      else if (isRetVFGEdge(edge->getEdgeKind()))
        callee =
            edge->getSrcNode() ? edge->getSrcNode()->getFunction() : nullptr;
      if (callee)
        graph.markConnectedCallee(edge->getCallSite(), callee);
    }
  }
}

static void populateDerivedValueOperands(SVFGNode *node) {
  if (!node)
    return;
  if (auto *binary = dyn_cast<BinaryOpSVFGNode>(node)) {
    if (const auto *I = dyn_cast_or_null<Instruction>(binary->getValue())) {
      for (unsigned i = 0, e = I->getNumOperands(); i < e; ++i)
        binary->setOpVer(i, I->getOperand(i));
    }
  } else if (auto *unary = dyn_cast<UnaryOpSVFGNode>(node)) {
    if (const auto *I = dyn_cast_or_null<Instruction>(unary->getValue())) {
      if (I->getNumOperands() > 0)
        unary->setOpVer(0, I->getOperand(0));
    }
  } else if (auto *cmp = dyn_cast<CmpSVFGNode>(node)) {
    if (const auto *I = dyn_cast_or_null<Instruction>(cmp->getValue())) {
      for (unsigned i = 0, e = I->getNumOperands(); i < e; ++i)
        cmp->setOpVer(i, I->getOperand(i));
    }
  } else if (auto *phi = dyn_cast<PhiSVFGNode>(node)) {
    if (const auto *phiInst = phi->getPHINode()) {
      for (unsigned i = 0, e = phiInst->getNumIncomingValues(); i < e; ++i)
        phi->setOpVer(i, phiInst->getIncomingValue(i));
    }
  }
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
static SVFGNode *createLegacyInterMSSAPhiNode(uint32_t id,
                                              const ICFGNode *icfgNode,
                                              const CallBase *resolvedCall,
                                              const Function *resolvedFunction,
                                              uint32_t memReg,
                                              const SVFGNodeBS &pts) {
  if (resolvedCall)
    return new InterMSSAPhiSVFGNode(id, icfgNode, resolvedCall, memReg, pts);
  return new InterMSSAPhiSVFGNode(id, icfgNode, resolvedFunction, memReg, pts);
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static SVFGNode *createNodeForKind(const ParsedNode &parsed,
                                   const ParsedNodeMeta &meta,
                                   const Module *module, const SVFG &graph) {
  const Value *resolvedValue = resolveValueAnchor(module, meta.valueAnchor);
  const Instruction *resolvedInst = dyn_cast_or_null<Instruction>(
      resolveValueAnchor(module, meta.instAnchor));
  const Function *resolvedFunction = dyn_cast_or_null<Function>(
      resolveValueAnchor(module, meta.functionAnchor));
  const CallBase *resolvedCall = resolveCallAnchor(module, meta.callAnchor);

  const ICFG *icfg = graph.getICFG();
  ICFG *mutableICFG = const_cast<ICFG *>(icfg);
  const ICFGNode *icfgNode = resolveICFGNode(graph, meta.instAnchor, nullptr);
  if (!icfgNode && resolvedValue && icfg) {
    if (const auto *I = dyn_cast<Instruction>(resolvedValue))
      icfgNode = mutableICFG->getIntraBlockNode(I->getParent());
  }
  if (icfg) {
    switch (parsed.kind) {
    case SVFGK::FormalParm:
    case SVFGK::VarArg:
    case SVFGK::FormalIn:
    case SVFGK::EntryChi:
      if (parsed.kind == SVFGK::EntryChi && !resolvedFunction)
        icfgNode = mutableICFG->getGlobalInitICFGNode();
      else if (resolvedFunction && !resolvedFunction->isDeclaration())
        icfgNode = mutableICFG->getFunEntryICFGNode(resolvedFunction);
      break;
    case SVFGK::FormalRet:
    case SVFGK::FormalOut:
    case SVFGK::RetMu:
      if (resolvedFunction && !resolvedFunction->isDeclaration())
        icfgNode = mutableICFG->getFunExitICFGNode(resolvedFunction);
      break;
    case SVFGK::ActualRet:
    case SVFGK::ActualOut:
      if (resolvedCall)
        icfgNode = mutableICFG->getRetICFGNode(resolvedCall);
      break;
    case SVFGK::ActualParm:
    case SVFGK::ActualIn:
    case SVFGK::CallMu:
    case SVFGK::CallChi:
      if (resolvedCall)
        icfgNode = mutableICFG->getIntraBlockNode(resolvedCall->getParent());
      break;
    default:
      if (!icfgNode && resolvedCall)
        icfgNode = mutableICFG->getIntraBlockNode(resolvedCall->getParent());
      else if (!icfgNode && resolvedFunction &&
               !resolvedFunction->isDeclaration())
        icfgNode = mutableICFG->getFunEntryICFGNode(resolvedFunction);
      break;
    }
  }

  SVFGNode *node = nullptr;
  switch (parsed.kind) {
  case SVFGK::Stmt:
    node = new StmtSVFGNode(parsed.id, SVFGK::Stmt, icfgNode, resolvedValue);
    break;
  case SVFGK::Addr:
    node = new AddrSVFGNode(parsed.id, icfgNode, resolvedValue, parsed.aux0);
    break;
  case SVFGK::Copy:
    node = new CopySVFGNode(parsed.id, icfgNode, resolvedValue);
    break;
  case SVFGK::Load:
    node = new LoadSVFGNode(parsed.id, icfgNode, resolvedValue, parsed.aux0);
    break;
  case SVFGK::Store:
    node = new StoreSVFGNode(parsed.id, icfgNode, resolvedValue, parsed.aux0);
    break;
  case SVFGK::Gep:
    node = new GepSVFGNode(parsed.id, icfgNode, resolvedValue);
    break;
  case SVFGK::BinaryOp:
    node = new BinaryOpSVFGNode(parsed.id, icfgNode, resolvedValue);
    break;
  case SVFGK::UnaryOp:
    node = new UnaryOpSVFGNode(parsed.id, icfgNode, resolvedValue);
    break;
  case SVFGK::Cmp:
    node = new CmpSVFGNode(parsed.id, icfgNode, resolvedValue);
    break;
  case SVFGK::Branch:
    node = new BranchSVFGNode(parsed.id, icfgNode, resolvedValue);
    break;
  case SVFGK::Phi:
    node = new PhiSVFGNode(parsed.id, SVFGK::Phi, icfgNode,
                           resolvedValue ? resolvedValue : resolvedInst);
    break;
  case SVFGK::IntraPhi:
    node = new IntraPhiSVFGNode(parsed.id, icfgNode,
                                resolvedValue ? resolvedValue : resolvedInst);
    break;
  case SVFGK::InterPhi:
    if (resolvedCall)
      node =
          new InterPhiSVFGNode(parsed.id, icfgNode, resolvedCall, resolvedValue);
    else
      node = new InterPhiSVFGNode(parsed.id, icfgNode, resolvedFunction,
                                  resolvedValue);
    break;
  case SVFGK::MPhi:
    node = new MSSAPhiSVFGNode(parsed.id, SVFGK::MPhi, icfgNode, parsed.memReg,
                               parsed.pts);
    break;
  case SVFGK::MIntraPhi:
    node = new IntraMSSAPhiSVFGNode(parsed.id, icfgNode, parsed.memReg,
                                    parsed.version, parsed.pts);
    break;
  case SVFGK::MInterPhi:
    node = createLegacyInterMSSAPhiNode(parsed.id, icfgNode, resolvedCall,
                                        resolvedFunction, parsed.memReg,
                                        parsed.pts);
    break;
  case SVFGK::FormalIn:
    node = new FormalInSVFGNode(parsed.id, icfgNode, resolvedFunction,
                                parsed.memReg, parsed.pts, parsed.version);
    break;
  case SVFGK::FormalOut:
    node = new FormalOutSVFGNode(parsed.id, icfgNode, resolvedFunction,
                                 parsed.memReg, parsed.pts, parsed.version);
    break;
  case SVFGK::ActualIn:
    node = new ActualInSVFGNode(parsed.id, icfgNode, resolvedCall,
                                parsed.memReg, parsed.pts, parsed.version);
    break;
  case SVFGK::ActualOut:
    node = new ActualOutSVFGNode(parsed.id, icfgNode, resolvedCall,
                                 parsed.memReg, parsed.pts, parsed.version);
    break;
  case SVFGK::LoadMu:
    node = new LoadMuSVFGNode(parsed.id, icfgNode,
                              dyn_cast_or_null<LoadInst>(resolvedInst),
                              parsed.memReg, parsed.pts, parsed.version);
    break;
  case SVFGK::StoreChi:
    node = new StoreChiSVFGNode(parsed.id, icfgNode,
                                dyn_cast_or_null<StoreInst>(resolvedInst),
                                parsed.memReg, parsed.pts, parsed.version);
    break;
  case SVFGK::CallMu:
    node = new ActualInSVFGNode(parsed.id, icfgNode, resolvedCall,
                                parsed.memReg, parsed.pts, parsed.version);
    break;
  case SVFGK::CallChi:
    node = new ActualOutSVFGNode(parsed.id, icfgNode, resolvedCall,
                                 parsed.memReg, parsed.pts, parsed.version);
    break;
  case SVFGK::RetMu:
    node = new FormalOutSVFGNode(parsed.id, icfgNode, resolvedFunction,
                                 parsed.memReg, parsed.pts, parsed.version);
    break;
  case SVFGK::EntryChi:
    node = new EntryChiSVFGNode(parsed.id, icfgNode, resolvedFunction,
                                parsed.memReg, parsed.pts, parsed.version);
    break;
  case SVFGK::FormalParm:
    node = new FormalParmSVFGNode(parsed.id, icfgNode, resolvedFunction,
                                  parsed.aux0,
                                  dyn_cast_or_null<Argument>(resolvedValue));
    break;
  case SVFGK::ActualParm:
    node = new ActualParmSVFGNode(parsed.id, icfgNode, resolvedCall,
                                  parsed.aux0, resolvedValue);
    break;
  case SVFGK::FormalRet:
    node = new FormalRetSVFGNode(parsed.id, icfgNode, resolvedFunction,
                                 resolvedValue);
    break;
  case SVFGK::ActualRet:
    node = new ActualRetSVFGNode(parsed.id, icfgNode, resolvedCall);
    break;
  case SVFGK::VarArg:
    node = new VarArgSVFGNode(parsed.id, icfgNode, resolvedFunction);
    break;
  case SVFGK::NullPtr:
    node = new NullPtrSVFGNode(parsed.id, icfgNode);
    break;
  case SVFGK::Dummy:
    node = new DummySVFGNode(parsed.id, icfgNode);
    break;
  case SVFGK::DummyVProp:
    node = new DummyVersionPropSVFGNode(parsed.id, icfgNode, parsed.memReg,
                                        parsed.version);
    break;
  case SVFGK::Variant:
  case SVFGK::Total:
    break;
  }

  if (!node)
    node = new DummySVFGNode(parsed.id, icfgNode);
  node->setValueId(parsed.aux1);
  if (auto *loadNode = dyn_cast<LoadSVFGNode>(node))
    loadNode->setMemoryUse(parsed.memReg, parsed.version, parsed.pts);
  else if (auto *storeNode = dyn_cast<StoreSVFGNode>(node))
    storeNode->setMemoryDef(parsed.memReg, parsed.version, parsed.pts);
  return node;
}

static void applyValueOperands(SVFGNode *node, const ParsedNodeMeta &meta,
                               const Module *module) {
  if (!node)
    return;

  if (meta.valueOperands.empty()) {
    populateDerivedValueOperands(node);
    return;
  }

  if (auto *binary = dyn_cast<BinaryOpSVFGNode>(node)) {
    for (const auto &entry : meta.valueOperands)
      binary->setOpVer(entry.first, resolveValueAnchor(module, entry.second));
    return;
  }
  if (auto *unary = dyn_cast<UnaryOpSVFGNode>(node)) {
    for (const auto &entry : meta.valueOperands)
      unary->setOpVer(entry.first, resolveValueAnchor(module, entry.second));
    return;
  }
  if (auto *cmp = dyn_cast<CmpSVFGNode>(node)) {
    for (const auto &entry : meta.valueOperands)
      cmp->setOpVer(entry.first, resolveValueAnchor(module, entry.second));
    return;
  }
  if (auto *phi = dyn_cast<PhiSVFGNode>(node)) {
    for (const auto &entry : meta.valueOperands)
      phi->setOpVer(entry.first, resolveValueAnchor(module, entry.second));
  }
}

static void applyMSSAOperands(SVFGNode *node, const ParsedNodeMeta &meta) {
  auto *phi = dyn_cast<MSSAPhiSVFGNode>(node);
  if (!phi)
    return;
  for (const auto &entry : meta.mssaOperands)
    phi->setOpVer(std::get<0>(entry), std::get<1>(entry), std::get<2>(entry));
}

} // namespace

bool SVFGSerializer::writeDot(const SVFG &graph, const std::string &filename,
                              bool simple) {
  std::ofstream file(filename);
  if (!file.is_open())
    return false;

  file << "digraph SVFG {\n";
  file << "  rankdir=TB;\n";
  file << "  node [shape=box, fontsize=10];\n";

  for (const auto &pair : graph) {
    const SVFGNode *node = pair.second;
    file << "  N" << node->getId() << " [label=\"";
    if (simple) {
      file << node->getId() << ":" << static_cast<uint32_t>(node->getNodeKind());
    } else {
      file << "ID: " << node->getId() << "\\n";
      file << "Kind: " << static_cast<uint32_t>(node->getNodeKind());
    }
    if (!simple) {
      if (const auto *val = node->getValue()) {
        file << "\\nVal: " << val->getName().str();
      }
      if (const auto fnDebug = graph.getNodeFunctionDebug(node->getId());
          !fnDebug.empty()) {
        file << "\\nFn: " << fnDebug;
      }
      if (const auto callDebug = graph.getNodeCallSiteDebug(node->getId());
          !callDebug.empty()) {
        file << "\\nCS: " << callDebug;
      }
    } else if (const auto *val = node->getValue()) {
      file << "\\nVal: " << val->getName().str();
    }
    file << "\"];\n";
  }

  for (const auto &pair : graph) {
    const SVFGNode *node = pair.second;
    for (const SVFGEdge *edge : node->getOutEdges()) {
      file << "  N" << edge->getSrcNode()->getId() << " -> N"
           << edge->getDstNode()->getId();
      file << " [label=\"";
      if (simple) {
        file << static_cast<uint32_t>(edge->getEdgeKind());
      } else {
        file << edge->toString();
        if (edge->hasCallSiteDebug()) {
          file << "\\n" << edge->getCallSiteDebug();
        }
      }
      file << "\"";
      if (!simple) {
        if (edge->hasCallSite() || edge->hasCallSiteDebug()) {
          file << ", color=darkgreen";
        }
        if (!edge->getPointsTo().empty()) {
          file << ", tooltip=\"pts=" << edge->getPointsTo().size() << "\"";
        }
        if (edge->getWeight() == SVFGEdge::EdgeWeight::Many) {
          file << ", style=dashed";
        }
      }
      file << "];\n";
    }
  }

  file << "}\n";
  return true;
}

bool SVFGSerializer::writeText(const SVFG &graph, const std::string &filename) {
  std::ofstream file(filename);
  if (!file.is_open())
    return false;

  file << kHeaderV7 << "\n";

  // Persist object debug labels to preserve points-to identity across reloads.
  for (const auto &pair : graph.getObjectDebugMap()) {
    file << "O " << pair.first << " " << pair.second << "\n";
  }

  for (const auto &pair : graph.getObjectDebugMap()) {
    const uint32_t objId = pair.first;
    const auto *info = graph.getObjectInfo(objId);
    const uint32_t flags = info ? packObjectInfoFlags(*info) : 0;
    const uint32_t baseObjId = info ? info->baseObjId : 0;
    file << "T " << objId << " " << flags << " " << baseObjId << " ";
    writeAnchor(file, getAnchorForValue(graph.getObjectValue(objId)));
    file << "\n";
  }

  for (const auto &pair : graph.getIndCallSiteMap()) {
    for (const CallBase *callSite : pair.second) {
      file << "I " << pair.first << " ";
      writeAnchor(file, getAnchorForValue(callSite));
      file << "\n";
    }
  }

  for (const auto &pair : graph) {
    const SVFGNode *node = pair.second;
    std::string fnDebug = graph.getNodeFunctionDebug(node->getId());
    std::string csDebug = graph.getNodeCallSiteDebug(node->getId());
    if (fnDebug.empty()) {
      if (const llvm::Function *F = node->getFunction()) {
        fnDebug = F->getName().str();
      }
    }
    if (csDebug.empty()) {
      if (const CallBase *callSite = node->getCallSite()) {
        std::string label;
        llvm::raw_string_ostream os(label);
        os << callSite->getFunction()->getName() << "->";
        if (const llvm::Function *callee = callSite->getCalledFunction()) {
          os << callee->getName();
        } else {
          os << "ind";
        }
        csDebug = os.str();
      }
    }
    if (!fnDebug.empty() || !csDebug.empty()) {
      file << "M " << node->getId() << " " << std::quoted(fnDebug) << " "
           << std::quoted(csDebug) << "\n";
    }

    writeNodeMetaAnchor(file, 'A', node->getId(), getNodeValueAnchor(node));
    writeNodeMetaAnchor(file, 'B', node->getId(),
                        getNodeInstructionAnchor(node));
    writeNodeMetaAnchor(file, 'C', node->getId(), getNodeFunctionAnchor(node));
    writeNodeMetaAnchor(file, 'D', node->getId(), getNodeCallAnchor(node));

    if (const auto *binary = dyn_cast<BinaryOpSVFGNode>(node)) {
      for (auto it = binary->opVerBegin(), eit = binary->opVerEnd(); it != eit;
           ++it) {
        file << "P " << node->getId() << " " << it->first << " ";
        writeAnchor(file, getAnchorForValue(it->second));
        file << "\n";
      }
    } else if (const auto *unary = dyn_cast<UnaryOpSVFGNode>(node)) {
      for (auto it = unary->opVerBegin(), eit = unary->opVerEnd(); it != eit;
           ++it) {
        file << "P " << node->getId() << " " << it->first << " ";
        writeAnchor(file, getAnchorForValue(it->second));
        file << "\n";
      }
    } else if (const auto *cmp = dyn_cast<CmpSVFGNode>(node)) {
      for (auto it = cmp->opVerBegin(), eit = cmp->opVerEnd(); it != eit;
           ++it) {
        file << "P " << node->getId() << " " << it->first << " ";
        writeAnchor(file, getAnchorForValue(it->second));
        file << "\n";
      }
    } else if (const auto *phi = dyn_cast<PhiSVFGNode>(node)) {
      for (auto it = phi->opVerBegin(), eit = phi->opVerEnd(); it != eit;
           ++it) {
        file << "P " << node->getId() << " " << it->first << " ";
        writeAnchor(file, getAnchorForValue(it->second));
        file << "\n";
      }
    }

    if (const auto *mssaPhi = dyn_cast<MSSAPhiSVFGNode>(node)) {
      for (auto it = mssaPhi->opVerBegin(), eit = mssaPhi->opVerEnd();
           it != eit; ++it) {
        file << "Q " << node->getId() << " " << it->first << " "
             << it->second.memReg << " " << it->second.version << "\n";
      }
    }
  }

  for (const auto &pair : graph) {
    const SVFGNode *node = pair.second;
    uint32_t aux0 = 0;
    uint32_t aux1 = 0;
    switch (node->getNodeKind()) {
    case SVFGK::Addr:
      aux0 = dynamic_cast<const AddrSVFGNode *>(node)->getObjectId();
      break;
    case SVFGK::Load:
      aux0 = dynamic_cast<const LoadSVFGNode *>(node)->getLoadFromPtr();
      break;
    case SVFGK::Store:
      aux0 = dynamic_cast<const StoreSVFGNode *>(node)->getStoreToPtr();
      break;
    case SVFGK::FormalParm:
      aux0 = dynamic_cast<const FormalParmSVFGNode *>(node)->getParamIndex();
      break;
    case SVFGK::ActualParm:
      aux0 = dynamic_cast<const ActualParmSVFGNode *>(node)->getParamIndex();
      break;
    default:
      break;
    }
    aux1 = node->getValueId();

    SVFGNodeBS pts;
    if (const auto *p = node->getPointsTo()) {
      pts = *p;
    }

    file << "N " << node->getId() << " "
         << static_cast<uint32_t>(node->getNodeKind()) << " "
         << node->getMemReg() << " " << node->getSSAVersion() << " " << aux0
         << " " << aux1 << " " << pts.size();
    for (uint32_t pt : pts) {
      file << " " << pt;
    }
    file << "\n";
  }

  for (const auto &pair : graph) {
    const SVFGNode *node = pair.second;
    for (const SVFGEdge *edge : node->getOutEdges()) {
      file << "E " << edge->getSrcNode()->getId() << " "
           << edge->getDstNode()->getId() << " "
           << static_cast<uint32_t>(edge->getEdgeKind()) << " "
           << static_cast<uint32_t>(edge->getWeight()) << " "
           << edge->getPointsTo().size();
      for (uint32_t pt : edge->getPointsTo()) {
        file << " " << pt;
      }
      file << " ";
      writeAnchor(file, getAnchorForValue(edge->getCallSite()));
      file << " " << std::quoted(edge->getCallSiteDebug());
      file << "\n";
    }
  }
  return true;
}

bool SVFGSerializer::readText(SVFG &graph, const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open())
    return false;

  std::unordered_map<uint32_t, ParsedNode> nodes;
  std::unordered_map<uint32_t, ParsedNodeMeta> nodeMeta;
  std::vector<ParsedEdge> edges;
  std::vector<ParsedObjectInfo> objects;
  std::vector<ParsedIndCallSite> indCallSites;
  std::string line;
  bool sawV5 = false;
  while (std::getline(file, line)) {
    if (line.empty())
      continue;
    if (line == kHeaderV2) {
      continue;
    }
    if (line == kHeaderV3) {
      continue;
    }
    if (line == kHeaderV4) {
      continue;
    }
    if (line == kHeaderV5) {
      sawV5 = true;
      continue;
    }
    if (line == kHeaderV6) {
      sawV5 = true;
      continue;
    }
    if (line == kHeaderV7) {
      sawV5 = true;
      continue;
    }
    std::istringstream iss(line);
    char tag = 0;
    iss >> tag;
    if (tag == 'O') {
      uint32_t objId = 0;
      iss >> objId;
      std::string label;
      std::getline(iss, label);
      // Trim leading space.
      if (!label.empty() && label.front() == ' ')
        label.erase(label.begin());
      graph.setObjectDebug(objId, std::move(label));
      continue;
    }
    if (tag == 'T') {
      ParsedObjectInfo parsed;
      iss >> parsed.objId >> parsed.flags >> parsed.baseObjId;
      if (readAnchor(iss, parsed.valueAnchor))
        objects.push_back(std::move(parsed));
      continue;
    }
    if (tag == 'I') {
      ParsedIndCallSite parsed;
      iss >> parsed.funPtrNodeId;
      if (readAnchor(iss, parsed.callAnchor))
        indCallSites.push_back(std::move(parsed));
      continue;
    }
    if (tag == 'M') {
      uint32_t nodeId = 0;
      std::string fnDebug;
      std::string csDebug;
      iss >> nodeId >> std::quoted(fnDebug) >> std::quoted(csDebug);
      if (!fnDebug.empty()) {
        graph.setNodeFunctionDebug(nodeId, fnDebug);
      }
      if (!csDebug.empty()) {
        graph.setNodeCallSiteDebug(nodeId, csDebug);
      }
      continue;
    }
    if (tag == 'A' || tag == 'B' || tag == 'C' || tag == 'D') {
      uint32_t nodeId = 0;
      iss >> nodeId;
      Anchor anchor;
      if (!readAnchor(iss, anchor))
        continue;
      ParsedNodeMeta &meta = nodeMeta[nodeId];
      switch (tag) {
      case 'A':
        meta.valueAnchor = std::move(anchor);
        break;
      case 'B':
        meta.instAnchor = std::move(anchor);
        break;
      case 'C':
        meta.functionAnchor = std::move(anchor);
        break;
      case 'D':
        meta.callAnchor = std::move(anchor);
        break;
      default:
        break;
      }
      continue;
    }
    if (tag == 'P') {
      uint32_t nodeId = 0;
      uint32_t pos = 0;
      iss >> nodeId >> pos;
      Anchor anchor;
      if (readAnchor(iss, anchor))
        nodeMeta[nodeId].valueOperands.emplace_back(pos, std::move(anchor));
      continue;
    }
    if (tag == 'Q') {
      uint32_t nodeId = 0;
      uint32_t pos = 0;
      uint32_t memReg = 0;
      uint32_t version = 0;
      if (iss >> nodeId >> pos >> memReg >> version)
        nodeMeta[nodeId].mssaOperands.emplace_back(pos, memReg, version);
      continue;
    }
    if (tag == 'N') {
      ParsedNode parsed;
      uint32_t kindVal = 0;
      uint32_t ptsCount = 0;
      iss >> parsed.id >> kindVal >> parsed.memReg >> parsed.version;
      parsed.kind = static_cast<SVFGK>(kindVal);

      // V2 adds aux0 aux1 ptsCount pts...
      if (iss >> parsed.aux0 >> parsed.aux1 >> ptsCount) {
        // ok
      } else {
        // V1 format: "N id kind memReg version"
        parsed.aux0 = 0;
        parsed.aux1 = 0;
        ptsCount = 0;
        iss.clear();
      }
      for (uint32_t i = 0; i < ptsCount; ++i) {
        uint32_t pt = 0;
        if (!(iss >> pt))
          break;
        parsed.pts.insert(pt);
      }
      nodes.emplace(parsed.id, std::move(parsed));
    } else if (tag == 'E') {
      ParsedEdge edge;
      iss >> edge.src >> edge.dst >> edge.kind;

      // Optional fields for newer format.
      if (iss >> edge.weight) {
        uint32_t ptsCount = 0;
        if (iss >> ptsCount) {
          for (uint32_t i = 0; i < ptsCount; ++i) {
            uint32_t pt = 0;
            if (!(iss >> pt))
              break;
            edge.pts.insert(pt);
          }
          if (sawV5) {
            (void)readAnchor(iss, edge.callAnchor);
            iss >> std::quoted(edge.callSiteDebug);
          } else {
            iss.clear();
            std::string fallback;
            if (iss >> fallback)
              edge.callSiteDebug = std::move(fallback);
          }
        }
      }

      edges.push_back(std::move(edge));
    }
  }

  const Module *module = getModuleFromICFG(graph.getICFG());

  std::map<uint32_t, ParsedNode> orderedNodes(nodes.begin(), nodes.end());
  for (const auto &entry : orderedNodes) {
    const ParsedNode &parsed = entry.second;
    ParsedNodeMeta meta;
    auto metaIt = nodeMeta.find(parsed.id);
    if (metaIt != nodeMeta.end())
      meta = metaIt->second;

    SVFGNode *node = createNodeForKind(parsed, meta, module, graph);
    graph.addNode(node);
    applyValueOperands(node, meta, module);
    applyMSSAOperands(node, meta);
    registerNodeBindings(graph, node);
  }

  for (const ParsedObjectInfo &object : objects) {
    graph.setObjectInfo(object.objId,
                        unpackObjectInfoFlags(object.flags, object.baseObjId));
    if (const Value *V = resolveValueAnchor(module, object.valueAnchor))
      graph.setObjectValue(object.objId, V);
  }

  for (const ParsedIndCallSite &entry : indCallSites) {
    if (const CallBase *cs = resolveCallAnchor(module, entry.callAnchor))
      graph.addIndCallSite(entry.funPtrNodeId, cs);
  }

  for (const auto &edgeInfo : edges) {
    SVFGNode *src = graph.getNode(edgeInfo.src);
    SVFGNode *dst = graph.getNode(edgeInfo.dst);
    if (src && dst) {
      const CallBase *callSite = resolveCallAnchor(module, edgeInfo.callAnchor);
      SVFGEdgeK kind =
          canonicalizeEdgeKind(static_cast<SVFGEdgeK>(edgeInfo.kind), callSite);
      SVFGEdge *edge = graph.addEdge(src, dst, kind, callSite, edgeInfo.pts,
                                     edgeInfo.callSiteDebug);
      if (edge)
        edge->setWeight(static_cast<SVFGEdge::EdgeWeight>(edgeInfo.weight));
    }
  }

  rebuildCallsiteConnectivity(graph);
  return true;
}
