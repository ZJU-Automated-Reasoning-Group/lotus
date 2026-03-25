#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "IR/GVFG/LotusAdapter.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <gtest/gtest.h>
#include <algorithm>

using namespace llvm;
using namespace lotus::gvfg;
using namespace lotus::unittest;

namespace {

static bool containsUseSite(GuardedValueFlowNode *node,
                            GuardedValueFlowSite *site) {
  if (!node || !site)
    return false;
  for (GuardedValueFlowSite *use_site : node->useSites()) {
    if (use_site == site)
      return true;
  }
  return false;
}

class GuardedValueFlowParityTest : public LlvmModuleTest {
protected:
  struct Pipeline {
    std::unique_ptr<legacy::PassManager> pm;
    GuardedValueFlowGraphBuilderPass *builder{nullptr};
  };

  struct AdapterPipeline {
    std::unique_ptr<legacy::PassManager> pm;
    LotusAA *lotus{nullptr};
    GuardedValueFlowGraphBuilderPass *builder{nullptr};
  };

  static void initializePassInfra() {
    static bool initialized = false;
    if (initialized)
      return;

    auto &registry = *PassRegistry::getPassRegistry();
    initializeCore(registry);
    initializeAnalysis(registry);
    initializeTransformUtils(registry);
    initialized = true;
  }

  Pipeline runBuilder(Module &M) {
    initializePassInfra();
    Pipeline pipeline;
    pipeline.pm = std::make_unique<legacy::PassManager>();
    pipeline.builder = new GuardedValueFlowGraphBuilderPass();
    pipeline.pm->add(new gsa::ControlDependenceAnalysisPass());
    pipeline.pm->add(new gsa::GateAnalysisPass());
    pipeline.pm->add(pipeline.builder);
    pipeline.pm->run(M);
    return pipeline;
  }

  AdapterPipeline runAdapterPipeline(Module &M) {
    initializePassInfra();
    AdapterPipeline pipeline;
    pipeline.pm = std::make_unique<legacy::PassManager>();
    pipeline.lotus = new LotusAA();
    pipeline.builder = new GuardedValueFlowGraphBuilderPass();
    pipeline.pm->add(new gsa::ControlDependenceAnalysisPass());
    pipeline.pm->add(new gsa::GateAnalysisPass());
    pipeline.pm->add(pipeline.lotus);
    pipeline.pm->add(pipeline.builder);
    pipeline.pm->add(new LotusGuardedValueFlowAdapterPass());
    pipeline.pm->run(M);
    return pipeline;
  }
};

TEST_F(GuardedValueFlowParityTest,
       TracksReverseParentsAndPriorValueFlowFiltering) {
  const char *source = R"(
    define void @test() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  GuardedValueFlowGraph graph(F);
  BasicBlock *entry = &F->getEntryBlock();
  auto *cond = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, Type::getInt1Ty(context),
      &graph, entry);
  auto *lhs = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, Type::getInt32Ty(context),
      &graph, entry);
  auto *rhs = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, Type::getInt32Ty(context),
      &graph, entry);

  auto *add = graph.createNode<GuardedValueFlowOpcodeNode>(
      GuardedValueFlowNode::Kind::SimpleOpcode, Type::getInt32Ty(context),
      &graph, entry, GuardedValueFlowOpcodeNode::OpcodeKind::Add);
  add->addChild(lhs);
  add->addChild(rhs);

  auto *cast = graph.createNode<GuardedValueFlowOpcodeNode>(
      GuardedValueFlowNode::Kind::CastOpcode, Type::getInt64Ty(context), &graph,
      entry, GuardedValueFlowOpcodeNode::OpcodeKind::SExt);
  cast->addChild(lhs);

  auto *select = graph.createNode<GuardedValueFlowOpcodeNode>(
      GuardedValueFlowNode::Kind::SimpleOpcode, Type::getInt32Ty(context),
      &graph, entry, GuardedValueFlowOpcodeNode::OpcodeKind::Select);
  select->addChild(cond);
  select->addChild(lhs);
  select->addChild(rhs);

  auto *region = graph.findOrCreateUnitRegion(cond, true, entry,
                                              ConditionRef::none());
  ASSERT_NE(region, nullptr);

  EXPECT_TRUE(lhs->containsParent(add));
  EXPECT_TRUE(lhs->containsParent(cast));
  EXPECT_TRUE(lhs->containsParent(select));
  EXPECT_EQ(lhs->getNumParents(), 3u);

  auto lhs_vflow = lhs->getValueFlowParents();
  EXPECT_EQ(lhs_vflow.size(), 3u);
  EXPECT_NE(find(lhs_vflow.begin(), lhs_vflow.end(), add), lhs_vflow.end());
  EXPECT_NE(find(lhs_vflow.begin(), lhs_vflow.end(), cast), lhs_vflow.end());
  EXPECT_NE(find(lhs_vflow.begin(), lhs_vflow.end(), select), lhs_vflow.end());

  EXPECT_EQ(cond->getNumParents(), 2u);
  auto cond_vflow = cond->getValueFlowParents();
  EXPECT_TRUE(cond_vflow.empty());

  add->clearChildren();
  EXPECT_FALSE(lhs->containsParent(add));
  EXPECT_EQ(lhs->getNumParents(), 2u);
}

