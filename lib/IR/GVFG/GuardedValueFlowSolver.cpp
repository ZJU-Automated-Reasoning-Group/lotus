#include "IR/GVFG/GuardedValueFlowSolver.h"

#include <climits>

#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>

using namespace llvm;
using namespace lotus::gvfg;

#define DEBUG_TYPE "gvfg-solver"

static cl::opt<unsigned> GVFGDDMaxDepth(
    "gvfg-solver-dd-max-depth", cl::init(UINT_MAX),
    cl::desc(
        "The max searching depth for guarded value-flow data dependencies."));

static cl::opt<unsigned>
    GVFGCDMaxDepth("gvfg-solver-cd-max-depth", cl::init(UINT_MAX),
                   cl::desc("The max searching depth for guarded value-flow "
                            "control dependencies."));

static cl::opt<bool> GVFGUseRegionAsCtrlDep(
    "gvfg-solver-use-region", cl::init(false),
    cl::desc("Use region nodes directly when computing control dependencies."));

static cl::opt<bool>
    GVFGIgnoreBitVecOverflow("gvfg-solver-ignore-bv-overflow", cl::init(true),
                             cl::desc("Ignore bitvector overflow heuristically "
                                      "for mul/div/shift operations."));

namespace {

static bool isTerminalNode(const GuardedValueFlowNode *node) {
  return node && node->children().empty();
}

static bool isResultLikeNode(const GuardedValueFlowNode *node) {
  if (!node)
    return false;
  return isa<GuardedValueFlowArgumentNode>(node) ||
         isa<GuardedValueFlowPhiNode>(node) ||
         isa<GuardedValueFlowReturnNode>(node) ||
         isa<GuardedValueFlowCallOutputNode>(node) ||
         isa<GuardedValueFlowCallSummaryNode>(node) ||
         node->getKind() == GuardedValueFlowNode::Kind::SimpleOperand ||
         node->getKind() == GuardedValueFlowNode::Kind::Unknown ||
         node->getKind() == GuardedValueFlowNode::Kind::UndefValue ||
         node->getKind() == GuardedValueFlowNode::Kind::LoadMemory ||
         node->getKind() == GuardedValueFlowNode::Kind::StoreMemory ||
         node->getKind() == GuardedValueFlowNode::Kind::Region ||
         node->getKind() == GuardedValueFlowNode::Kind::InterfaceCondition;
}

static unsigned getVectorElementCount(Type *type) {
  if (auto *vector_type = dyn_cast_or_null<VectorType>(type))
    return vector_type->getElementCount().getKnownMinValue();
  return 1;
}

static std::string buildNodeSymbol(const GuardedValueFlowNode *node) {
  std::string description = node->getDescription();
  for (char &ch : description) {
    if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
      ch = '_';
  }
  return (Twine("gvfg_") + Twine(node->getNodeId()) + "_" + description).str();
}

static bool shouldExcludeSummaryBackedChild(const GuardedValueFlowNode *current,
                                            const GuardedValueFlowNode *child) {
  if (!current || !child ||
      current->getKind() != GuardedValueFlowNode::Kind::LoadMemory) {
    return false;
  }

  const GuardedValueFlowNode *cursor = child;
  while (cursor && cursor->children().size() == 1)
    cursor = cursor->children().front().target;

  return cursor &&
         cursor->getKind() == GuardedValueFlowNode::Kind::CallSiteReturnSummary;
}

static bool isBinaryOpcodeKind(GuardedValueFlowOpcodeNode::OpcodeKind kind) {
  switch (kind) {
  case GuardedValueFlowOpcodeNode::OpcodeKind::URem:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FRem:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SRem:
  case GuardedValueFlowOpcodeNode::OpcodeKind::UDiv:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SDiv:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FDiv:
  case GuardedValueFlowOpcodeNode::OpcodeKind::And:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Or:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Xor:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Shl:
  case GuardedValueFlowOpcodeNode::OpcodeKind::LShr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::AShr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Mul:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FMul:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FAdd:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FSub:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Add:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Sub:
    return true;
  default:
    return false;
  }
}

static bool isNonNullTerminalValue(const GuardedValueFlowNode *node) {
  if (!node)
    return false;
  Value *value = node->getLLVMValue();
  if (!value)
    return false;
  return isa<AllocaInst>(value) ||
         isa<GlobalValue>(value->stripInBoundsConstantOffsets());
}

static SMTExpr getMatchingRegionExpr(GuardedValueFlowSolver &solver,
                                     const GuardedValueFlowNode *load_node,
                                     const GuardedValueFlowNode *producer) {
  auto *region = load_node ? load_node->getMatchingRegion(producer) : nullptr;
  if (!region)
    return solver.getSMTFactory().createBoolVal(true);
  return solver.getOrInsertExpr(region).bv12bool();
}

} // namespace

GuardedValueFlowSolver::GuardedValueFlowSolver(SMTFactory &factory,
                                               const DataLayout &dl)
    : SMTSolver(factory.createSMTSolver()), DL(dl) {}

GuardedValueFlowSolver::~GuardedValueFlowSolver() {}

SMTExprVec GuardedValueFlowSolver::getCtrlDeps(const GuardedValueFlowNode *node,
                                               const QueryContext *context) {
  assert(node && "Expected a modeled node.");
  BasicBlock *block = node->getParentBasicBlock();
  assert(block && "Expected node to belong to a block.");
  auto *graph = node->getGraph();
  assert(graph && "Expected node to belong to a graph.");
  return getCtrlDeps(block, graph, context);
}

SMTExprVec
GuardedValueFlowSolver::getCtrlDeps(BasicBlock *block,
                                    const GuardedValueFlowGraph *graph,
                                    const QueryContext *context) {
  auto pair = getCtrlDepsPair(block, graph, context);
  return SMTExprVec::merge(pair.first.copy(), pair.second);
}

std::pair<SMTExprVec, SMTExprVec>
GuardedValueFlowSolver::getCtrlDepsPair(BasicBlock *block,
                                        const GuardedValueFlowGraph *graph,
                                        const QueryContext *context) {
  return computeCtrlDepsPair(block, graph, context);
}

