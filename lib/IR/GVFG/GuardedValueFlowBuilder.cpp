#include "IR/GSA/GSA.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"

#include <algorithm>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::gvfg;

namespace {

static void
recordBuilderDiagnostic(GuardedValueFlowGraph &graph,
                        GuardedValueFlowGraph::Diagnostic::Severity severity,
                        const Twine &message, Instruction *inst = nullptr,
                        BasicBlock *block = nullptr) {
  GuardedValueFlowGraph::Diagnostic diagnostic;
  diagnostic.origin = GuardedValueFlowGraph::Diagnostic::Origin::Builder;
  diagnostic.severity = severity;
  diagnostic.message = message.str();
  diagnostic.instruction = inst;
  diagnostic.block = block;
  graph.addDiagnostic(std::move(diagnostic));
}

static BasicBlock *getValueBlock(Value *V, Function &F) {
  if (auto *I = dyn_cast<Instruction>(V))
    return I->getParent();
  return F.empty() ? nullptr : &F.getEntryBlock();
}

static void setValueDescription(GuardedValueFlowNode *node, Value *value) {
  if (!node || !value)
    return;
  if (isa<ConstantPointerNull>(value) || isa<ConstantAggregateZero>(value)) {
    node->setDescription("0");
    return;
  }
  if (value->hasName()) {
    node->setDescription(value->getName().str());
    return;
  }

  std::string buffer;
  raw_string_ostream os(buffer);
  value->printAsOperand(os, false);
  node->setDescription(os.str());
}

static GuardedValueFlowOpcodeNode::OpcodeKind translateOpcode(unsigned opcode) {
  switch (opcode) {
  case Instruction::URem:
    return GuardedValueFlowOpcodeNode::OpcodeKind::URem;
  case Instruction::FRem:
    return GuardedValueFlowOpcodeNode::OpcodeKind::FRem;
  case Instruction::SRem:
    return GuardedValueFlowOpcodeNode::OpcodeKind::SRem;
  case Instruction::UDiv:
    return GuardedValueFlowOpcodeNode::OpcodeKind::UDiv;
  case Instruction::SDiv:
    return GuardedValueFlowOpcodeNode::OpcodeKind::SDiv;
  case Instruction::FDiv:
    return GuardedValueFlowOpcodeNode::OpcodeKind::FDiv;
  case Instruction::And:
    return GuardedValueFlowOpcodeNode::OpcodeKind::And;
  case Instruction::Or:
    return GuardedValueFlowOpcodeNode::OpcodeKind::Or;
  case Instruction::Xor:
    return GuardedValueFlowOpcodeNode::OpcodeKind::Xor;
  case Instruction::Shl:
    return GuardedValueFlowOpcodeNode::OpcodeKind::Shl;
  case Instruction::LShr:
    return GuardedValueFlowOpcodeNode::OpcodeKind::LShr;
  case Instruction::AShr:
    return GuardedValueFlowOpcodeNode::OpcodeKind::AShr;
  case Instruction::Mul:
    return GuardedValueFlowOpcodeNode::OpcodeKind::Mul;
  case Instruction::FMul:
    return GuardedValueFlowOpcodeNode::OpcodeKind::FMul;
  case Instruction::FAdd:
    return GuardedValueFlowOpcodeNode::OpcodeKind::FAdd;
  case Instruction::FSub:
    return GuardedValueFlowOpcodeNode::OpcodeKind::FSub;
  case Instruction::Add:
    return GuardedValueFlowOpcodeNode::OpcodeKind::Add;
  case Instruction::Sub:
    return GuardedValueFlowOpcodeNode::OpcodeKind::Sub;
  case Instruction::AddrSpaceCast:
    return GuardedValueFlowOpcodeNode::OpcodeKind::AddrSpaceCast;
  case Instruction::IntToPtr:
    return GuardedValueFlowOpcodeNode::OpcodeKind::IntToPtr;
  case Instruction::PtrToInt:
    return GuardedValueFlowOpcodeNode::OpcodeKind::PtrToInt;
  case Instruction::BitCast:
    return GuardedValueFlowOpcodeNode::OpcodeKind::BitCast;
  case Instruction::ZExt:
    return GuardedValueFlowOpcodeNode::OpcodeKind::ZExt;
  case Instruction::SExt:
    return GuardedValueFlowOpcodeNode::OpcodeKind::SExt;
  case Instruction::Trunc:
    return GuardedValueFlowOpcodeNode::OpcodeKind::Trunc;
  case Instruction::FPTrunc:
    return GuardedValueFlowOpcodeNode::OpcodeKind::FPTrunc;
  case Instruction::FPExt:
    return GuardedValueFlowOpcodeNode::OpcodeKind::FPExt;
  case Instruction::SIToFP:
    return GuardedValueFlowOpcodeNode::OpcodeKind::SIToFP;
  case Instruction::FPToSI:
    return GuardedValueFlowOpcodeNode::OpcodeKind::FPToSI;
  case Instruction::UIToFP:
    return GuardedValueFlowOpcodeNode::OpcodeKind::UIToFP;
  case Instruction::FPToUI:
    return GuardedValueFlowOpcodeNode::OpcodeKind::FPToUI;
  case Instruction::ExtractElement:
    return GuardedValueFlowOpcodeNode::OpcodeKind::ExtractElement;
  case Instruction::InsertElement:
    return GuardedValueFlowOpcodeNode::OpcodeKind::InsertElement;
  case Instruction::GetElementPtr:
    return GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr;
  case Instruction::Select:
    return GuardedValueFlowOpcodeNode::OpcodeKind::Select;
  case Instruction::ICmp:
    return GuardedValueFlowOpcodeNode::OpcodeKind::ICmp;
  case Instruction::FCmp:
    return GuardedValueFlowOpcodeNode::OpcodeKind::FCmp;
  default:
    return GuardedValueFlowOpcodeNode::OpcodeKind::Invalid;
  }
}

static GuardedValueFlowOpcodeNode *
createOpcodeNode(GuardedValueFlowGraph &graph,
                 GuardedValueFlowOpcodeNode::OpcodeKind opcode_kind, Type *type,
                 BasicBlock *block, GuardedValueFlowNode::Kind kind,
                 StringRef description) {
  auto *node = graph.createNode<GuardedValueFlowOpcodeNode>(kind, type, &graph,
                                                            block, opcode_kind);
  node->setDescription(description.str());
  return node;
}

static GuardedValueFlowNode *
findOrCreateSyntheticGuardNode(GuardedValueFlowGraph &graph, Instruction *inst,
                               BasicBlock *successor, StringRef description) {
  if (!inst || !successor)
    return nullptr;
  if (auto *existing = graph.findSyntheticGuardNode(inst, successor))
    return existing;

  auto *node = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand,
      Type::getInt1Ty(inst->getContext()), &graph, inst->getParent(), nullptr,
      inst);
  node->setDescription(description.str());
  graph.mapSyntheticGuardNode(inst, successor, node);
  return node;
}

static GuardedValueFlowNode *findOrCreateValueNode(GuardedValueFlowGraph &graph,
                                                   Value *V, Function &F);

static GuardedValueFlowNode *
findOrCreateSimpleNode(GuardedValueFlowGraph &graph, Value *V, Function &F,
                       GuardedValueFlowNode::Kind kind) {
  if (!V)
    return nullptr;
  if (auto *existing = graph.findNode(V))
    return existing;

  auto *node = graph.createNode<GuardedValueFlowNode>(
      kind, V->getType(), &graph, getValueBlock(V, F), V,
      dyn_cast<Instruction>(V));
  setValueDescription(node, V);
  graph.mapValueNode(V, node);
  return node;
}