TEST_F(GuardedValueFlowParityTest, ModelsReturnPhiSelectAndOperationalSites) {
  const char *source = R"(
    define i32 @test(i1 %cond, i32* %p, i32 %x, i32 %y, i32 %idx) {
    entry:
      %sum = add i32 %x, %y
      %cmp = icmp sgt i32 %sum, 0
      br i1 %cond, label %then, label %else
    then:
      %div = sdiv i32 %sum, %y
      %gep = getelementptr i32, i32* %p, i32 %idx
      store i32 %div, i32* %gep
      br label %merge
    else:
      %sel = select i1 %cmp, i32 %x, i32 %y
      br label %merge
    merge:
      %phi = phi i32 [ %div, %then ], [ %sel, %else ]
      ret i32 %phi
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  GuardedValueFlowReturnNode *common_return = nullptr;
  GuardedValueFlowPhiNode *phi_node = nullptr;
  GuardedValueFlowNode *select_node = nullptr;
  StoreInst *store_inst = nullptr;
  for (const auto &node_ptr : graph.nodes()) {
    if (node_ptr->getKind() == GuardedValueFlowNode::Kind::CommonReturn)
      common_return = dyn_cast<GuardedValueFlowReturnNode>(node_ptr.get());
    if (node_ptr->getKind() == GuardedValueFlowNode::Kind::Phi)
      phi_node = dyn_cast<GuardedValueFlowPhiNode>(node_ptr.get());
  }

  for (Instruction &I : instructions(*F)) {
    if (isa<SelectInst>(&I))
      select_node = graph.findNode(&I);
    if (auto *SI = dyn_cast<StoreInst>(&I))
      store_inst = SI;
  }

  ASSERT_NE(common_return, nullptr);
  ASSERT_NE(phi_node, nullptr);
  ASSERT_NE(select_node, nullptr);
  ASSERT_EQ(common_return->children().size(), 1u);
  EXPECT_EQ(common_return->children().front().target, phi_node);
  auto *common_return_site = common_return->getReturnSite(phi_node);
  EXPECT_NE(common_return_site, nullptr);
  EXPECT_FALSE(containsUseSite(phi_node, common_return_site));
  EXPECT_EQ(common_return->getRegion(), graph.findRegion(&F->getEntryBlock()));

  ASSERT_EQ(phi_node->incoming().size(), 2u);
  EXPECT_NE(phi_node->incoming()[0].value_node, nullptr);
  EXPECT_NE(phi_node->incoming()[1].value_node, nullptr);
  EXPECT_NE(phi_node->incoming()[0].incoming_block, nullptr);
  EXPECT_NE(phi_node->incoming()[1].incoming_block, nullptr);

  ASSERT_EQ(select_node->children().size(), 1u);
  auto *select_opcode =
      dyn_cast<GuardedValueFlowOpcodeNode>(select_node->children().front().target);
  ASSERT_NE(select_opcode, nullptr);
  EXPECT_EQ(select_opcode->getOpcodeKind(),
            GuardedValueFlowOpcodeNode::OpcodeKind::Select);
  EXPECT_EQ(select_opcode->children().size(), 3u);
  ASSERT_NE(store_inst, nullptr);
  auto *store_mem = graph.findStoreMemoryNode(store_inst->getValueOperand(), store_inst);
  ASSERT_NE(store_mem, nullptr);
  EXPECT_EQ(store_mem->getRegion(), graph.findRegion(store_inst->getParent()));
  EXPECT_EQ(select_node->getRegion(), graph.findRegion(select_node->getParentBasicBlock()));
  EXPECT_EQ(graph.findNode(F->getArg(0))->getRegion(),
            graph.findRegion(&F->getEntryBlock()));

  bool saw_compare_site = false;
  bool saw_div_site = false;
  bool saw_gep_site = false;
  for (const auto &site_ptr : graph.sites()) {
    if (auto *cmp_site =
            dynamic_cast<GuardedValueFlowCompareSite *>(site_ptr.get())) {
      saw_compare_site = true;
      EXPECT_NE(cmp_site->getLhsOperand(), nullptr);
      EXPECT_NE(cmp_site->getRhsOperand(), nullptr);
    }
    if (auto *div_site = dynamic_cast<GuardedValueFlowDivSite *>(site_ptr.get())) {
      saw_div_site = true;
      EXPECT_NE(div_site->getLhsOperand(), nullptr);
      EXPECT_NE(div_site->getRhsOperand(), nullptr);
    }
    if (auto *gep_site =
            dynamic_cast<GuardedValueFlowGEPReferenceSite *>(site_ptr.get())) {
      saw_gep_site = true;
      EXPECT_NE(gep_site->getPointerOperand(), nullptr);
      EXPECT_FALSE(gep_site->getOffsetOperands().empty());
      EXPECT_NE(gep_site->getResultNode(), nullptr);
    }
  }

  EXPECT_TRUE(saw_compare_site);
  EXPECT_TRUE(saw_div_site);
  EXPECT_TRUE(saw_gep_site);
}

TEST_F(GuardedValueFlowParityTest,
       SharesRepresentativeLoadMemoryNodesForEquivalentLoads) {
  const char *source = R"(
    define i32 @test(i32* %p, i32 %v) {
    entry:
      store i32 %v, i32* %p
      %a = load i32, i32* %p
      %b = load i32, i32* %p
      %sum = add i32 %a, %b
      ret i32 %sum
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runAdapterPipeline(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  SmallVector<LoadInst *, 2> loads;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I))
      loads.push_back(LI);
  }
  ASSERT_EQ(loads.size(), 2u);

  auto *first_mem = graph.findLoadMemoryNode(loads[0]);
  auto *second_mem = graph.findLoadMemoryNode(loads[1]);
  ASSERT_NE(first_mem, nullptr);
  ASSERT_NE(second_mem, nullptr);
  EXPECT_EQ(first_mem, second_mem);

  auto *first_load_node = graph.findNode(loads[0]);
  auto *second_load_node = graph.findNode(loads[1]);
  ASSERT_NE(first_load_node, nullptr);
  ASSERT_NE(second_load_node, nullptr);
  ASSERT_EQ(first_load_node->children().size(), 1u);
  ASSERT_EQ(second_load_node->children().size(), 1u);
  EXPECT_EQ(first_load_node->children().front().target, first_mem);
  EXPECT_EQ(second_load_node->children().front().target, second_mem);
}