SMTExprVec
GuardedValueFlowSolver::getPhiGated(const GuardedValueFlowPhiNode *phi_node,
                                    GuardedValueFlowPhiNode::Incoming incoming,
                                    const QueryContext *context) {
  auto pair = computePhiGatedPair(phi_node, incoming, context);
  return SMTExprVec::merge(pair.first.copy(), pair.second);
}

SMTExprVec GuardedValueFlowSolver::getPhiGated(
    const GuardedValueFlowPhiNode *phi_node, const GuardedValueFlowNode *value,
    BasicBlock *block, const QueryContext *context) {
  for (const auto &incoming : phi_node->incoming()) {
    if (incoming.incoming_block == block && incoming.value_node == value)
      return getPhiGated(phi_node, incoming, context);
  }
  llvm_unreachable("Requested PHI gating for a non-incoming value/block pair");
}

SMTExprVec
GuardedValueFlowSolver::getPhiGated(const GuardedValueFlowPhiNode *phi_node,
                                    const GuardedValueFlowNode *value,
                                    const QueryContext *context) {
  SMTExprVec gated = Factory->createEmptySMTExprVec();
  SMTExprVec ors = Factory->createEmptySMTExprVec();
  for (const auto &incoming : phi_node->incoming()) {
    if (incoming.value_node == value)
      ors.push_back(getPhiGated(phi_node, incoming, context).toAndExpr());
  }
  gated.push_back(ors.toOrExpr());
  return gated;
}

SMTExprVec GuardedValueFlowSolver::getDataDeps(const GuardedValueFlowNode *node,
                                               const QueryContext *context) {
  return computeDataDeps(node, context);
}

SMTExpr
GuardedValueFlowSolver::getOrInsertExpr(const GuardedValueFlowNode *node) {
  assert(node);

  if (isa<GuardedValueFlowArgumentNode>(node) &&
      !FunctionArgumentCache.contains(node)) {
    FunctionArgumentCache.add(node);
  } else if (auto *call_output =
                 dyn_cast<GuardedValueFlowCallOutputNode>(node)) {
    if (!CallSiteOutputCache.contains(call_output))
      CallSiteOutputCache.add(call_output);
  }

  auto existing = NodeExprMap.find(node);
  if (existing != NodeExprMap.end())
    return existing->second;

  const std::string symbol = buildNodeSymbol(node);
  NodeSymbolNameMap[symbol] = node;

  SMTExpr result = Factory->createEmptySMTExpr();
  if (isTerminalNode(node) && !isa<GuardedValueFlowOpcodeNode>(node)) {
    if (Value *value = node->getLLVMValue()) {
      if (auto *constant_int = dyn_cast<ConstantInt>(value)) {
        result = Factory->createBitVecVal(
            llvm::toString(constant_int->getValue(), 10, false),
            DL.getTypeSizeInBits(node->getType()));
        NodeExprMap.insert({node, result});
        return result;
      }
      if (isa<ConstantPointerNull>(value) ||
          isa<ConstantAggregateZero>(value)) {
        result =
            Factory->createBitVecVal(0, DL.getTypeSizeInBits(node->getType()));
        NodeExprMap.insert({node, result});
        return result;
      }
      if (auto *constant_fp = dyn_cast<ConstantFP>(value)) {
        APInt bits = constant_fp->getValueAPF().bitcastToAPInt();
        result =
            Factory->createBitVecVal(llvm::toString(bits, 10, false),
                                     DL.getTypeSizeInBits(node->getType()));
        NodeExprMap.insert({node, result});
        return result;
      }
      if (auto *cds = dyn_cast<ConstantDataSequential>(value)) {
        Type *element_type = cds->getElementType();
        if (!element_type->isFloatTy() && !element_type->isDoubleTy() &&
            !element_type->isIntegerTy()) {
          result = Factory->createBitVecConst(
              symbol, DL.getTypeSizeInBits(node->getType()));
        } else {
          unsigned element_num = cds->getNumElements();
          uint64_t elem_size =
              DL.getTypeSizeInBits(node->getType()) / std::max(1u, element_num);
          for (unsigned idx = 0; idx < element_num; ++idx) {
            uint64_t elem = 0;
            if (element_type->isFloatTy())
              elem = static_cast<uint64_t>(cds->getElementAsFloat(idx));
            else if (element_type->isDoubleTy())
              elem = static_cast<uint64_t>(cds->getElementAsDouble(idx));
            else
              elem = cds->getElementAsInteger(idx);

            SMTExpr next = Factory->createBitVecVal(elem, elem_size);
            result = idx == 0 ? next : result.basic_concat(next);
          }
        }
        NodeExprMap.insert({node, result});
        return result;
      }
    }
  }

  result =
      Factory->createBitVecConst(symbol, DL.getTypeSizeInBits(node->getType()));
  NodeExprMap.insert({node, result});
  return result;
}

SMTExpr GuardedValueFlowSolver::encodeOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  auto existing = OpcodeConstraintsCacheMap.find(node);
  if (existing != OpcodeConstraintsCacheMap.end()) {
    for (const auto &edge : node->children()) {
      if (isa<GuardedValueFlowArgumentNode>(edge.target) &&
          !FunctionArgumentCache.contains(edge.target)) {
        FunctionArgumentCache.add(edge.target);
      } else if (auto *call_output =
                     dyn_cast<GuardedValueFlowCallOutputNode>(edge.target)) {
        if (!CallSiteOutputCache.contains(call_output))
          CallSiteOutputCache.add(call_output);
      }
    }
    return existing->second;
  }

  SMTExpr result = Factory->createEmptySMTExpr();
  switch (node->getOpcodeKind()) {
  case GuardedValueFlowOpcodeNode::OpcodeKind::Trunc:
  case GuardedValueFlowOpcodeNode::OpcodeKind::ZExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::IntToPtr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::PtrToInt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::BitCast:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPTrunc:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SIToFP:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPToSI:
  case GuardedValueFlowOpcodeNode::OpcodeKind::UIToFP:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPToUI:
  case GuardedValueFlowOpcodeNode::OpcodeKind::AddrSpaceCast:
    result = encodeCastOpcodeNode(node);
    break;
  case GuardedValueFlowOpcodeNode::OpcodeKind::Select:
    result = encodeSelectOpcodeNode(node);
    break;
  case GuardedValueFlowOpcodeNode::OpcodeKind::ExtractElement:
    result = encodeExtractElementOpcodeNode(node);
    break;
  case GuardedValueFlowOpcodeNode::OpcodeKind::InsertElement:
    result = encodeInsertElementOpcodeNode(node);
    break;
  case GuardedValueFlowOpcodeNode::OpcodeKind::ICmp:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FCmp:
    result = encodeCompareOpcodeNode(node);
    break;
  case GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr:
    result = encodeGEPOpcodeNode(node);
    break;
  case GuardedValueFlowOpcodeNode::OpcodeKind::Concat:
    result = encodeConcatOpcodeNode(node);
    break;
  default:
    assert(isBinaryOpcodeKind(node->getOpcodeKind()) && "Unsupported opcode");
    result = encodeBinaryOpcodeNode(node);
    break;
  }

  OpcodeConstraintsCacheMap.insert({node, result});
  return result;
}