static GuardedValueFlowCallOutputNode *
findOrCreateCallOutputNode(GuardedValueFlowGraph &graph, CallBase &call,
                           Function &F) {
  if (auto *existing = dyn_cast_or_null<GuardedValueFlowCallOutputNode>(
          graph.findNode(&call))) {
    return existing;
  }

  auto *site = graph.findCallSite(&call);
  if (!site) {
    site = graph.createSite<GuardedValueFlowCallSite>(&graph, &call);
    graph.mapCallSite(&call, site);
  }

  auto *output = graph.createNode<GuardedValueFlowCallOutputNode>(
      GuardedValueFlowNode::Kind::CallSiteCommonOutput, call.getType(), &graph,
      call.getParent(), &call, &call, call.getCalledFunction());
  output->setDescription("call.output");
  site->setCommonOutput(output);
  graph.mapValueNode(&call, output);
  return output;
}

static GuardedValueFlowNode *findOrCreateValueNode(GuardedValueFlowGraph &graph,
                                                   Value *V, Function &F) {
  if (!V)
    return nullptr;

  while (auto *GA = dyn_cast<GlobalAlias>(V))
    V = GA->getAliasee();

  if (auto *existing = graph.findNode(V))
    return existing;

  // Calls do not materialize as plain SSA operands. The callsite owns the
  // direct result channel so later interprocedural code can distinguish direct
  // returns from pseudo side-effect channels.
  if (auto *CB = dyn_cast<CallBase>(V)) {
    if (!CB->getType()->isVoidTy() &&
        !(CB->getCalledFunction() && CB->getCalledFunction()->isIntrinsic()))
      return findOrCreateCallOutputNode(graph, *CB, F);
  }

  if (auto *Arg = dyn_cast<Argument>(V)) {
    auto kind = Arg->getParent() ? GuardedValueFlowNode::Kind::CommonArgument
                                 : GuardedValueFlowNode::Kind::PseudoArgument;
    auto *node = graph.createNode<GuardedValueFlowArgumentNode>(
        kind, Arg->getType(), &graph, getValueBlock(Arg, F), Arg);
    setValueDescription(node, Arg);
    graph.mapValueNode(Arg, node);
    return node;
  }

  if (isa<VAArgInst>(V)) {
    auto *node = graph.createNode<GuardedValueFlowArgumentNode>(
        GuardedValueFlowNode::Kind::VariableArgument, V->getType(), &graph,
        getValueBlock(V, F), V);
    setValueDescription(node, V);
    graph.mapValueNode(V, node);
    return node;
  }

  if (isa<PHINode>(V)) {
    auto *node = graph.createNode<GuardedValueFlowPhiNode>(
        V->getType(), &graph, getValueBlock(V, F), V, dyn_cast<Instruction>(V));
    setValueDescription(node, V);
    graph.mapValueNode(V, node);
    return node;
  }

  if (isa<UndefValue>(V))
    return findOrCreateSimpleNode(graph, V, F,
                                  GuardedValueFlowNode::Kind::UndefValue);

  return findOrCreateSimpleNode(graph, V, F,
                                GuardedValueFlowNode::Kind::SimpleOperand);
}

static GuardedValueFlowNode *createConcatAggregate(GuardedValueFlowGraph &graph,
                                                   Value *aggregate,
                                                   Function &F, bool &failed);

static GuardedValueFlowNode *
createBinaryWithIntConst(GuardedValueFlowGraph &graph,
                         GuardedValueFlowOpcodeNode::OpcodeKind opcode_kind,
                         Type *type, BasicBlock *block,
                         GuardedValueFlowNode *operand, int64_t constant) {
  auto *opcode =
      createOpcodeNode(graph, opcode_kind, type, block,
                       GuardedValueFlowNode::Kind::SimpleOpcode, "bin.const");
  opcode->setIntConstant(constant);
  opcode->addChild(operand);
  return opcode;
}

static GuardedValueFlowNode *modelConstantExpr(ConstantExpr *CE,
                                               GuardedValueFlowGraph &graph,
                                               Function &F, bool &failed);

static GuardedValueFlowNode *
findOrCreateSwitchCasePredicate(GuardedValueFlowGraph &graph, SwitchInst *SI,
                                BasicBlock *successor, Function &F,
                                bool &failed);

static GuardedValueFlowNode *
getOrCreateOperandRepresentation(GuardedValueFlowGraph &graph, Value *V,
                                 Function &F, bool &failed) {
  if (!V)
    return nullptr;
  // Constants that behave like fully materialized values stay as ordinary
  // operand nodes. Aggregate constants and constant expressions are expanded so
  // downstream users can traverse through the same value-flow interface they
  // use for instructions.
  if (isa<ConstantPointerNull>(V) || isa<ConstantAggregateZero>(V))
    return findOrCreateValueNode(graph, V, F);
  if (isa<ConstantExpr>(V))
    return modelConstantExpr(cast<ConstantExpr>(V), graph, F, failed);
  if (isa<ConstantArray>(V) || isa<ConstantStruct>(V) ||
      isa<ConstantVector>(V) ||
      (isa<Constant>(V) && V->getType()->isAggregateType()))
    return createConcatAggregate(graph, V, F, failed);
  return findOrCreateValueNode(graph, V, F);
}

static GuardedValueFlowNode *createConcatAggregate(GuardedValueFlowGraph &graph,
                                                   Value *aggregate,
                                                   Function &F, bool &failed) {
  auto *result = findOrCreateValueNode(graph, aggregate, F);
  if (!result)
    return nullptr;
  if (!result->children().empty())
    return result;

  auto *opcode = createOpcodeNode(
      graph, GuardedValueFlowOpcodeNode::OpcodeKind::Concat,
      aggregate->getType(), getValueBlock(aggregate, F),
      GuardedValueFlowNode::Kind::SimpleOpcode, "const.concat");

  if (auto *CA = dyn_cast<ConstantArray>(aggregate)) {
    for (Value *operand : CA->operands())
      opcode->addChild(
          getOrCreateOperandRepresentation(graph, operand, F, failed));
  } else if (auto *CV = dyn_cast<ConstantVector>(aggregate)) {
    for (Value *operand : CV->operands())
      opcode->addChild(
          getOrCreateOperandRepresentation(graph, operand, F, failed));
  } else if (auto *CS = dyn_cast<ConstantStruct>(aggregate)) {
    for (Value *operand : CS->operands())
      opcode->addChild(
          getOrCreateOperandRepresentation(graph, operand, F, failed));
  } else if (auto *CDS = dyn_cast<ConstantDataSequential>(aggregate)) {
    for (unsigned idx = 0; idx < CDS->getNumElements(); ++idx)
      opcode->addChild(getOrCreateOperandRepresentation(
          graph, CDS->getElementAsConstant(idx), F, failed));
  } else if (auto *C = dyn_cast<Constant>(aggregate)) {
    for (Value *operand : C->operands())
      opcode->addChild(
          getOrCreateOperandRepresentation(graph, operand, F, failed));
  }

  result->addChild(opcode);
  return result;
}

template <typename GEPValueT>
static GuardedValueFlowNode *
modelGEPOperator(GEPValueT *GEP, BasicBlock *block,
                 GuardedValueFlowGraph &graph, Function &F, bool &failed,
                 GuardedValueFlowGEPReferenceSite *gep_site = nullptr);