TEST_F(GuardedValueFlowParityTest, BuildsCompoundRegionsForNestedControlDependence) {
  const char *source = R"(
    define void @test(i1 %a, i1 %b, i32* %p) {
    entry:
      br i1 %a, label %outer, label %exit
    outer:
      br i1 %b, label %inner, label %join
    inner:
      store i32 1, i32* %p
      br label %join
    join:
      ret void
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  BasicBlock *outer = F->getBasicBlockList().getNextNode(F->getEntryBlock());
  ASSERT_NE(outer, nullptr);
  BasicBlock *inner = nullptr;
  for (BasicBlock &BB : *F) {
    if (BB.getName() == "inner")
      inner = &BB;
  }
  ASSERT_NE(inner, nullptr);

  auto *outer_region = graph.findRegion(outer);
  auto *inner_region = graph.findRegion(inner);
  ASSERT_NE(outer_region, nullptr);
  ASSERT_NE(inner_region, nullptr);
  EXPECT_FALSE(outer_region->isAlwaysTrue());
  EXPECT_TRUE(inner_region->isCompound());
  ASSERT_EQ(inner_region->children().size(), 1u);
  auto *inner_opcode =
      dyn_cast<GuardedValueFlowOpcodeNode>(inner_region->children().front().target);
  ASSERT_NE(inner_opcode, nullptr);
  EXPECT_EQ(inner_opcode->getOpcodeKind(),
            GuardedValueFlowOpcodeNode::OpcodeKind::And);
}

TEST_F(GuardedValueFlowParityTest,
       UsesPriorDereferenceAndDivideUseSiteSemantics) {
  const char *source = R"(
    define i32 @test(i32* %p, i32 %lhs, i32 %rhs) {
    entry:
      %v = load i32, i32* %p
      %q = sdiv i32 %lhs, %rhs
      %sum = add i32 %v, %q
      ret i32 %sum
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  LoadInst *load = nullptr;
  BinaryOperator *div = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I))
      load = LI;
    if (I.getOpcode() == Instruction::SDiv)
      div = cast<BinaryOperator>(&I);
  }
  ASSERT_NE(load, nullptr);
  ASSERT_NE(div, nullptr);

  auto *ptr_node = graph.findNode(F->getArg(0));
  auto *load_value = graph.findNode(load);
  auto *lhs_node = graph.findNode(F->getArg(1));
  auto *rhs_node = graph.findNode(F->getArg(2));
  ASSERT_NE(ptr_node, nullptr);
  ASSERT_NE(load_value, nullptr);
  ASSERT_NE(lhs_node, nullptr);
  ASSERT_NE(rhs_node, nullptr);

  GuardedValueFlowDereferenceSite *deref_site = nullptr;
  GuardedValueFlowDivSite *div_site = nullptr;
  for (const auto &site_ptr : graph.sites()) {
    if (site_ptr->getInstruction() == load)
      deref_site = dynamic_cast<GuardedValueFlowDereferenceSite *>(site_ptr.get());
    if (site_ptr->getInstruction() == div)
      div_site = dynamic_cast<GuardedValueFlowDivSite *>(site_ptr.get());
  }

  ASSERT_NE(deref_site, nullptr);
  EXPECT_TRUE(containsUseSite(ptr_node, deref_site));
  EXPECT_FALSE(containsUseSite(load_value, deref_site));

  ASSERT_NE(div_site, nullptr);
  EXPECT_TRUE(containsUseSite(rhs_node, div_site));
  EXPECT_FALSE(containsUseSite(lhs_node, div_site));
}