SMTExpr GuardedValueFlowSolver::encodeBinaryOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  SMTExpr lhs = getOrInsertExpr(node->children()[0].target);
  SMTExpr rhs = Factory->createEmptySMTExpr();
  SMTExpr self = getOrInsertExpr(node);
  unsigned elem_num = getVectorElementCount(node->getType());

  if (node->children().size() == 2) {
    rhs = getOrInsertExpr(node->children()[1].target);
  } else {
    rhs = Factory->createBitVecVal(node->getIntConstant(), lhs.getBitVecSize());
  }

  switch (node->getOpcodeKind()) {
  case GuardedValueFlowOpcodeNode::OpcodeKind::FAdd:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Add:
    return self == lhs.array_add(rhs, elem_num);
  case GuardedValueFlowOpcodeNode::OpcodeKind::FSub:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Sub:
    return self == lhs.array_sub(rhs, elem_num);
  case GuardedValueFlowOpcodeNode::OpcodeKind::FMul:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Mul: {
    SMTExpr ret = self == lhs.array_mul(rhs, elem_num);
    if (GVFGIgnoreBitVecOverflow.getValue())
      ret = ret && (lhs == 0 || rhs == 0 || self != 0);
    return ret;
  }
  case GuardedValueFlowOpcodeNode::OpcodeKind::UDiv: {
    SMTExpr ret = self == lhs.array_udiv(rhs, elem_num);
    if (GVFGIgnoreBitVecOverflow.getValue())
      ret = ret && (lhs == 0 || rhs == 0 || self != 0);
    return ret;
  }
  case GuardedValueFlowOpcodeNode::OpcodeKind::FDiv:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SDiv: {
    SMTExpr ret = self == lhs.array_sdiv(rhs, elem_num);
    if (GVFGIgnoreBitVecOverflow.getValue())
      ret = ret && (lhs == 0 || rhs == 0 || self != 0);
    return ret;
  }
  case GuardedValueFlowOpcodeNode::OpcodeKind::URem:
    return self == lhs.array_urem(rhs, elem_num);
  case GuardedValueFlowOpcodeNode::OpcodeKind::SRem:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FRem:
    return self == lhs.array_srem(rhs, elem_num);
  case GuardedValueFlowOpcodeNode::OpcodeKind::Shl: {
    SMTExpr ret = self == lhs.array_shl(rhs, elem_num);
    if (GVFGIgnoreBitVecOverflow.getValue())
      ret = ret && (lhs == 0 || rhs >= 10 || self != 0);
    return ret;
  }
  case GuardedValueFlowOpcodeNode::OpcodeKind::LShr: {
    SMTExpr ret = self == lhs.array_lshr(rhs, elem_num);
    if (GVFGIgnoreBitVecOverflow.getValue())
      ret = ret && (lhs == 0 || rhs >= 10 || self != 0);
    return ret;
  }
  case GuardedValueFlowOpcodeNode::OpcodeKind::AShr: {
    SMTExpr ret = self == lhs.array_ashr(rhs, elem_num);
    if (GVFGIgnoreBitVecOverflow.getValue())
      ret = ret && (lhs == 0 || rhs >= 10 || self != 0);
    return ret;
  }
  case GuardedValueFlowOpcodeNode::OpcodeKind::And:
    return self == lhs.array_and(rhs, elem_num);
  case GuardedValueFlowOpcodeNode::OpcodeKind::Or:
    return self == lhs.array_or(rhs, elem_num);
  case GuardedValueFlowOpcodeNode::OpcodeKind::Xor:
    return self == lhs.array_xor(rhs, elem_num);
  default:
    llvm_unreachable("Unsupported binary opcode");
  }
}

SMTExpr GuardedValueFlowSolver::encodeCompareOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  SMTExpr self = getOrInsertExpr(node);
  SMTExpr lhs = getOrInsertExpr(node->children()[0].target);
  SMTExpr rhs = getOrInsertExpr(node->children()[1].target);
  unsigned elem_num = getVectorElementCount(node->getType());

  switch (static_cast<CmpInst::Predicate>(node->getCmpPredicate())) {
  case CmpInst::FCMP_FALSE:
  case CmpInst::FCMP_UNO:
    return self == Factory->createBitVecVal(0, self.getBitVecSize());
  case CmpInst::FCMP_OEQ:
  case CmpInst::FCMP_UEQ:
  case CmpInst::ICMP_EQ:
    return self == lhs.array_eq(rhs, elem_num);
  case CmpInst::FCMP_ONE:
  case CmpInst::FCMP_UNE:
  case CmpInst::ICMP_NE:
    return self == lhs.array_ne(rhs, elem_num);
  case CmpInst::ICMP_UGT:
    return self == lhs.array_ugt(rhs, elem_num);
  case CmpInst::ICMP_UGE:
    return self == lhs.array_uge(rhs, elem_num);
  case CmpInst::ICMP_ULT:
    return self == lhs.array_ult(rhs, elem_num);
  case CmpInst::ICMP_ULE:
    return self == lhs.array_ule(rhs, elem_num);
  case CmpInst::FCMP_OGT:
  case CmpInst::FCMP_UGT:
  case CmpInst::ICMP_SGT:
    return self == lhs.array_sgt(rhs, elem_num);
  case CmpInst::FCMP_OGE:
  case CmpInst::FCMP_UGE:
  case CmpInst::ICMP_SGE:
    return self == lhs.array_sge(rhs, elem_num);
  case CmpInst::FCMP_OLT:
  case CmpInst::FCMP_ULT:
  case CmpInst::ICMP_SLT:
    return self == lhs.array_slt(rhs, elem_num);
  case CmpInst::FCMP_OLE:
  case CmpInst::FCMP_ULE:
  case CmpInst::ICMP_SLE:
    return self == lhs.array_sle(rhs, elem_num);
  case CmpInst::FCMP_ORD:
  case CmpInst::FCMP_TRUE:
    return self == Factory->createBitVecVal(static_cast<uint64_t>(-1),
                                            self.getBitVecSize());
  default:
    llvm_unreachable("Unsupported compare predicate");
  }
}