static GuardedValueFlowNode *modelConstantExpr(ConstantExpr *CE,
                                               GuardedValueFlowGraph &graph,
                                               Function &F, bool &failed) {
  auto *result = findOrCreateValueNode(graph, CE, F);
  if (!result)
    return nullptr;
  if (!result->children().empty())
    return result;

  auto *block = getValueBlock(CE, F);
  auto addBinaryExpr = [&](unsigned opcode) {
    auto *node =
        createOpcodeNode(graph, translateOpcode(opcode), CE->getType(), block,
                         GuardedValueFlowNode::Kind::SimpleOpcode,
                         Instruction::getOpcodeName(opcode));
    node->addChild(
        getOrCreateOperandRepresentation(graph, CE->getOperand(0), F, failed));
    node->addChild(
        getOrCreateOperandRepresentation(graph, CE->getOperand(1), F, failed));
    result->addChild(node);
  };

  switch (CE->getOpcode()) {
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::BitCast:
  case Instruction::AddrSpaceCast: {
    auto *opcode =
        createOpcodeNode(graph, translateOpcode(CE->getOpcode()), CE->getType(),
                         block, GuardedValueFlowNode::Kind::CastOpcode,
                         Instruction::getOpcodeName(CE->getOpcode()));
    opcode->addChild(
        getOrCreateOperandRepresentation(graph, CE->getOperand(0), F, failed));
    const DataLayout &DL = F.getParent()->getDataLayout();
    Type *src_ty = CE->getOperand(0)->getType();
    Type *dst_ty = CE->getType();
    if (src_ty->isSized() && dst_ty->isSized())
      opcode->setCastWidths(DL.getTypeSizeInBits(src_ty),
                            DL.getTypeSizeInBits(dst_ty));
    result->addChild(opcode);
    break;
  }
  case Instruction::Select: {
    auto *opcode = createOpcodeNode(
        graph, GuardedValueFlowOpcodeNode::OpcodeKind::Select, CE->getType(),
        block, GuardedValueFlowNode::Kind::SimpleOpcode, "select");
    opcode->addChild(
        getOrCreateOperandRepresentation(graph, CE->getOperand(0), F, failed));
    opcode->addChild(
        getOrCreateOperandRepresentation(graph, CE->getOperand(1), F, failed));
    opcode->addChild(
        getOrCreateOperandRepresentation(graph, CE->getOperand(2), F, failed));
    result->addChild(opcode);
    break;
  }
  case Instruction::InsertElement:
  case Instruction::ExtractElement: {
    auto *opcode =
        createOpcodeNode(graph, translateOpcode(CE->getOpcode()), CE->getType(),
                         block, GuardedValueFlowNode::Kind::SimpleOpcode,
                         Instruction::getOpcodeName(CE->getOpcode()));
    for (Value *operand : CE->operands())
      opcode->addChild(
          getOrCreateOperandRepresentation(graph, operand, F, failed));
    result->addChild(opcode);
    break;
  }
  case Instruction::GetElementPtr: {
    if (auto *gep_node = modelGEPOperator(cast<GEPOperator>(CE), block, graph,
                                          F, failed, nullptr))
      result->addChild(gep_node);
    break;
  }
  case Instruction::ICmp:
  case Instruction::FCmp: {
    auto *opcode =
        createOpcodeNode(graph, translateOpcode(CE->getOpcode()), CE->getType(),
                         block, GuardedValueFlowNode::Kind::SimpleOpcode,
                         Instruction::getOpcodeName(CE->getOpcode()));
    opcode->setCmpPredicate(CE->getPredicate());
    opcode->addChild(
        getOrCreateOperandRepresentation(graph, CE->getOperand(0), F, failed));
    opcode->addChild(
        getOrCreateOperandRepresentation(graph, CE->getOperand(1), F, failed));
    result->addChild(opcode);
    break;
  }
  case Instruction::InsertValue:
  case Instruction::ExtractValue:
  case Instruction::ShuffleVector:
    // The previous implementation tolerated these constant-expression forms by
    // leaving the result
    // partially modeled instead of dropping the whole graph.
    break;
  case Instruction::URem:
  case Instruction::FRem:
  case Instruction::SRem:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::FAdd:
  case Instruction::FSub:
  case Instruction::Add:
  case Instruction::Sub:
    addBinaryExpr(CE->getOpcode());
    break;
  default:
    failed = true;
    recordBuilderDiagnostic(
        graph, GuardedValueFlowGraph::Diagnostic::Severity::Warning,
        Twine("Unsupported constant expression in function ") + F.getName() +
            ": " + Twine(CE->getOpcodeName()),
        dyn_cast<Instruction>(CE), getValueBlock(CE, F));
    break;
  }

  return result;
}

static GuardedValueFlowNode *
createSwitchCaseCompare(GuardedValueFlowGraph &graph, SwitchInst *SI,
                        ConstantInt *case_value, Function &F, bool &failed) {
  auto *switch_value =
      getOrCreateOperandRepresentation(graph, SI->getCondition(), F, failed);
  auto *case_node =
      getOrCreateOperandRepresentation(graph, case_value, F, failed);
  auto *opcode = createOpcodeNode(
      graph, GuardedValueFlowOpcodeNode::OpcodeKind::ICmp,
      Type::getInt1Ty(SI->getContext()), SI->getParent(),
      GuardedValueFlowNode::Kind::SimpleOpcode, "switch.case.eq");
  opcode->setCmpPredicate(CmpInst::ICMP_EQ);
  opcode->addChild(switch_value);
  opcode->addChild(case_node);

  auto *result = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand,
      Type::getInt1Ty(SI->getContext()), &graph, SI->getParent(), nullptr, SI);
  result->setDescription("switch.case.guard");
  result->addChild(opcode);
  return result;
}

static GuardedValueFlowNode *
findOrCreateSwitchCasePredicate(GuardedValueFlowGraph &graph, SwitchInst *SI,
                                BasicBlock *successor, Function &F,
                                bool &failed) {
  if (!SI || !successor)
    return nullptr;
  if (auto *existing = graph.findSyntheticGuardNode(SI, successor))
    return existing;

  SmallVector<ConstantInt *, 4> matching_cases;
  for (auto case_it = SI->case_begin(); case_it != SI->case_end(); ++case_it) {
    if (case_it->getCaseSuccessor() == successor)
      matching_cases.push_back(case_it->getCaseValue());
  }

  GuardedValueFlowNode *predicate = nullptr;
  if (!matching_cases.empty()) {
    for (ConstantInt *case_value : matching_cases) {
      auto *case_guard =
          createSwitchCaseCompare(graph, SI, case_value, F, failed);
      if (!predicate) {
        predicate = case_guard;
        continue;
      }

      auto *or_opcode = createOpcodeNode(
          graph, GuardedValueFlowOpcodeNode::OpcodeKind::Or,
          Type::getInt1Ty(SI->getContext()), SI->getParent(),
          GuardedValueFlowNode::Kind::SimpleOpcode, "switch.case.or");
      or_opcode->addChild(predicate);
      or_opcode->addChild(case_guard);
      auto *or_value = graph.createNode<GuardedValueFlowNode>(
          GuardedValueFlowNode::Kind::SimpleOperand,
          Type::getInt1Ty(SI->getContext()), &graph, SI->getParent(), nullptr,
          SI);
      or_value->setDescription("switch.case.guard");
      or_value->addChild(or_opcode);
      predicate = or_value;
    }
  } else if (SI->getDefaultDest() == successor) {
    SmallVector<GuardedValueFlowNode *, 4> case_guards;
    for (auto case_it = SI->case_begin(); case_it != SI->case_end();
         ++case_it) {
      case_guards.push_back(createSwitchCaseCompare(
          graph, SI, case_it->getCaseValue(), F, failed));
    }

    if (case_guards.empty()) {
      predicate = getOrCreateOperandRepresentation(
          graph, ConstantInt::getTrue(SI->getContext()), F, failed);
    } else {
      GuardedValueFlowNode *covered = case_guards.front();
      for (size_t idx = 1; idx < case_guards.size(); ++idx) {
        auto *or_opcode = createOpcodeNode(
            graph, GuardedValueFlowOpcodeNode::OpcodeKind::Or,
            Type::getInt1Ty(SI->getContext()), SI->getParent(),
            GuardedValueFlowNode::Kind::SimpleOpcode, "switch.default.covered");
        or_opcode->addChild(covered);
        or_opcode->addChild(case_guards[idx]);
        auto *or_value = graph.createNode<GuardedValueFlowNode>(
            GuardedValueFlowNode::Kind::SimpleOperand,
            Type::getInt1Ty(SI->getContext()), &graph, SI->getParent(), nullptr,
            SI);
        or_value->setDescription("switch.default.covered");
        or_value->addChild(or_opcode);
        covered = or_value;
      }

      auto *not_opcode = createOpcodeNode(
          graph, GuardedValueFlowOpcodeNode::OpcodeKind::Xor,
          Type::getInt1Ty(SI->getContext()), SI->getParent(),
          GuardedValueFlowNode::Kind::SimpleOpcode, "switch.default.not");
      not_opcode->setIntConstant(-1);
      not_opcode->addChild(covered);
      predicate = graph.createNode<GuardedValueFlowNode>(
          GuardedValueFlowNode::Kind::SimpleOperand,
          Type::getInt1Ty(SI->getContext()), &graph, SI->getParent(), nullptr,
          SI);
      predicate->setDescription("switch.default.guard");
      predicate->addChild(not_opcode);
    }
  }

  if (predicate)
    graph.mapSyntheticGuardNode(SI, successor, predicate);
  return predicate;
}