TEST_F(GuardedValueFlowParityTest,
       PreservesLabeledConditionForNonImmediateControlledBlock) {
  const char *source = R"(
    define void @test(i1 %a, i32* %p) {
    entry:
      br i1 %a, label %then, label %exit
    then:
      br label %mid
    mid:
      br label %inner
    inner:
      store i32 1, i32* %p
      ret void
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  BasicBlock *then_bb = nullptr;
  BasicBlock *inner_bb = nullptr;
  for (BasicBlock &BB : *F) {
    if (BB.getName() == "then")
      then_bb = &BB;
    if (BB.getName() == "inner")
      inner_bb = &BB;
  }
  ASSERT_NE(then_bb, nullptr);
  ASSERT_NE(inner_bb, nullptr);

  auto block_conditions = graph.getBlockConditions(inner_bb);
  ASSERT_EQ(block_conditions.size(), 1u);
  EXPECT_EQ(block_conditions.front().control_block, &F->getEntryBlock());
  EXPECT_EQ(block_conditions.front().guard_successor, then_bb);
  EXPECT_TRUE(block_conditions.front().sense);
  EXPECT_NE(block_conditions.front().condition_node, nullptr);
  EXPECT_EQ(block_conditions.front().condition.getKind(),
            ConditionRef::Kind::StructuralGuard);
}

TEST_F(GuardedValueFlowParityTest,
       SimplifiesContradictoryAndTautologicalRegions) {
  const char *source = R"(
    define void @test(i1 %cond, i1 %other) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret void
    else:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  auto *cond_node = graph.findNode(F->getArg(0));
  auto *other_node = graph.findNode(F->getArg(1));
  ASSERT_NE(cond_node, nullptr);
  ASSERT_NE(other_node, nullptr);

  auto *true_region = graph.findUnitRegion(cond_node, true);
  auto *false_region = graph.findUnitRegion(cond_node, false);
  ASSERT_NE(true_region, nullptr);
  ASSERT_NE(false_region, nullptr);

  ASSERT_EQ(false_region->children().size(), 1u);
  auto *not_opcode =
      dyn_cast<GuardedValueFlowOpcodeNode>(false_region->children().front().target);
  ASSERT_NE(not_opcode, nullptr);
  EXPECT_EQ(not_opcode->getOpcodeKind(),
            GuardedValueFlowOpcodeNode::OpcodeKind::Xor);
  EXPECT_TRUE(not_opcode->hasIntConstant());
  EXPECT_EQ(not_opcode->getIntConstant(), -1);

  auto *not_true = graph.findOrCreateNotRegion(true_region, &F->getEntryBlock());
  EXPECT_EQ(not_true, false_region);

  auto *other_true = graph.findOrCreateUnitRegion(
      other_node, true, &F->getEntryBlock(), ConditionRef::none());
  ASSERT_NE(other_true, nullptr);

  auto *nontrivial_and =
      graph.findOrCreateAndRegion(true_region, other_true, &F->getEntryBlock());
  ASSERT_EQ(nontrivial_and->children().size(), 1u);
  auto *and_opcode = dyn_cast<GuardedValueFlowOpcodeNode>(
      nontrivial_and->children().front().target);
  ASSERT_NE(and_opcode, nullptr);
  EXPECT_EQ(and_opcode->getOpcodeKind(),
            GuardedValueFlowOpcodeNode::OpcodeKind::And);

  auto *nontrivial_or =
      graph.findOrCreateOrRegion(true_region, other_true, &F->getEntryBlock());
  ASSERT_EQ(nontrivial_or->children().size(), 1u);
  auto *or_opcode = dyn_cast<GuardedValueFlowOpcodeNode>(
      nontrivial_or->children().front().target);
  ASSERT_NE(or_opcode, nullptr);
  EXPECT_EQ(or_opcode->getOpcodeKind(),
            GuardedValueFlowOpcodeNode::OpcodeKind::Or);

  auto *contradiction =
      graph.findOrCreateAndRegion(true_region, false_region, &F->getEntryBlock());
  EXPECT_TRUE(contradiction->isAlwaysFalse());
  EXPECT_FALSE(contradiction->isSatisfiable());
  ASSERT_EQ(contradiction->children().size(), 1u);
  auto *false_literal = contradiction->children().front().target;
  ASSERT_NE(false_literal, nullptr);
  auto *false_value = dyn_cast_or_null<ConstantInt>(false_literal->getLLVMValue());
  ASSERT_NE(false_value, nullptr);
  EXPECT_FALSE(false_value->isOne());

  auto *tautology =
      graph.findOrCreateOrRegion(true_region, false_region, &F->getEntryBlock());
  EXPECT_TRUE(tautology->isAlwaysTrue());
  EXPECT_TRUE(tautology->isSatisfiable());
  ASSERT_EQ(tautology->children().size(), 1u);
  auto *true_literal = tautology->children().front().target;
  ASSERT_NE(true_literal, nullptr);
  auto *true_value = dyn_cast_or_null<ConstantInt>(true_literal->getLLVMValue());
  ASSERT_NE(true_value, nullptr);
  EXPECT_TRUE(true_value->isOne());
}

TEST_F(GuardedValueFlowParityTest,
       ModelsConstantExprAndConstantAggregateOperands) {
  const char *source = R"(
    @g = global i32 0

    define i8* @test() {
    entry:
      %slot = alloca [2 x i32]
      store [2 x i32] [i32 1, i32 2], [2 x i32]* %slot
      ret i8* bitcast (i32* @g to i8*)
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  StoreInst *store = nullptr;
  ReturnInst *ret = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *SI = dyn_cast<StoreInst>(&I))
      store = SI;
    if (auto *RI = dyn_cast<ReturnInst>(&I))
      ret = RI;
  }
  ASSERT_NE(store, nullptr);
  ASSERT_NE(ret, nullptr);

  auto *aggregate_node = graph.findNode(store->getValueOperand());
  ASSERT_NE(aggregate_node, nullptr);
  ASSERT_EQ(aggregate_node->children().size(), 1u);
  auto *concat =
      dyn_cast<GuardedValueFlowOpcodeNode>(aggregate_node->children().front().target);
  ASSERT_NE(concat, nullptr);
  EXPECT_EQ(concat->getOpcodeKind(),
            GuardedValueFlowOpcodeNode::OpcodeKind::Concat);

  auto *const_expr_node = graph.findNode(ret->getReturnValue());
  ASSERT_NE(const_expr_node, nullptr);
  ASSERT_EQ(const_expr_node->children().size(), 1u);
  auto *cast_node =
      dyn_cast<GuardedValueFlowOpcodeNode>(const_expr_node->children().front().target);
  ASSERT_NE(cast_node, nullptr);
  EXPECT_EQ(cast_node->getOpcodeKind(),
            GuardedValueFlowOpcodeNode::OpcodeKind::BitCast);
}