SMTExpr GuardedValueFlowSolver::encodeCastOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  SMTExpr self = getOrInsertExpr(node);
  SMTExpr child = getOrInsertExpr(node->children()[0].target);
  unsigned elem_num = getVectorElementCount(node->getType());
  uint64_t origin_size = node->getCastSrcBits();
  uint64_t target_size = node->getCastDstBits();

  switch (node->getOpcodeKind()) {
  case GuardedValueFlowOpcodeNode::OpcodeKind::Trunc:
    return self == child.array_trunc(origin_size - target_size, elem_num);
  case GuardedValueFlowOpcodeNode::OpcodeKind::ZExt:
    return self == child.array_zext(target_size - origin_size, elem_num);
  case GuardedValueFlowOpcodeNode::OpcodeKind::SExt:
    return self == child.array_sext(target_size - origin_size, elem_num);
  case GuardedValueFlowOpcodeNode::OpcodeKind::PtrToInt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::IntToPtr:
    if (origin_size < target_size)
      return self == child.array_zext(target_size - origin_size, elem_num);
    if (origin_size > target_size)
      return self == child.array_trunc(origin_size - target_size, elem_num);
    return self == child;
  case GuardedValueFlowOpcodeNode::OpcodeKind::BitCast:
    return self == child;
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPTrunc:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::UIToFP:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SIToFP:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPToSI:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPToUI:
    if (target_size == origin_size)
      return self == child;
    if (target_size < origin_size)
      return self == child.array_trunc(origin_size - target_size, elem_num);
    return self == child.array_sext(target_size - origin_size, elem_num);
  case GuardedValueFlowOpcodeNode::OpcodeKind::AddrSpaceCast:
    return Factory->createBoolVal(true);
  default:
    llvm_unreachable("Unsupported cast opcode");
  }
}

SMTExpr GuardedValueFlowSolver::encodeGEPOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  SMTExpr base = getOrInsertExpr(node->children()[0].target);
  SMTExpr computed = getOrInsertExpr(node->children()[1].target);
  SMTExpr null_expr = Factory->createBitVecVal(0, base.getBitVecSize());

  return (getOrInsertExpr(node) ==
          (base == null_expr).basic_ite(null_expr, computed)) &&
         (base == null_expr || computed != null_expr);
}

SMTExpr GuardedValueFlowSolver::encodeSelectOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  SMTExpr cond = getOrInsertExpr(node->children()[0].target);
  SMTExpr true_value = getOrInsertExpr(node->children()[1].target);
  SMTExpr false_value = getOrInsertExpr(node->children()[2].target);
  unsigned elem_num =
      getVectorElementCount(node->children()[0].target->getType());
  return getOrInsertExpr(node) ==
         cond.array_ite(true_value, false_value, elem_num);
}

SMTExpr GuardedValueFlowSolver::encodeExtractElementOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  SMTExpr self = getOrInsertExpr(node);
  SMTExpr vec = getOrInsertExpr(node->children()[0].target);
  const GuardedValueFlowNode *index_node = node->children()[1].target;
  Value *index_value = index_node ? index_node->getLLVMValue() : nullptr;
  unsigned elem_num =
      getVectorElementCount(node->children()[0].target->getType());

  if (auto *constant_index = dyn_cast_or_null<ConstantInt>(index_value))
    return self == vec.array_elmt(elem_num, constant_index->getZExtValue());

  SMTExpr index_expr = getOrInsertExpr(index_node);
  SMTExpr ret = (index_expr != 0 || self == vec.array_elmt(elem_num, 0));
  for (unsigned idx = 1; idx < elem_num; ++idx)
    ret = ret && (index_expr != static_cast<int>(idx) ||
                  self == vec.array_elmt(elem_num, idx));
  return ret && (index_expr >= 0 && index_expr < static_cast<int>(elem_num));
}

SMTExpr GuardedValueFlowSolver::encodeInsertElementOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  SMTExpr self = getOrInsertExpr(node);
  SMTExpr vec = getOrInsertExpr(node->children()[0].target);
  SMTExpr value = getOrInsertExpr(node->children()[2].target);
  const GuardedValueFlowNode *index_node = node->children()[1].target;
  Value *index_value = index_node ? index_node->getLLVMValue() : nullptr;

  Type *vec_type = node->children()[0].target->getType();
  unsigned elem_num = getVectorElementCount(vec_type);
  uint64_t vec_size = vec.getBitVecSize();
  uint64_t elem_size = vec_size / std::max(1u, elem_num);

  auto build_case = [&](unsigned idx) {
    if (idx == 0) {
      return self.array_elmt(elem_num, 0) == value &&
             self.basic_extract(vec_size - elem_size - 1, 0) ==
                 vec.basic_extract(vec_size - elem_size - 1, 0);
    }
    if (idx == elem_num - 1) {
      return self.array_elmt(elem_num, elem_num - 1) == value &&
             self.basic_extract(vec_size - 1, elem_size) ==
                 vec.basic_extract(vec_size - 1, elem_size);
    }
    return self.array_elmt(elem_num, idx) == value &&
           self.basic_extract(vec_size - 1, vec_size - idx * elem_size) ==
               vec.basic_extract(vec_size - 1, vec_size - idx * elem_size) &&
           self.basic_extract(vec_size - (idx + 1) * elem_size - 1, 0) ==
               vec.basic_extract(vec_size - (idx + 1) * elem_size - 1, 0);
  };

  if (auto *constant_index = dyn_cast_or_null<ConstantInt>(index_value))
    return build_case(constant_index->getZExtValue());

  SMTExpr index_expr = getOrInsertExpr(index_node);
  SMTExpr ret = (index_expr != 0 || build_case(0));
  for (unsigned idx = 1; idx < elem_num; ++idx)
    ret = ret && (index_expr != static_cast<int>(idx) || build_case(idx));
  return ret && (index_expr >= 0 && index_expr < static_cast<int>(elem_num));
}