template <typename GEPValueT>
static GuardedValueFlowNode *
modelGEPOperator(GEPValueT *GEP, BasicBlock *block,
                 GuardedValueFlowGraph &graph, Function &F, bool &failed,
                 GuardedValueFlowGEPReferenceSite *gep_site) {
  if (GEP->getType()->isVectorTy()) {
    failed = true;
    recordBuilderDiagnostic(
        graph, GuardedValueFlowGraph::Diagnostic::Severity::Warning,
        Twine("Unsupported vector GEP in function ") + F.getName(),
        dyn_cast<Instruction>(GEP), block);
    return nullptr;
  }

  const DataLayout &DL = F.getParent()->getDataLayout();
  auto *base_node = getOrCreateOperandRepresentation(
      graph, GEP->getPointerOperand(), F, failed);
  auto *orig_base = base_node;
  int64_t accumulated_offset = 0;
  Type *current_type = GEP->getSourceElementType();

  // Lower address arithmetic into explicit add/mul/cast nodes while preserving
  // a stable site view of the original GEP operands.
  for (unsigned idx = 0; idx < GEP->getNumIndices(); ++idx) {
    Value *index_value = GEP->getOperand(idx + 1);
    Type *agg_or_ptr_ty = current_type;
    Type *element_ty = nullptr;
    if (auto *struct_ty = dyn_cast_or_null<StructType>(agg_or_ptr_ty)) {
      if (auto *CI = dyn_cast<ConstantInt>(index_value)) {
        int64_t field_idx = CI->getSExtValue();
        if (field_idx >= 0 &&
            static_cast<unsigned>(field_idx) < struct_ty->getNumElements())
          element_ty =
              struct_ty->getElementType(static_cast<unsigned>(field_idx));
      }
    } else if (auto *array_ty = dyn_cast_or_null<ArrayType>(agg_or_ptr_ty)) {
      element_ty = array_ty->getElementType();
    } else if (auto *vector_ty = dyn_cast_or_null<VectorType>(agg_or_ptr_ty)) {
      element_ty = vector_ty->getElementType();
    } else if (auto *pointer_ty =
                   dyn_cast_or_null<PointerType>(agg_or_ptr_ty)) {
      element_ty = pointer_ty->getPointerElementType();
    }
    if (!element_ty)
      element_ty = agg_or_ptr_ty;
    if (!element_ty->isSized())
      continue;

    if (auto *CI = dyn_cast<ConstantInt>(index_value)) {
      int64_t const_index = CI->getSExtValue();
      if (agg_or_ptr_ty->isStructTy()) {
        auto *struct_ty = cast<StructType>(agg_or_ptr_ty);
        if (const_index >= 0 &&
            static_cast<unsigned>(const_index) < struct_ty->getNumElements()) {
          for (int64_t field_idx = 0; field_idx < const_index; ++field_idx) {
            Type *field_ty =
                struct_ty->getElementType(static_cast<unsigned>(field_idx));
            if (field_ty->isSized())
              accumulated_offset +=
                  static_cast<int64_t>(DL.getTypeSizeInBits(field_ty));
          }
        }
      } else {
        accumulated_offset +=
            const_index *
            static_cast<int64_t>(DL.getTypeSizeInBits(element_ty));
      }
      current_type = element_ty;
      continue;
    }

    if (accumulated_offset != 0) {
      auto *add_const = createBinaryWithIntConst(
          graph, GuardedValueFlowOpcodeNode::OpcodeKind::Add, GEP->getType(),
          block, base_node, accumulated_offset);
      auto *offset_base = graph.createNode<GuardedValueFlowNode>(
          GuardedValueFlowNode::Kind::SimpleOperand, GEP->getType(), &graph,
          block, nullptr, dyn_cast<Instruction>(GEP));
      offset_base->setDescription("gep.offset.base");
      offset_base->addChild(add_const);
      base_node = offset_base;
      accumulated_offset = 0;
    }

    auto *non_const_index =
        getOrCreateOperandRepresentation(graph, index_value, F, failed);
    // Keep the GEP site aligned with the original IR operands. The lowering
    // below may introduce cast/add temporaries, but those should not appear as
    // structural site offsets.

    Type *base_ptr_ty = GEP->getPointerOperandType();
    if (base_ptr_ty->isPointerTy() && index_value->getType()->isSized() &&
        base_ptr_ty->isSized() &&
        DL.getTypeSizeInBits(base_ptr_ty) !=
            DL.getTypeSizeInBits(index_value->getType())) {
      auto opcode_kind = DL.getTypeSizeInBits(base_ptr_ty) <
                                 DL.getTypeSizeInBits(index_value->getType())
                             ? GuardedValueFlowOpcodeNode::OpcodeKind::Trunc
                             : GuardedValueFlowOpcodeNode::OpcodeKind::SExt;
      auto *cast_node = createOpcodeNode(graph, opcode_kind, base_ptr_ty, block,
                                         GuardedValueFlowNode::Kind::CastOpcode,
                                         "gep.index.cast");
      cast_node->addChild(non_const_index);
      cast_node->setCastWidths(DL.getTypeSizeInBits(index_value->getType()),
                               DL.getTypeSizeInBits(base_ptr_ty));
      auto *cast_value = graph.createNode<GuardedValueFlowNode>(
          GuardedValueFlowNode::Kind::SimpleOperand, base_ptr_ty, &graph, block,
          nullptr, dyn_cast<Instruction>(GEP));
      cast_value->setDescription("gep.index.cast.value");
      cast_value->addChild(cast_node);
      non_const_index = cast_value;
    }

    auto *mul = createBinaryWithIntConst(
        graph, GuardedValueFlowOpcodeNode::OpcodeKind::Mul, GEP->getType(),
        block, non_const_index,
        static_cast<int64_t>(DL.getTypeSizeInBits(element_ty)));
    auto *offset_node = graph.createNode<GuardedValueFlowNode>(
        GuardedValueFlowNode::Kind::SimpleOperand, GEP->getType(), &graph,
        block, nullptr, dyn_cast<Instruction>(GEP));
    offset_node->setDescription("gep.dynamic.offset");
    offset_node->addChild(mul);

    auto *add = createOpcodeNode(
        graph, GuardedValueFlowOpcodeNode::OpcodeKind::Add, GEP->getType(),
        block, GuardedValueFlowNode::Kind::SimpleOpcode, "gep.add");
    add->addChild(base_node);
    add->addChild(offset_node);
    auto *next_base = graph.createNode<GuardedValueFlowNode>(
        GuardedValueFlowNode::Kind::SimpleOperand, GEP->getType(), &graph,
        block, nullptr, dyn_cast<Instruction>(GEP));
    next_base->setDescription("gep.base.next");
    next_base->addChild(add);
    base_node = next_base;
    current_type = element_ty;
  }

  if (accumulated_offset != 0) {
    auto *add_const = createBinaryWithIntConst(
        graph, GuardedValueFlowOpcodeNode::OpcodeKind::Add, GEP->getType(),
        block, base_node, accumulated_offset);
    auto *offset_base = graph.createNode<GuardedValueFlowNode>(
        GuardedValueFlowNode::Kind::SimpleOperand, GEP->getType(), &graph,
        block, nullptr, dyn_cast<Instruction>(GEP));
    offset_base->setDescription("gep.offset.final");
    offset_base->addChild(add_const);
    base_node = offset_base;
  }

  auto *gep_opcode = createOpcodeNode(
      graph, GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr,
      GEP->getType(), block, GuardedValueFlowNode::Kind::SimpleOpcode, "gep");
  gep_opcode->addChild(orig_base);
  gep_opcode->addChild(base_node);

  if (gep_site) {
    gep_site->setPointerOperand(orig_base);
    orig_base->addUseSite(gep_site);
  }

  return gep_opcode;
}