TEST_F(GuardedValueFlowParityTest,
       ToleratesPartiallyModeledConstantExprOperands) {
  const char *source = R"(
    define i32 @test() {
    entry:
      ret i32 extractvalue ({ i32, i32 } { i32 1, i32 2 }, 0)
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  auto *ret = cast<ReturnInst>(F->getEntryBlock().getTerminator());
  auto *const_expr_node = graph.findNode(ret->getReturnValue());
  ASSERT_NE(const_expr_node, nullptr);
  EXPECT_TRUE(const_expr_node->children().empty());
}

TEST_F(GuardedValueFlowParityTest,
       UsesPriorStructFieldSizeOffsetsForGEP) {
  const char *source = R"(
    %pair = type { i8, i32 }

    define i32* @test(%pair* %p) {
    entry:
      %field = getelementptr %pair, %pair* %p, i64 0, i32 1
      ret i32* %field
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  GetElementPtrInst *gep = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
      gep = GEP;
      break;
    }
  }
  ASSERT_NE(gep, nullptr);

  auto *gep_node = graph.findNode(gep);
  ASSERT_NE(gep_node, nullptr);
  ASSERT_EQ(gep_node->children().size(), 1u);
  auto *gep_opcode =
      dyn_cast<GuardedValueFlowOpcodeNode>(gep_node->children().front().target);
  ASSERT_NE(gep_opcode, nullptr);
  ASSERT_EQ(gep_opcode->children().size(), 2u);

  auto *offset_base = gep_opcode->children()[1].target;
  ASSERT_NE(offset_base, nullptr);
  ASSERT_EQ(offset_base->children().size(), 1u);
  auto *offset_opcode =
      dyn_cast<GuardedValueFlowOpcodeNode>(offset_base->children().front().target);
  ASSERT_NE(offset_opcode, nullptr);
  EXPECT_TRUE(offset_opcode->hasIntConstant());
  EXPECT_EQ(offset_opcode->getIntConstant(), 8);
}