SMTExpr GuardedValueFlowSolver::encodeConcatOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  SMTExpr lhs = getOrInsertExpr(node);
  SMTExpr rhs = getOrInsertExpr(node->children()[0].target);
  for (size_t idx = 1; idx < node->children().size(); ++idx) {
    SMTExpr next = getOrInsertExpr(node->children()[idx].target);
    rhs = rhs.basic_concat(next);
  }
  return lhs == rhs;
}

std::pair<SMTExprVec, SMTExprVec> GuardedValueFlowSolver::_getPhiGated(
    const GuardedValueFlowPhiNode *phi_node,
    GuardedValueFlowPhiNode::Incoming incoming) {
  auto *graph = incoming.value_node->getGraph();
  assert(graph && incoming.incoming_block);

  auto gated = _getCtrlDeps(incoming.incoming_block, graph);
  SMTExprVec gated_ctrl = gated.first.copy();
  SMTExprVec gated_data = gated.second;

  if (incoming.condition_node) {
    SMTExpr cond_expr = getOrInsertExpr(incoming.condition_node).bv12bool();
    gated_ctrl.push_back(incoming.condition_sense ? cond_expr : !cond_expr);
    gated_data =
        SMTExprVec::merge(gated_data, _getDataDeps(incoming.condition_node));
  }
  return {gated_ctrl, gated_data};
}

std::pair<SMTExprVec, SMTExprVec>
GuardedValueFlowSolver::computeCtrlDepsPair(BasicBlock *block,
                                            const GuardedValueFlowGraph *graph,
                                            const QueryContext *context) {
  (void)context;
  return _getCtrlDeps(block, graph);
}

std::pair<SMTExprVec, SMTExprVec> GuardedValueFlowSolver::computePhiGatedPair(
    const GuardedValueFlowPhiNode *phi_node,
    GuardedValueFlowPhiNode::Incoming incoming, const QueryContext *context) {
  (void)context;
  return _getPhiGated(phi_node, incoming);
}

SMTExprVec
GuardedValueFlowSolver::computeDataDeps(const GuardedValueFlowNode *node,
                                        const QueryContext *context) {
  (void)context;
  return _getDataDeps(node);
}

std::pair<SMTExprVec, SMTExprVec> GuardedValueFlowSolver::_getCtrlDeps(
    BasicBlock *block, const GuardedValueFlowGraph *graph, size_t depth) {
  assert(block && graph);

  if (GVFGUseRegionAsCtrlDep.getValue()) {
    auto *region =
        const_cast<GuardedValueFlowGraph *>(graph)->findRegion(block);
    if (!region)
      region =
          const_cast<GuardedValueFlowGraph *>(graph)->getAlwaysTrueRegion();
    SMTExprVec ctrl = Factory->createEmptySMTExprVec();
    SMTExprVec data = _getDataDeps(region);
    ctrl.push_back(getOrInsertExpr(region).bv12bool());
    return {ctrl, data};
  }

  if (depth >= GVFGCDMaxDepth) {
    SMTExprVec ctrl = Factory->createEmptySMTExprVec();
    SMTExprVec data = Factory->createEmptySMTExprVec();
    ctrl.push_back(Factory->createBoolConst(
        (Twine("BB") + Twine(reinterpret_cast<uintptr_t>(block))).str()));
    return {ctrl, data};
  }

  auto cached = CtrlCacheMap.find(block);
  if (cached != CtrlCacheMap.end()) {
    SMTExprVec ctrl = cached->second;
    SMTExprVec data = Factory->createEmptySMTExprVec();

    if (BBCache.contains(block))
      return {ctrl, data};

    std::unordered_set<BasicBlock *> visited;
    SmallVector<
        std::pair<const GuardedValueFlowGraph::BlockCondition *, size_t>, 8>
        data_dep_nodes;
    for (const auto &cond : graph->getBlockConditions(block))
      data_dep_nodes.push_back({&cond, depth});

    while (!data_dep_nodes.empty()) {
      auto entry = data_dep_nodes.pop_back_val();
      const GuardedValueFlowGraph::BlockCondition *data_dep_node = entry.first;
      size_t dfs_depth = entry.second;
      if (dfs_depth >= GVFGCDMaxDepth)
        continue;

      visited.insert(data_dep_node->control_block);
      data =
          SMTExprVec::merge(data, _getDataDeps(data_dep_node->condition_node));

      BasicBlock *control_block = data_dep_node->control_block;
      if (control_block && !BBCache.contains(control_block)) {
        BBCache.add(control_block);
        for (const auto &cond : graph->getBlockConditions(control_block)) {
          if (!visited.count(cond.control_block))
            data_dep_nodes.push_back({&cond, dfs_depth + 1});
        }
      }
    }

    return {ctrl, data};
  }

  SMTExprVec ctrl = Factory->createEmptySMTExprVec();
  SMTExprVec data = Factory->createEmptySMTExprVec();

  auto block_conditions = graph->getBlockConditions(block);
  if (!block_conditions.empty()) {
    SMTExprVec conditions = Factory->createEmptySMTExprVec();
    for (const auto &cond : block_conditions) {
      SMTExpr cond_expr = getOrInsertExpr(cond.condition_node).bv12bool();
      auto pair = _getCtrlDeps(cond.control_block, graph, depth + 1);
      conditions.push_back((cond.sense ? cond_expr : !cond_expr) &&
                           pair.first.toAndExpr());
      data = SMTExprVec::merge(data, pair.second);
    }

    ctrl.push_back(conditions.toOrExpr());
    if (!BBCache.contains(block)) {
      BBCache.add(block);
      for (const auto &cond : block_conditions)
        data = SMTExprVec::merge(data, _getDataDeps(cond.condition_node));
    }
  }

  CtrlCacheMap.insert({block, ctrl});
  return {ctrl, data};
}