static bool computeGuardForControlledBlock(
    gsa::ControlDependenceAnalysis &cda, GuardedValueFlowGraph &graph,
    Function &F, Instruction *term, BasicBlock *target, ConditionRef &cond,
    GuardedValueFlowNode *&cond_node, bool &sense, BasicBlock *&guard_successor,
    bool &failed) {
  cond = ConditionRef::none();
  cond_node = nullptr;
  sense = true;
  guard_successor = nullptr;
  if (!term || !target)
    return false;

  if (auto *br = dyn_cast<BranchInst>(term)) {
    if (!br->isConditional())
      return false;
    BasicBlock *succ0 = br->getSuccessor(0);
    BasicBlock *succ1 = br->getSuccessor(1);
    bool succ0_reaches = succ0 == target || cda.isReachable(succ0, target);
    bool succ1_reaches = succ1 == target || cda.isReachable(succ1, target);
    if (!succ0_reaches && !succ1_reaches)
      return false;
    if (succ0_reaches == succ1_reaches)
      return false;
    sense = succ0_reaches;
    guard_successor = sense ? succ0 : succ1;
    cond_node =
        getOrCreateOperandRepresentation(graph, br->getCondition(), F, failed);
    cond = ConditionRef::fromGuard(
        sense ? gsa::GuardKind::BranchTrue : gsa::GuardKind::BranchFalse,
        br->getParent(), guard_successor, br->getCondition());
    return true;
  }

  if (auto *si = dyn_cast<SwitchInst>(term)) {
    SmallVector<BasicBlock *, 4> reaching_successors;
    for (unsigned idx = 0; idx < si->getNumSuccessors(); ++idx) {
      BasicBlock *succ = si->getSuccessor(idx);
      if (succ == target || cda.isReachable(succ, target))
        reaching_successors.push_back(succ);
    }
    if (reaching_successors.size() != 1)
      return false;

    guard_successor = reaching_successors.front();
    cond_node =
        findOrCreateSwitchCasePredicate(graph, si, guard_successor, F, failed);
    if (!cond_node)
      return false;

    ConstantInt *case_value = nullptr;
    for (auto case_it = si->case_begin(); case_it != si->case_end();
         ++case_it) {
      if (case_it->getCaseSuccessor() == guard_successor) {
        case_value = case_it->getCaseValue();
        break;
      }
    }
    cond = ConditionRef::fromGuard(
        case_value ? gsa::GuardKind::SwitchCase : gsa::GuardKind::SwitchDefault,
        si->getParent(), guard_successor, si->getCondition(), case_value);
    return true;
  }

  return false;
}

static void collectBlockConditions(GuardedValueFlowGraph &graph, Function &F,
                                   gsa::ControlDependenceAnalysis &cda,
                                   bool &failed) {
  SmallVector<BasicBlock *, 16> blocks;
  for (BasicBlock &BB : F) {
    if (cda.isTracked(BB))
      blocks.push_back(&BB);
  }

  std::sort(blocks.begin(), blocks.end(),
            [&](BasicBlock *lhs, BasicBlock *rhs) {
              return cda.getBBTopoIdx(lhs) < cda.getBBTopoIdx(rhs);
            });

  for (BasicBlock *BB : blocks) {
    for (BasicBlock *dep : cda.getCDBlocks(BB)) {
      ConditionRef cond;
      GuardedValueFlowNode *cond_node = nullptr;
      bool sense = true;
      BasicBlock *guard_successor = nullptr;
      if (!computeGuardForControlledBlock(
              cda, graph, F, dep ? dep->getTerminator() : nullptr, BB, cond,
              cond_node, sense, guard_successor, failed)) {
        continue;
      }
      graph.addBlockCondition(BB,
                              {cond_node, dep, guard_successor, cond, sense});
    }
  }
}

static void buildRegions(GuardedValueFlowGraph &graph, Function &F,
                         gsa::ControlDependenceAnalysis &cda, bool &failed) {
  collectBlockConditions(graph, F, cda, failed);

  SmallVector<BasicBlock *, 16> blocks;
  for (BasicBlock &BB : F) {
    if (cda.isTracked(BB))
      blocks.push_back(&BB);
  }
  std::sort(blocks.begin(), blocks.end(),
            [&](BasicBlock *lhs, BasicBlock *rhs) {
              return cda.getBBTopoIdx(lhs) < cda.getBBTopoIdx(rhs);
            });

  for (BasicBlock *BB : blocks) {
    GuardedValueFlowRegionNode *region = nullptr;
    // A block region is the disjunction of each controlling branch path that
    // can reach the block. Each branch path is the local branch choice AND the
    // region of the controlling block.
    for (const auto &block_cond : graph.getBlockConditions(BB)) {
      auto *unit = graph.findOrCreateUnitRegion(block_cond.condition_node,
                                                block_cond.sense, BB,
                                                block_cond.condition);
      auto *parent = graph.findRegion(block_cond.control_block);
      if (!parent)
        parent = graph.getAlwaysTrueRegion();
      auto *branch_region = graph.findOrCreateAndRegion(unit, parent, BB);
      region = region ? graph.findOrCreateOrRegion(region, branch_region, BB)
                      : branch_region;
    }

    if (!region)
      region = graph.getAlwaysTrueRegion();
    graph.mapRegion(BB, region);
  }
}