TEST_F(GuardedValueFlowParityTest, UsesTruncForWiderDynamicGEPIndices) {
  const char *source = R"(
    define i32* @test(i32* %p, i128 %idx) {
    entry:
      %elt = getelementptr i32, i32* %p, i128 %idx
      ret i32* %elt
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  GetElementPtrInst *gep = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *GEP = dyn_cast<GetElementPtrInst>(&I))
      gep = GEP;
  }
  ASSERT_NE(gep, nullptr);

  GuardedValueFlowOpcodeNode *index_cast = nullptr;
  for (const auto &node_ptr : graph.nodes()) {
    auto *opcode = dyn_cast<GuardedValueFlowOpcodeNode>(node_ptr.get());
    if (!opcode || opcode->getKind() != GuardedValueFlowNode::Kind::CastOpcode)
      continue;
    if (opcode->getDescription() == "gep.index.cast") {
      index_cast = opcode;
      break;
    }
  }

  ASSERT_NE(index_cast, nullptr);
  EXPECT_EQ(index_cast->getOpcodeKind(),
            GuardedValueFlowOpcodeNode::OpcodeKind::Trunc);
}

TEST_F(GuardedValueFlowParityTest,
       UsesOriginalIndexOperandExactlyOnceForGEPSiteOffsets) {
  const char *source = R"(
    define i32* @test(i32* %p, i128 %idx) {
    entry:
      %elt = getelementptr i32, i32* %p, i128 %idx
      ret i32* %elt
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  auto *ptr_node = graph.findNode(F->getArg(0));
  auto *idx_node = graph.findNode(F->getArg(1));
  ASSERT_NE(ptr_node, nullptr);
  ASSERT_NE(idx_node, nullptr);

  GuardedValueFlowGEPReferenceSite *gep_site = nullptr;
  for (const auto &site_ptr : graph.sites()) {
    auto *candidate =
        dynamic_cast<GuardedValueFlowGEPReferenceSite *>(site_ptr.get());
    if (!candidate)
      continue;
    gep_site = candidate;
    break;
  }

  ASSERT_NE(gep_site, nullptr);
  EXPECT_EQ(gep_site->getPointerOperand(), ptr_node);
  ASSERT_EQ(gep_site->getOffsetOperands().size(), 1u);
  EXPECT_EQ(gep_site->getOffsetOperands().front(), idx_node);
  EXPECT_EQ(std::count(gep_site->getOffsetOperands().begin(),
                       gep_site->getOffsetOperands().end(), idx_node),
            1);
}

TEST_F(GuardedValueFlowParityTest,
       BuildsSwitchFunctionsWithDiagnosticsAndStructuredGuards) {
  const char *source = R"(
    define i32 @test(i32 %x) {
    entry:
      switch i32 %x, label %default [ i32 1, label %one ]
    one:
      ret i32 1
    default:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  BasicBlock *one_bb = nullptr;
  BasicBlock *default_bb = nullptr;
  for (BasicBlock &BB : *F) {
    if (BB.getName() == "one")
      one_bb = &BB;
    if (BB.getName() == "default")
      default_bb = &BB;
  }
  ASSERT_NE(one_bb, nullptr);
  ASSERT_NE(default_bb, nullptr);

  auto *switch_inst = cast<SwitchInst>(F->getEntryBlock().getTerminator());
  auto *case_guard = graph.findSyntheticGuardNode(switch_inst, one_bb);
  auto *default_guard = graph.findSyntheticGuardNode(switch_inst, default_bb);
  ASSERT_NE(case_guard, nullptr);
  ASSERT_NE(default_guard, nullptr);
  EXPECT_FALSE(graph.getBlockConditions(one_bb).empty());
  EXPECT_FALSE(graph.getBlockConditions(default_bb).empty());
  EXPECT_TRUE(graph.diagnostics().empty());
}

TEST_F(GuardedValueFlowParityTest, ModelsFNegWithoutDroppingTheGraph) {
  const char *source = R"(
    define float @test(float %x) {
    entry:
      %neg = fneg float %x
      ret float %neg
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  Instruction *fneg_inst = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (I.getOpcode() == Instruction::FNeg) {
      fneg_inst = &I;
      break;
    }
  }
  ASSERT_NE(fneg_inst, nullptr);

  auto *neg_node = graph.findNode(fneg_inst);
  ASSERT_NE(neg_node, nullptr);
  ASSERT_EQ(neg_node->children().size(), 1u);
  auto *opcode =
      dyn_cast<GuardedValueFlowOpcodeNode>(neg_node->children().front().target);
  ASSERT_NE(opcode, nullptr);
  EXPECT_EQ(opcode->getOpcodeKind(),
            GuardedValueFlowOpcodeNode::OpcodeKind::FSub);
  EXPECT_TRUE(graph.diagnostics().empty());
}

TEST_F(GuardedValueFlowParityTest,
       KeepsGraphsForUnsupportedInstructionsUsingUnknownNodesAndDiagnostics) {
  const char *source = R"(
    define i32 @test({i32, i32} %pair) {
    entry:
      %field = extractvalue {i32, i32} %pair, 0
      ret i32 %field
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  Instruction *extract_inst = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (I.getOpcode() == Instruction::ExtractValue) {
      extract_inst = &I;
      break;
    }
  }
  ASSERT_NE(extract_inst, nullptr);

  auto *field_node = graph.findNode(extract_inst);
  ASSERT_NE(field_node, nullptr);
  EXPECT_EQ(field_node->getKind(), GuardedValueFlowNode::Kind::Unknown);
  EXPECT_TRUE(graph.hasDiagnostics());
}

} // namespace