SMTExprVec
GuardedValueFlowSolver::_getDataDeps(const GuardedValueFlowNode *node,
                                     size_t depth) {
  SMTExprVec ret = Factory->createEmptySMTExprVec();
  if (!node)
    return ret;

  if (depth >= GVFGDDMaxDepth)
    return ret;

  if (ConstraintCache.contains(node)) {
    if (GVFGDDMaxDepth.getNumOccurrences()) {
      for (const auto &edge : node->children()) {
        if (edge.target && !isTerminalNode(edge.target))
          ret = SMTExprVec::merge(ret, _getDataDeps(edge.target, depth + 1));
      }
    }
    return ret;
  }
  ConstraintCache.add(node);

  if (isResultLikeNode(node)) {
    SMTExpr self = getOrInsertExpr(node);

    if (auto *phi_node = dyn_cast<GuardedValueFlowPhiNode>(node)) {
      for (const auto &incoming : phi_node->incoming()) {
        auto gated = _getPhiGated(phi_node, incoming);
        ret.push_back((!gated.first.toAndExpr()) ||
                      (self == getOrInsertExpr(incoming.value_node)));
        ret = SMTExprVec::merge(ret, gated.second);
      }
    } else if (auto *return_node = dyn_cast<GuardedValueFlowReturnNode>(node)) {
      for (const auto &edge : return_node->children()) {
        auto *child = edge.target;
        auto *site = return_node->getReturnSite(child);
        BasicBlock *site_block = site && site->getInstruction()
                                     ? site->getInstruction()->getParent()
                                     : return_node->getParentBasicBlock();
        auto gated = _getCtrlDeps(site_block, return_node->getGraph());
        ret.push_back(!gated.first.toAndExpr() ||
                      self == getOrInsertExpr(child));
        ret = SMTExprVec::merge(ret, gated.second);
      }
    } else if (node->getKind() == GuardedValueFlowNode::Kind::LoadMemory) {
      for (const auto &edge : node->children()) {
        auto *child = edge.target;
        if (!child || shouldExcludeSummaryBackedChild(node, child))
          continue;
        SMTExpr cond_expr = getMatchingRegionExpr(*this, node, child);
        ret = SMTExprVec::merge(ret,
                                _getDataDeps(node->getMatchingRegion(child)));
        ret.push_back((!cond_expr) || (self == getOrInsertExpr(child)));
      }
    }

    if (!isTerminalNode(node)) {
      SMTExprVec cases = Factory->createEmptySMTExprVec();
      for (const auto &edge : node->children()) {
        if (!edge.target || shouldExcludeSummaryBackedChild(node, edge.target))
          continue;
        cases.push_back(self == getOrInsertExpr(edge.target));
      }

      if (!cases.empty())
        ret.push_back(cases.toOrExpr());

      for (const auto &edge : node->children()) {
        if (edge.target && !shouldExcludeSummaryBackedChild(node, edge.target))
          ret = SMTExprVec::merge(ret, _getDataDeps(edge.target, depth + 1));
      }
    } else if (isNonNullTerminalValue(node)) {
      ret.push_back(self != 0);
    }
  } else if (auto *opcode_node = dyn_cast<GuardedValueFlowOpcodeNode>(node)) {
    ret.push_back(encodeOpcodeNode(opcode_node));
    for (const auto &edge : node->children()) {
      if (edge.target)
        ret = SMTExprVec::merge(ret, _getDataDeps(edge.target, depth + 1));
    }
  } else {
    llvm_unreachable("Unhandled guarded value-flow node category");
  }

  return ret;
}

SMTExprVec GuardedValueFlowSolver::getDeps(const GuardedValueFlowNode *node,
                                           const GuardedValueFlowNode *child) {
  assert(node && (!child || child->containsParent(node)));

  SMTExprVec ret = Factory->createEmptySMTExprVec();
  if (isa<GuardedValueFlowOpcodeNode>(node) || !child) {
    if (auto *opcode_node = dyn_cast<GuardedValueFlowOpcodeNode>(node)) {
      if (opcode_node->getOpcodeKind() ==
              GuardedValueFlowOpcodeNode::OpcodeKind::Select &&
          child) {
        ConstraintCache.add(node);
        SMTExpr node_expr = getOrInsertExpr(node);
        SMTExpr child_expr = getOrInsertExpr(child);
        ret.push_back(node_expr == child_expr);
        if (node->children()[1].target == child)
          ret.push_back(getOrInsertExpr(node->children()[0].target) == 1);
        else
          ret.push_back(getOrInsertExpr(node->children()[0].target) == 0);
        ret = SMTExprVec::merge(_getDataDeps(node->children()[0].target), ret);
      } else {
        ret = SMTExprVec::merge(_getDataDeps(node), ret);
      }
    } else {
      ret = SMTExprVec::merge(_getDataDeps(node), ret);
    }
  } else {
    ConstraintCache.add(node);
    SMTExpr node_expr = getOrInsertExpr(node);
    SMTExpr child_expr = getOrInsertExpr(child);
    ret.push_back(node_expr == child_expr);

    if (auto *phi_node = dyn_cast<GuardedValueFlowPhiNode>(node)) {
      SMTExprVec gated_ctrls = Factory->createEmptySMTExprVec();
      for (const auto &incoming : phi_node->incoming()) {
        if (incoming.value_node == child) {
          auto pair = _getPhiGated(phi_node, incoming);
          gated_ctrls.push_back(pair.first.toAndExpr());
          ret = SMTExprVec::merge(ret, pair.second);
        }
      }
      ret.push_back(gated_ctrls.toOrExpr());
      return ret;
    } else if (node->getKind() == GuardedValueFlowNode::Kind::LoadMemory) {
      auto *region = node->getMatchingRegion(child);
      if (region) {
        ret = SMTExprVec::merge(ret, _getDataDeps(region));
        ret.push_back(getOrInsertExpr(region).bv12bool());
      }
    }
  }

  if (!child || node->getParentBasicBlock() != child->getParentBasicBlock()) {
    auto pair = _getCtrlDeps(node->getParentBasicBlock(), node->getGraph());
    ret.mergeWithAnd(pair.first);
    ret = SMTExprVec::merge(ret, pair.second);
  }
  return ret;
}