static bool modelIntrinsicCall(CallBase &call, GuardedValueFlowGraph &graph,
                               Function &F, bool &failed) {
  Function *callee = call.getCalledFunction();
  if (!callee || !callee->isIntrinsic())
    return false;

  switch (callee->getIntrinsicID()) {
  case Intrinsic::memset:
  case Intrinsic::memmove:
  case Intrinsic::memcpy: {
    auto *site = graph.findCallSite(&call);
    if (!site) {
      site = graph.createSite<GuardedValueFlowCallSite>(&graph, &call);
      graph.mapCallSite(&call, site);
    }
    site->addCallee(callee);
    for (Value *arg : call.args()) {
      auto *arg_node = getOrCreateOperandRepresentation(graph, arg, F, failed);
      arg_node->addUseSite(site);
      site->addCommonInput(arg_node);
    }
    return true;
  }
  case Intrinsic::expect: {
    auto *result = findOrCreateValueNode(graph, &call, F);
    result->addChild(getOrCreateOperandRepresentation(
        graph, call.getArgOperand(0), F, failed));
    return true;
  }
  case Intrinsic::bswap: {
    auto *site = graph.findCallSite(&call);
    if (!site) {
      site = graph.createSite<GuardedValueFlowCallSite>(&graph, &call);
      graph.mapCallSite(&call, site);
    }
    site->addCallee(callee);
    for (Value *arg : call.args()) {
      auto *arg_node = getOrCreateOperandRepresentation(graph, arg, F, failed);
      arg_node->addUseSite(site);
      site->addCommonInput(arg_node);
    }
    if (!call.getType()->isVoidTy())
      (void)findOrCreateCallOutputNode(graph, call, F);
    return true;
  }
  default:
    if (!call.getType()->isVoidTy())
      (void)findOrCreateValueNode(graph, &call, F);
    return true;
  }
}

static GuardedValueFlowNode *
findOrCreateUnknownInstructionNode(GuardedValueFlowGraph &graph, Instruction &I,
                                   StringRef description) {
  if (I.getType()->isVoidTy())
    return nullptr;
  if (auto *existing = graph.findNode(&I))
    return existing;

  auto *node = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::Unknown, I.getType(), &graph, I.getParent(),
      &I, &I);
  node->setDescription(description.str());
  graph.mapValueNode(&I, node);
  return node;
}

static void recordUnsupportedInstruction(GuardedValueFlowGraph &graph,
                                         Function &F, Instruction &I,
                                         StringRef detail) {
  recordBuilderDiagnostic(graph,
                          GuardedValueFlowGraph::Diagnostic::Severity::Warning,
                          Twine("Partially modeled instruction in function ") +
                              F.getName() + ": " + detail,
                          &I, I.getParent());
}