std::pair<SMTExprVec, SMTExprVec>
GuardedValueFlowSolver::getDepsPair(const GuardedValueFlowNode *node,
                                    const GuardedValueFlowNode *child) {
  assert(node && (!child || child->containsParent(node)));
  SMTExprVec ctrl = Factory->createEmptySMTExprVec();
  SMTExprVec data = Factory->createEmptySMTExprVec();

  data = SMTExprVec::merge(_getDataDeps(node), data);
  if (!(isa<GuardedValueFlowOpcodeNode>(node) || !child)) {
    if (auto *phi_node = dyn_cast<GuardedValueFlowPhiNode>(node)) {
      SMTExprVec gated = Factory->createEmptySMTExprVec();
      for (const auto &incoming : phi_node->incoming()) {
        if (incoming.value_node == child)
          gated.push_back(_getPhiGated(phi_node, incoming).first.toAndExpr());
      }
      ctrl.push_back(gated.toOrExpr());
      return {ctrl, data};
    } else if (node->getKind() == GuardedValueFlowNode::Kind::LoadMemory) {
      if (auto *region = node->getMatchingRegion(child))
        ctrl.push_back(getOrInsertExpr(region).bv12bool());
    }
  } else if (auto *opcode_node = dyn_cast<GuardedValueFlowOpcodeNode>(node)) {
    if (opcode_node->getOpcodeKind() ==
            GuardedValueFlowOpcodeNode::OpcodeKind::Select &&
        child) {
      if (node->children()[1].target == child)
        ctrl.push_back(getOrInsertExpr(node->children()[0].target) == 1);
      else
        ctrl.push_back(getOrInsertExpr(node->children()[0].target) == 0);
    }
  }

  if (!child || node->getParentBasicBlock() != child->getParentBasicBlock()) {
    auto pair = _getCtrlDeps(node->getParentBasicBlock(), node->getGraph());
    ctrl = SMTExprVec::merge(ctrl, pair.first);
    data = SMTExprVec::merge(data, pair.second);
  }
  return {ctrl, data};
}

void GuardedValueFlowSolver::push() {
  SMTSolver::push();
  ConstraintCache.push();
  FunctionArgumentCache.push();
  CallSiteOutputCache.push();
  BBCache.push();
}

void GuardedValueFlowSolver::pop(unsigned n) {
  SMTSolver::pop(n);
  ConstraintCache.pop(n);
  FunctionArgumentCache.pop(n);
  CallSiteOutputCache.pop(n);
  BBCache.pop(n);
}

void GuardedValueFlowSolver::reset() {
  SMTSolver::reset();
  ConstraintCache.reset();
  FunctionArgumentCache.reset();
  CallSiteOutputCache.reset();
  BBCache.reset();
  CtrlCacheMap.clear();
  OpcodeConstraintsCacheMap.clear();
  NodeExprMap.clear();
  NodeSymbolNameMap.clear();
}

GuardedValueFlowSolver::SMTResultType GuardedValueFlowSolver::check() {
  return SMTSolver::check();
}

std::pair<SMTExprVec, SMTExprVec> DTGuardedValueFlowSolver::computeCtrlDepsPair(
    BasicBlock *block, const GuardedValueFlowGraph *graph,
    const QueryContext *context) {
  BasicBlock *prev_block = context ? context->previous_block : nullptr;
  return _getCtrlDepsWrapper(block, graph, prev_block);
}

std::pair<SMTExprVec, SMTExprVec> DTGuardedValueFlowSolver::computePhiGatedPair(
    const GuardedValueFlowPhiNode *phi_node,
    GuardedValueFlowPhiNode::Incoming incoming, const QueryContext *context) {
  assert(context && context->previous_block);
  return _getPhiGated(phi_node, incoming, context->previous_block);
}

SMTExprVec
DTGuardedValueFlowSolver::computeDataDeps(const GuardedValueFlowNode *node,
                                          const QueryContext *context) {
  assert(context && context->previous_block);
  return _getDataDeps(node, context->previous_block);
}

std::pair<SMTExprVec, SMTExprVec> DTGuardedValueFlowSolver::_getPhiGated(
    const GuardedValueFlowPhiNode *phi_node,
    GuardedValueFlowPhiNode::Incoming incoming, BasicBlock *prev_block) {
  assert(prev_block && incoming.incoming_block);
  auto pair = _getCtrlDepsWrapper(incoming.incoming_block, phi_node->getGraph(),
                                  prev_block);
  SMTExprVec ctrl = pair.first.copy();
  SMTExprVec data = pair.second;
  if (incoming.condition_node) {
    SMTExpr cond_expr = getOrInsertExpr(incoming.condition_node).bv12bool();
    ctrl.push_back(incoming.condition_sense ? cond_expr : !cond_expr);
    data = SMTExprVec::merge(_getDataDeps(incoming.condition_node, prev_block),
                             data);
  }
  return {ctrl, data};
}

SMTExprVec
DTGuardedValueFlowSolver::_getDataDeps(const GuardedValueFlowNode *node,
                                       BasicBlock *prev_block) {
  assert(prev_block);
  if (ConstraintCache.contains(node))
    return Factory->createEmptySMTExprVec();
  ConstraintCache.add(node);

  SMTExprVec ret = Factory->createEmptySMTExprVec();
  if (isResultLikeNode(node)) {
    SMTExpr self = getOrInsertExpr(node);
    if (auto *phi_node = dyn_cast<GuardedValueFlowPhiNode>(node)) {
      for (const auto &incoming : phi_node->incoming()) {
        auto pair = _getPhiGated(phi_node, incoming, prev_block);
        ret.push_back((!pair.first.toAndExpr()) ||
                      (self == getOrInsertExpr(incoming.value_node)));
        ret = SMTExprVec::merge(pair.second, ret);
      }
    }
    if (!isTerminalNode(node)) {
      SMTExprVec cases = Factory->createEmptySMTExprVec();
      for (const auto &edge : node->children()) {
        if (!edge.target || shouldExcludeSummaryBackedChild(node, edge.target))
          continue;
        cases.push_back(self == getOrInsertExpr(edge.target));
      }
      if (!cases.empty())
        ret.push_back(cases.toOrExpr());
      for (const auto &edge : node->children()) {
        if (edge.target &&
            !shouldExcludeSummaryBackedChild(node, edge.target) &&
            !isTerminalNode(edge.target))
          ret = SMTExprVec::merge(ret, _getDataDeps(edge.target, prev_block));
      }
    }
  } else if (auto *opcode_node = dyn_cast<GuardedValueFlowOpcodeNode>(node)) {
    ret.push_back(encodeOpcodeNode(opcode_node));
    for (const auto &edge : node->children()) {
      if (edge.target && !isTerminalNode(edge.target))
        ret = SMTExprVec::merge(ret, _getDataDeps(edge.target, prev_block));
    }
  }
  return ret;
}

std::pair<SMTExprVec, SMTExprVec> DTGuardedValueFlowSolver::_getCtrlDepsWrapper(
    BasicBlock *block, const GuardedValueFlowGraph *graph,
    BasicBlock *prev_block) {
  if (!prev_block)
    prev_block = &block->getParent()->getEntryBlock();

  if (!(DT->dominates(prev_block, block) || DT->dominates(block, prev_block)))
    prev_block = DT->findNearestCommonDominator(block, prev_block);
  return _getCtrlDeps(block, graph, prev_block);
}

std::pair<SMTExprVec, SMTExprVec>
DTGuardedValueFlowSolver::_getCtrlDeps(BasicBlock *block,
                                       const GuardedValueFlowGraph *graph,
                                       BasicBlock *prev_block) {
  assert(block && graph && prev_block && DT);

  SMTExprVec data = Factory->createEmptySMTExprVec();
  if (!DT->dominates(prev_block, block))
    return {Factory->createEmptySMTExprVec(), data};

  auto cached = CtrlCacheMap.find(block);
  if (cached != CtrlCacheMap.end()) {
    auto &pair = cached->second;
    BasicBlock *cached_prev = pair.second;

    if (!BBCache.contains(block)) {
      SmallVector<const GuardedValueFlowGraph::BlockCondition *, 8>
          data_dep_nodes;
      DenseSet<BasicBlock *> visited;
      for (const auto &cond : graph->getBlockConditions(block)) {
        if (cond.control_block && DT->dominates(prev_block, cond.control_block))
          data_dep_nodes.push_back(&cond);
      }

      while (!data_dep_nodes.empty()) {
        const auto *data_dep_node = data_dep_nodes.pop_back_val();
        if (data_dep_node->control_block)
          visited.insert(data_dep_node->control_block);
        data = SMTExprVec::merge(
            data, _getDataDeps(data_dep_node->condition_node, prev_block));

        BasicBlock *control_block = data_dep_node->control_block;
        if (control_block && !BBCache.contains(control_block)) {
          BBCache.add(control_block);
          for (const auto &cond : graph->getBlockConditions(control_block)) {
            if (cond.control_block && !visited.count(cond.control_block))
              data_dep_nodes.push_back(&cond);
          }
        }
      }
    }

    if (DT->dominates(cached_prev, prev_block)) {
      return {pair.first, data};
    } else if (DT->dominates(prev_block, cached_prev)) {
      auto partial = _getCtrlDeps(cached_prev, graph, prev_block);
      pair.first.mergeWithAnd(partial.first);
      pair.second = prev_block;
      return {pair.first, SMTExprVec::merge(data, partial.second)};
    }
    llvm_unreachable("Unexpected dominator relationship");
  }

  SMTExprVec ctrl = Factory->createEmptySMTExprVec();
  auto block_conditions = graph->getBlockConditions(block);
  if (!block_conditions.empty()) {
    SMTExprVec conditions = Factory->createEmptySMTExprVec();
    for (const auto &cond : block_conditions) {
      if (!cond.control_block || !DT->dominates(prev_block, cond.control_block))
        continue;
      SMTExpr cond_expr = getOrInsertExpr(cond.condition_node).bv12bool();
      auto pair = _getCtrlDeps(cond.control_block, graph, prev_block);
      conditions.push_back((cond.sense ? cond_expr : !cond_expr) &&
                           pair.first.toAndExpr());
      data = SMTExprVec::merge(data, pair.second);
    }

    ctrl.push_back(conditions.toOrExpr());
    if (!BBCache.contains(block)) {
      BBCache.add(block);
      for (const auto &cond : block_conditions) {
        if (cond.control_block && DT->dominates(prev_block, cond.control_block))
          data = SMTExprVec::merge(
              data, _getDataDeps(cond.condition_node, prev_block));
      }
    }
  }

  CtrlCacheMap.insert({block, {ctrl, prev_block}});
  return {ctrl, data};
}

SMTExprVec
DTGuardedValueFlowSolver::getDeps(const GuardedValueFlowNode *node,
                                  const GuardedValueFlowNode *child) {
  BasicBlock *current_block = node->getParentBasicBlock();
  BasicBlock *prev_block = child ? child->getParentBasicBlock()
                                 : &current_block->getParent()->getEntryBlock();

  SMTExprVec ret = Factory->createEmptySMTExprVec();
  if (isa<GuardedValueFlowOpcodeNode>(node) || !child) {
    ret = SMTExprVec::merge(ret, _getDataDeps(node, prev_block));
  } else {
    ConstraintCache.add(node);
    SMTExpr node_expr = getOrInsertExpr(node);
    SMTExpr child_expr = getOrInsertExpr(child);
    ret.push_back(node_expr == child_expr);

    if (auto *phi_node = dyn_cast<GuardedValueFlowPhiNode>(node)) {
      SMTExprVec gated_ctrls = Factory->createEmptySMTExprVec();
      for (const auto &incoming : phi_node->incoming()) {
        if (incoming.value_node == child) {
          auto pair = _getPhiGated(phi_node, incoming, prev_block);
          gated_ctrls.push_back(pair.first.toAndExpr());
          ret = SMTExprVec::merge(ret, pair.second);
        }
      }
      ret.push_back(gated_ctrls.toOrExpr());
      return ret;
    }
  }

  auto pair = _getCtrlDepsWrapper(current_block, node->getGraph(), prev_block);
  ret.mergeWithAnd(pair.first);
  ret = SMTExprVec::merge(ret, pair.second);
  return ret;
}

void DTGuardedValueFlowSolver::reset() {
  GuardedValueFlowSolver::reset();
  CtrlCacheMap.clear();
}