static bool buildInstruction(GuardedValueFlowGraph &graph, Instruction &I,
                             Function &F, bool &failed) {
  auto *block = I.getParent();

  switch (I.getOpcode()) {
  case Instruction::Br: {
    auto *br = dyn_cast<BranchInst>(&I);
    if (!br) {
      failed = true;
      recordUnsupportedInstruction(graph, F, I, "invalid branch");
      return true;
    }
    if (br->isConditional()) {
      auto *cond_node = getOrCreateOperandRepresentation(
          graph, br->getCondition(), F, failed);
      (void)graph.findOrCreateUnitRegion(
          cond_node, true, br->getSuccessor(0),
          ConditionRef::fromGuard(gsa::GuardKind::BranchTrue, block,
                                  br->getSuccessor(0), br->getCondition()));
      (void)graph.findOrCreateUnitRegion(
          cond_node, false, br->getSuccessor(1),
          ConditionRef::fromGuard(gsa::GuardKind::BranchFalse, block,
                                  br->getSuccessor(1), br->getCondition()));
    }
    return true;
  }
  case Instruction::Ret: {
    auto *site = graph.findReturnSite(&I);
    if (!site) {
      site = graph.createSite<GuardedValueFlowReturnSite>(&graph, &I);
      graph.mapReturnSite(&I, site);
    }
    if (I.getNumOperands() == 0)
      return true;
    GuardedValueFlowReturnNode *common_return = nullptr;
    for (const auto &node_ptr : graph.nodes()) {
      if (node_ptr->getKind() == GuardedValueFlowNode::Kind::CommonReturn) {
        common_return = dyn_cast<GuardedValueFlowReturnNode>(node_ptr.get());
        break;
      }
    }
    if (!common_return)
      return true;
    auto *value_node =
        getOrCreateOperandRepresentation(graph, I.getOperand(0), F, failed);
    common_return->addChild(value_node);
    common_return->addReturnValueSitePair(value_node, site);
    return true;
  }
  case Instruction::Unreachable:
    return true;
  case Instruction::Switch: {
    auto *si = dyn_cast<SwitchInst>(&I);
    if (!si) {
      failed = true;
      recordUnsupportedInstruction(graph, F, I, "invalid switch");
      return true;
    }
    for (unsigned idx = 0; idx < si->getNumSuccessors(); ++idx)
      (void)findOrCreateSwitchCasePredicate(graph, si, si->getSuccessor(idx), F,
                                            failed);
    return true;
  }
  case Instruction::Invoke: {
    auto *invoke = dyn_cast<InvokeInst>(&I);
    if (!invoke) {
      failed = true;
      recordUnsupportedInstruction(graph, F, I, "invalid invoke");
      return true;
    }
    auto *site = graph.findCallSite(invoke);
    if (!site) {
      site = graph.createSite<GuardedValueFlowCallSite>(&graph, invoke);
      graph.mapCallSite(invoke, site);
    }
    if (Function *callee = invoke->getCalledFunction())
      site->addCallee(callee);
    if (!invoke->getType()->isVoidTy())
      (void)findOrCreateCallOutputNode(graph, *invoke, F);
    for (Value *arg : invoke->args())
      site->addCommonInput(
          getOrCreateOperandRepresentation(graph, arg, F, failed));
    recordUnsupportedInstruction(
        graph, F, I,
        "invoke control effects are degraded to a callsite-only model");
    return true;
  }
  case Instruction::LandingPad:
  case Instruction::Resume:
  case Instruction::IndirectBr:
  case Instruction::ExtractValue:
  case Instruction::InsertValue:
  case Instruction::ShuffleVector:
  case Instruction::AtomicRMW:
  case Instruction::AtomicCmpXchg:
  case Instruction::Fence: {
    failed = true;
    (void)findOrCreateUnknownInstructionNode(
        graph, I, (Twine("unsupported.") + I.getOpcodeName()).str());
    (void)graph.createSite<GuardedValueFlowSite>(
        GuardedValueFlowSite::Kind::Unknown, &graph, &I);
    recordUnsupportedInstruction(
        graph, F, I,
        (Twine("unsupported opcode lowered to unknown: ") + I.getOpcodeName())
            .str());
    return true;
  }
  case Instruction::FNeg: {
    auto *result = findOrCreateValueNode(graph, &I, F);
    auto *operand =
        getOrCreateOperandRepresentation(graph, I.getOperand(0), F, failed);
    auto *zero = getOrCreateOperandRepresentation(
        graph, ConstantFP::get(I.getType(), 0.0), F, failed);
    auto *opcode = createOpcodeNode(
        graph, GuardedValueFlowOpcodeNode::OpcodeKind::FSub, I.getType(), block,
        GuardedValueFlowNode::Kind::SimpleOpcode, "fneg");
    opcode->addChild(zero);
    opcode->addChild(operand);
    result->addChild(opcode);
    return true;
  }
  case Instruction::URem:
  case Instruction::FRem:
  case Instruction::SRem:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::FAdd:
  case Instruction::FSub:
  case Instruction::Add:
  case Instruction::Sub: {
    auto *result = findOrCreateValueNode(graph, &I, F);
    auto *lhs =
        getOrCreateOperandRepresentation(graph, I.getOperand(0), F, failed);
    auto *rhs =
        getOrCreateOperandRepresentation(graph, I.getOperand(1), F, failed);
    auto *opcode =
        createOpcodeNode(graph, translateOpcode(I.getOpcode()), I.getType(),
                         block, GuardedValueFlowNode::Kind::SimpleOpcode,
                         Instruction::getOpcodeName(I.getOpcode()));
    opcode->addChild(lhs);
    opcode->addChild(rhs);
    result->addChild(opcode);

    if (I.getOpcode() == Instruction::URem ||
        I.getOpcode() == Instruction::FRem ||
        I.getOpcode() == Instruction::SRem ||
        I.getOpcode() == Instruction::UDiv ||
        I.getOpcode() == Instruction::SDiv ||
        I.getOpcode() == Instruction::FDiv) {
      auto *site = graph.createSite<GuardedValueFlowDivSite>(&graph, &I);
      site->setLhsOperand(lhs);
      site->setRhsOperand(rhs);
      rhs->addUseSite(site);
    }
    return true;
  }
  case Instruction::AddrSpaceCast:
  case Instruction::IntToPtr:
  case Instruction::PtrToInt:
  case Instruction::BitCast:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::Trunc:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::SIToFP:
  case Instruction::FPToSI:
  case Instruction::UIToFP:
  case Instruction::FPToUI: {
    auto *result = findOrCreateValueNode(graph, &I, F);
    auto *operand =
        getOrCreateOperandRepresentation(graph, I.getOperand(0), F, failed);
    auto *opcode =
        createOpcodeNode(graph, translateOpcode(I.getOpcode()), I.getType(),
                         block, GuardedValueFlowNode::Kind::CastOpcode,
                         Instruction::getOpcodeName(I.getOpcode()));
    opcode->addChild(operand);
    const DataLayout &DL = F.getParent()->getDataLayout();
    Type *src_ty = I.getOperand(0)->getType();
    Type *dst_ty = I.getType();
    if (src_ty->isSized() && dst_ty->isSized())
      opcode->setCastWidths(DL.getTypeSizeInBits(src_ty),
                            DL.getTypeSizeInBits(dst_ty));
    result->addChild(opcode);
    return true;
  }
  case Instruction::ExtractElement:
  case Instruction::InsertElement: {
    auto *result = findOrCreateValueNode(graph, &I, F);
    auto *opcode =
        createOpcodeNode(graph, translateOpcode(I.getOpcode()), I.getType(),
                         block, GuardedValueFlowNode::Kind::SimpleOpcode,
                         Instruction::getOpcodeName(I.getOpcode()));
    for (Value *operand : I.operands())
      opcode->addChild(
          getOrCreateOperandRepresentation(graph, operand, F, failed));
    result->addChild(opcode);
    return true;
  }
  case Instruction::Load: {
    auto *result = findOrCreateValueNode(graph, &I, F);
    // The builder only creates a placeholder memory node here. The adapter
    // later replaces its incoming producers using LotusAA memory facts.
    auto *mem_node = graph.createNode<GuardedValueFlowNode>(
        GuardedValueFlowNode::Kind::LoadMemory, I.getType(), &graph, block,
        nullptr, &I);
    mem_node->setDescription("load.mem");
    graph.mapLoadMemoryNode(&I, mem_node);
    result->addChild(mem_node);

    auto *site = graph.createSite<GuardedValueFlowDereferenceSite>(&graph, &I);
    auto *ptr_node =
        getOrCreateOperandRepresentation(graph, I.getOperand(0), F, failed);
    ptr_node->addUseSite(site);
    site->setPointerOperand(ptr_node);
    return true;
  }
  case Instruction::Store: {
    auto *stored =
        getOrCreateOperandRepresentation(graph, I.getOperand(0), F, failed);
    // Store-memory nodes are keyed by the stored value and producing
    // instruction. Later adaptation may repopulate the producer chain, but the
    // structural location of the store remains anchored here.
    auto *mem_node = graph.findOrCreateStoreMemoryNode(
        I.getOperand(0), &I, I.getOperand(0)->getType(), block);
    mem_node->addChild(stored);
    auto *site = graph.createSite<GuardedValueFlowDereferenceSite>(&graph, &I);
    auto *ptr_node =
        getOrCreateOperandRepresentation(graph, I.getOperand(1), F, failed);
    ptr_node->addUseSite(site);
    stored->addUseSite(site);
    site->setPointerOperand(ptr_node);
    site->setValueOperand(stored);
    return true;
  }
  case Instruction::Alloca: {
    auto *node = findOrCreateValueNode(graph, &I, F);
    auto *site = graph.createSite<GuardedValueFlowAllocSite>(&graph, &I);
    node->addUseSite(site);
    return true;
  }
  case Instruction::GetElementPtr: {
    auto *result = findOrCreateValueNode(graph, &I, F);
    auto *site = graph.createSite<GuardedValueFlowGEPReferenceSite>(&graph, &I);
    auto *gep_inst = dyn_cast<GetElementPtrInst>(&I);
    if (!gep_inst) {
      failed = true;
      recordUnsupportedInstruction(graph, F, I, "invalid GEP");
      return true;
    }
    auto *opcode = modelGEPOperator(gep_inst, block, graph, F, failed, site);
    if (!opcode) {
      recordUnsupportedInstruction(
          graph, F, I, "GEP degraded to an unknown value representation");
      (void)findOrCreateUnknownInstructionNode(graph, I, "gep.unknown");
      return true;
    }
    result->addChild(opcode);
    result->addUseSite(site);
    site->setResultNode(result);
    for (Use &operand : I.operands()) {
      auto *operand_node =
          getOrCreateOperandRepresentation(graph, operand.get(), F, failed);
      operand_node->addUseSite(site);
      if (&operand == &I.getOperandUse(0))
        site->setPointerOperand(operand_node);
      else
        site->addOffsetOperand(operand_node);
    }
    return true;
  }
  case Instruction::ICmp:
  case Instruction::FCmp: {
    auto *lhs =
        getOrCreateOperandRepresentation(graph, I.getOperand(0), F, failed);
    auto *rhs =
        getOrCreateOperandRepresentation(graph, I.getOperand(1), F, failed);
    auto *result = findOrCreateValueNode(graph, &I, F);
    auto *opcode =
        createOpcodeNode(graph, translateOpcode(I.getOpcode()), I.getType(),
                         block, GuardedValueFlowNode::Kind::SimpleOpcode,
                         Instruction::getOpcodeName(I.getOpcode()));
    auto *cmp = dyn_cast<CmpInst>(&I);
    if (!cmp) {
      failed = true;
      recordUnsupportedInstruction(graph, F, I, "invalid compare");
      return true;
    }
    opcode->setCmpPredicate(cmp->getPredicate());
    opcode->addChild(lhs);
    opcode->addChild(rhs);
    result->addChild(opcode);

    auto *site = graph.createSite<GuardedValueFlowCompareSite>(&graph, &I);
    site->setLhsOperand(lhs);
    site->setRhsOperand(rhs);
    lhs->addUseSite(site);
    rhs->addUseSite(site);
    return true;
  }
  case Instruction::PHI: {
    auto *phi_node =
        dyn_cast<GuardedValueFlowPhiNode>(findOrCreateValueNode(graph, &I, F));
    auto *phi = dyn_cast<PHINode>(&I);
    if (!phi_node || !phi) {
      failed = true;
      recordUnsupportedInstruction(graph, F, I, "invalid phi");
      return true;
    }
    for (unsigned idx = 0; idx < phi->getNumIncomingValues(); ++idx) {
      auto *incoming_value = getOrCreateOperandRepresentation(
          graph, phi->getIncomingValue(idx), F, failed);
      BasicBlock *incoming_bb = phi->getIncomingBlock(idx);
      ConditionRef cond = ConditionRef::none();
      bool sense = true;
      GuardedValueFlowNode *cond_node = nullptr;
      if (auto *br = dyn_cast<BranchInst>(incoming_bb->getTerminator())) {
        if (br->isConditional()) {
          bool succ0_reaches = br->getSuccessor(0) == block;
          bool succ1_reaches = br->getSuccessor(1) == block;
          if (succ0_reaches != succ1_reaches) {
            sense = succ0_reaches;
            BasicBlock *guard_successor =
                sense ? br->getSuccessor(0) : br->getSuccessor(1);
            cond_node = getOrCreateOperandRepresentation(
                graph, br->getCondition(), F, failed);
            cond = ConditionRef::fromGuard(sense ? gsa::GuardKind::BranchTrue
                                                 : gsa::GuardKind::BranchFalse,
                                           incoming_bb, guard_successor,
                                           br->getCondition());
          }
        }
      } else if (auto *si =
                     dyn_cast<SwitchInst>(incoming_bb->getTerminator())) {
        BasicBlock *guard_successor = nullptr;
        for (unsigned succ_idx = 0; succ_idx < si->getNumSuccessors();
             ++succ_idx) {
          BasicBlock *succ = si->getSuccessor(succ_idx);
          if (succ == block) {
            guard_successor = succ;
            break;
          }
        }
        if (guard_successor) {
          cond_node = findOrCreateSwitchCasePredicate(
              graph, si, guard_successor, F, failed);
          ConstantInt *case_value = nullptr;
          for (auto case_it = si->case_begin(); case_it != si->case_end();
               ++case_it) {
            if (case_it->getCaseSuccessor() == guard_successor) {
              case_value = case_it->getCaseValue();
              break;
            }
          }
          cond = ConditionRef::fromGuard(
              case_value ? gsa::GuardKind::SwitchCase
                         : gsa::GuardKind::SwitchDefault,
              incoming_bb, guard_successor, si->getCondition(), case_value);
        }
      }
      // PHI incoming metadata keeps the immediate edge-local guard in addition
      // to the enclosing block region, which is what downstream path-sensitive
      // users need for precise merge semantics.
      phi_node->addIncoming(incoming_value, incoming_bb, cond_node, sense,
                            cond);
    }
    return true;
  }
  case Instruction::Call: {
    auto *call = dyn_cast<CallBase>(&I);
    if (!call) {
      failed = true;
      recordUnsupportedInstruction(graph, F, I, "invalid call");
      return true;
    }
    if (modelIntrinsicCall(*call, graph, F, failed))
      return true;

    auto *site = graph.findCallSite(call);
    if (!site) {
      site = graph.createSite<GuardedValueFlowCallSite>(&graph, call);
      graph.mapCallSite(call, site);
    }

    if (Function *callee = call->getCalledFunction())
      site->addCallee(callee);

    if (!call->getType()->isVoidTy())
      (void)findOrCreateCallOutputNode(graph, *call, F);

    for (Value *arg : call->args())
      site->addCommonInput(
          getOrCreateOperandRepresentation(graph, arg, F, failed));

    return true;
  }
  case Instruction::Select: {
    auto *result = findOrCreateValueNode(graph, &I, F);
    auto *opcode = createOpcodeNode(
        graph, GuardedValueFlowOpcodeNode::OpcodeKind::Select, I.getType(),
        block, GuardedValueFlowNode::Kind::SimpleOpcode, "select");
    opcode->addChild(
        getOrCreateOperandRepresentation(graph, I.getOperand(0), F, failed));
    opcode->addChild(
        getOrCreateOperandRepresentation(graph, I.getOperand(1), F, failed));
    opcode->addChild(
        getOrCreateOperandRepresentation(graph, I.getOperand(2), F, failed));
    result->addChild(opcode);
    return true;
  }
  case Instruction::VAArg: {
    (void)findOrCreateValueNode(graph, &I, F);
    return true;
  }
  default:
    failed = true;
    (void)findOrCreateUnknownInstructionNode(
        graph, I, (Twine("unsupported.") + I.getOpcodeName()).str());
    recordUnsupportedInstruction(
        graph, F, I,
        (Twine("unsupported opcode lowered to unknown: ") + I.getOpcodeName())
            .str());
    return true;
  }
}

} // namespace

char GuardedValueFlowGraphBuilderPass::ID = 0;
static RegisterPass<GuardedValueFlowGraphBuilderPass>
    X("gvfg-builder", "GuardedValueFlowGraph builder", false, true);

GuardedValueFlowGraphBuilderPass::GuardedValueFlowGraphBuilderPass()
    : ModulePass(ID) {}

void GuardedValueFlowGraphBuilderPass::getAnalysisUsage(
    AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<gsa::ControlDependenceAnalysisPass>();
  AU.addRequired<gsa::GateAnalysisPass>();
}

bool GuardedValueFlowGraphBuilderPass::runOnModule(Module &M) {
  graphs_.clear();
  auto &cda_pass = getAnalysis<gsa::ControlDependenceAnalysisPass>();
  (void)getAnalysis<gsa::GateAnalysisPass>();

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (!cda_pass.hasAnalysisFor(F))
      continue;
    if (auto graph = buildGraph(F))
      graphs_[&F] = std::move(graph);
  }
  return false;
}

bool GuardedValueFlowGraphBuilderPass::hasGraphFor(const Function &F) const {
  return graphs_.find(&F) != graphs_.end();
}

GuardedValueFlowGraph &
GuardedValueFlowGraphBuilderPass::getGraph(const Function &F) {
  auto it = graphs_.find(&F);
  assert(it != graphs_.end() && "Requested missing GuardedValueFlowGraph");
  return *it->second;
}

void GuardedValueFlowGraphBuilderPass::invalidateGraph(const Function &F) {
  graphs_.erase(&F);
}

std::unique_ptr<GuardedValueFlowGraph>
GuardedValueFlowGraphBuilderPass::buildGraph(Function &F) {
  auto graph = std::make_unique<GuardedValueFlowGraph>(&F);
  auto &cda = getAnalysis<gsa::ControlDependenceAnalysisPass>()
                  .getControlDependenceAnalysis(F);

  unsigned common_arg_index = 0;
  for (Argument &arg : F.args()) {
    auto *arg_node = findOrCreateValueNode(*graph, &arg, F);
    if (arg_node &&
        arg_node->getKind() == GuardedValueFlowNode::Kind::CommonArgument)
      arg_node->setIndex(common_arg_index++);
  }

  if (!F.getReturnType()->isVoidTy()) {
    auto *ret_node = graph->createNode<GuardedValueFlowReturnNode>(
        GuardedValueFlowNode::Kind::CommonReturn, F.getReturnType(),
        graph.get(), F.empty() ? nullptr : &F.getEntryBlock());
    ret_node->setDescription("return.common");
  }

  // Regions are built before instructions so nodes created during instruction
  // modeling inherit their block-level path condition immediately.
  bool region_degraded = false;
  buildRegions(*graph, F, cda, region_degraded);
  graph->refreshNodeRegions();

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      bool instruction_degraded = false;
      (void)buildInstruction(*graph, I, F, instruction_degraded);
    }
  }

  graph->refreshNodeRegions();

  return graph;
}

ModulePass *lotus::gvfg::createGuardedValueFlowGraphBuilderPass() {
  return new GuardedValueFlowGraphBuilderPass();
}
