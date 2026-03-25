#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "IR/GVFG/LotusAdapter.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::gvfg;
using namespace lotus::unittest;

namespace {

class GuardedValueFlowTest : public LlvmModuleTest {
protected:
  struct Pipeline {
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

  Pipeline runAdapter(Module &M) {
    initializePassInfra();
    Pipeline pipeline;
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

TEST_F(GuardedValueFlowTest, BuildsCallLoadStoreAndRegionNodes) {
  const char *source = R"(
    define i32* @callee(i32** %p) {
    entry:
      %tmp = load i32*, i32** %p
      ret i32* %tmp
    }

    define i32* @test(i1 %cond, i32** %p, i32* %v) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      store i32* %v, i32** %p
      br label %merge
    else:
      %ret = call i32* @callee(i32** %p)
      br label %merge
    merge:
      %phi = phi i32* [ %v, %then ], [ %ret, %else ]
      ret i32* %phi
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  EXPECT_NE(graph.findRegion(&F->getEntryBlock()), nullptr);
  EXPECT_NE(graph.findNode(F->getArg(1)), nullptr);

  Instruction *call_inst = nullptr;
  Instruction *phi_inst = nullptr;
  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB) {
      if (isa<CallInst>(&I))
        call_inst = &I;
      if (isa<PHINode>(&I))
        phi_inst = &I;
    }
  }

  ASSERT_NE(call_inst, nullptr);
  ASSERT_NE(phi_inst, nullptr);
  EXPECT_NE(graph.findCallSite(call_inst), nullptr);

  GuardedValueFlowNode *phi_node = graph.findNode(phi_inst);
  ASSERT_NE(phi_node, nullptr);
  EXPECT_EQ(phi_node->getKind(), GuardedValueFlowNode::Kind::Phi);
  EXPECT_EQ(phi_node->children().size(), 2u);
}

TEST_F(GuardedValueFlowTest, AssignsDenseCommonArgumentIndices) {
  const char *source = R"(
    define void @test(i32 %a, i8* %b, i64 %c) {
    entry:
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

  for (unsigned idx = 0; idx < F->arg_size(); ++idx) {
    auto *arg_node = graph.findNode(F->getArg(idx));
    ASSERT_NE(arg_node, nullptr);
    EXPECT_EQ(arg_node->getKind(), GuardedValueFlowNode::Kind::CommonArgument);
    EXPECT_EQ(arg_node->getIndex(), idx);
  }
}

TEST_F(GuardedValueFlowTest, UsesDensePseudoInterfaceIndices) {
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

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  auto *ret_inst = cast<ReturnInst>(F->getEntryBlock().getTerminator());
  auto *site = graph.createSite<GuardedValueFlowCallSite>(&graph, ret_inst);

  auto *pseudo_input0 = graph.createNode<GuardedValueFlowCallOutputNode>(
      GuardedValueFlowNode::Kind::CallSitePseudoInput,
      Type::getInt32Ty(context), &graph, &F->getEntryBlock(), nullptr, ret_inst,
      F);
  pseudo_input0->setIndex(0);
  site->addPseudoInput(F, pseudo_input0);

  auto *pseudo_input2 = graph.createNode<GuardedValueFlowCallOutputNode>(
      GuardedValueFlowNode::Kind::CallSitePseudoInput,
      Type::getInt32Ty(context), &graph, &F->getEntryBlock(), nullptr, ret_inst,
      F);
  pseudo_input2->setIndex(2);
  site->addPseudoInput(F, pseudo_input2);

  EXPECT_EQ(site->getNumPseudoInputs(F), 2u);
  EXPECT_EQ(site->getPseudoInput(F, 0), pseudo_input0);
  EXPECT_EQ(site->getPseudoInput(F, 1), pseudo_input2);
  EXPECT_EQ(site->getPseudoInput(F, 2), nullptr);
  EXPECT_EQ(pseudo_input0->getIndex(), 0u);
  EXPECT_EQ(pseudo_input2->getIndex(), 1u);
}

TEST_F(GuardedValueFlowTest, KeepsCanonicalAndInterfaceLookupsSeparate) {
  const char *source = R"(
    define void @test(i32* %p) {
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
  auto *formal = &*F->arg_begin();
  auto *common_arg = graph.createNode<GuardedValueFlowArgumentNode>(
      GuardedValueFlowNode::Kind::CommonArgument, formal->getType(), &graph,
      entry, formal);
  graph.mapValueNode(formal, common_arg);

  auto *pseudo_arg = graph.createNode<GuardedValueFlowArgumentNode>(
      GuardedValueFlowNode::Kind::PseudoArgument, formal->getType(), &graph,
      entry, formal);
  pseudo_arg->setIndex(0);
  graph.registerPseudoArgument(pseudo_arg);
  graph.mapPseudoArgumentSource(formal, pseudo_arg);

  auto *ret_inst = cast<ReturnInst>(entry->getTerminator());
  auto *interface_value =
      graph.createSyntheticInterfaceValue(Type::getInt32Ty(context),
                                         "pseudo.interface");
  auto *interface_node = graph.createNode<GuardedValueFlowCallOutputNode>(
      GuardedValueFlowNode::Kind::CallSitePseudoInput,
      interface_value->getType(), &graph, entry, interface_value, ret_inst, F);
  graph.mapInterfaceNode(interface_value, interface_node);

  EXPECT_EQ(graph.findNode(formal), common_arg);
  EXPECT_EQ(graph.findPseudoArgumentBySource(formal), pseudo_arg);
  EXPECT_EQ(graph.findInterfaceNode(interface_value), interface_node);
  EXPECT_EQ(graph.findNode(interface_value), nullptr);
}

TEST_F(GuardedValueFlowTest,
       KeepsAnonymousStoreMemoryProducersDistinctForMatchingRegions) {
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
  auto *ret_inst = cast<ReturnInst>(F->getEntryBlock().getTerminator());
  auto *load_mem = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::LoadMemory, Type::getInt32Ty(context), &graph,
      &F->getEntryBlock(), nullptr, ret_inst);
  auto *producer_a = graph.createAnonymousStoreMemoryNode(
      Type::getInt32Ty(context), &F->getEntryBlock(), ret_inst, "anon.a");
  auto *producer_b = graph.createAnonymousStoreMemoryNode(
      Type::getInt32Ty(context), &F->getEntryBlock(), ret_inst, "anon.b");

  auto *cond_node = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, Type::getInt1Ty(context),
      &graph, &F->getEntryBlock());
  auto *region_true = graph.findOrCreateUnitRegion(cond_node, true,
                                                   &F->getEntryBlock(),
                                                   ConditionRef::none());
  auto *region_false = graph.findOrCreateUnitRegion(cond_node, false,
                                                    &F->getEntryBlock(),
                                                    ConditionRef::none());

  ASSERT_NE(producer_a, producer_b);
  load_mem->addMatchingRegion(producer_a, region_true, ConditionRef::none());
  load_mem->addMatchingRegion(producer_b, region_false, ConditionRef::none());

  ASSERT_EQ(load_mem->getMatchingRegions().size(), 2u);
  EXPECT_EQ(load_mem->getMatchingRegion(producer_a), region_true);
  EXPECT_EQ(load_mem->getMatchingRegion(producer_b), region_false);
}

TEST_F(GuardedValueFlowTest, StoresSummaryNodesPerCalleeWithoutOverwrite) {
  const char *source = R"(
    declare void @callee_a()
    declare void @callee_b()

    define void @test() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  Function *callee_a = module->getFunction("callee_a");
  Function *callee_b = module->getFunction("callee_b");
  ASSERT_NE(F, nullptr);
  ASSERT_NE(callee_a, nullptr);
  ASSERT_NE(callee_b, nullptr);

  GuardedValueFlowGraph graph(F);
  auto *ret_inst = cast<ReturnInst>(F->getEntryBlock().getTerminator());
  auto *site = graph.createSite<GuardedValueFlowCallSite>(&graph, ret_inst);

  auto *input_a = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteArgumentSummary,
      Type::getInt32Ty(context), &graph, &F->getEntryBlock(), ret_inst,
      callee_a, 1);
  auto *input_b = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteArgumentSummary,
      Type::getInt32Ty(context), &graph, &F->getEntryBlock(), ret_inst,
      callee_b, 1);
  auto *output_a = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteReturnSummary,
      Type::getInt32Ty(context), &graph, &F->getEntryBlock(), ret_inst,
      callee_a, 2);
  auto *output_b = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteReturnSummary,
      Type::getInt32Ty(context), &graph, &F->getEntryBlock(), ret_inst,
      callee_b, 2);

  site->setInputSummaryNode(callee_a, 1, input_a);
  site->setInputSummaryNode(callee_b, 1, input_b);
  site->setOutputSummaryNode(callee_a, 2, output_a);
  site->setOutputSummaryNode(callee_b, 2, output_b);

  EXPECT_EQ(site->getInputSummaryNode(callee_a, 1), input_a);
  EXPECT_EQ(site->getInputSummaryNode(callee_b, 1), input_b);
  EXPECT_EQ(site->getOutputSummaryNode(callee_a, 2), output_a);
  EXPECT_EQ(site->getOutputSummaryNode(callee_b, 2), output_b);
}

TEST_F(GuardedValueFlowTest, ExposesHighLevelQueryHelpersForClients) {
  const char *source = R"(
    define i32 @test(i32* %p, i32 %v) {
    entry:
      store i32 %v, i32* %p
      %loaded = load i32, i32* %p
      ret i32 %loaded
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runAdapter(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  LoadInst *load = nullptr;
  StoreInst *store = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I))
      load = LI;
    if (auto *SI = dyn_cast<StoreInst>(&I))
      store = SI;
  }
  ASSERT_NE(load, nullptr);
  ASSERT_NE(store, nullptr);

  auto *load_node = graph.findNode(load);
  ASSERT_NE(load_node, nullptr);

  auto direct_dependencies = graph.getDirectDataDependencies(load_node);
  ASSERT_EQ(direct_dependencies.size(), 1u);
  EXPECT_EQ(direct_dependencies.front()->getKind(),
            GuardedValueFlowNode::Kind::LoadMemory);

  auto memory_producers = graph.getMemoryProducers(load_node);
  ASSERT_EQ(memory_producers.size(), 1u);
  EXPECT_EQ(memory_producers.front().producer_memory,
            graph.findStoreMemoryNode(store->getValueOperand(), store));
  ASSERT_NE(memory_producers.front().producer_value, nullptr);
  EXPECT_EQ(memory_producers.front().producer_value, graph.findNode(F->getArg(1)));

  auto control_dependencies = graph.getEffectiveControlDependencies(load_node);
  EXPECT_TRUE(control_dependencies.empty());
}

TEST_F(GuardedValueFlowTest,
       DropsLinkWhenAdapterCannotCastBetweenLinkedTypes) {
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
  auto *parent = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::LoadMemory, Type::getInt32Ty(context), &graph,
      entry);
  StructType *aggregate_ty =
      StructType::get(context, {Type::getInt32Ty(context),
                                Type::getInt32Ty(context)});
  auto *child = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, aggregate_ty, &graph, entry);

  auto *linked = LotusGuardedValueFlowAdapterPass::safeLink(
      graph, parent, child, 0.5f, ConditionRef::none());
  EXPECT_EQ(linked, nullptr);
  EXPECT_TRUE(parent->children().empty());
  EXPECT_TRUE(child->parents().empty());
}

TEST_F(GuardedValueFlowTest,
       LeavesUnknownNonVoidIntrinsicsAsPlainValueNodesWithoutCallSites) {
  const char *source = R"(
    declare i32 @llvm.ctpop.i32(i32)

    define i32 @test(i32 %x) {
    entry:
      %v = call i32 @llvm.ctpop.i32(i32 %x)
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  CallBase *intrinsic_call = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      intrinsic_call = CB;
      break;
    }
  }
  ASSERT_NE(intrinsic_call, nullptr);

  auto *value_node = graph.findNode(intrinsic_call);
  ASSERT_NE(value_node, nullptr);
  EXPECT_EQ(value_node->getKind(), GuardedValueFlowNode::Kind::SimpleOperand);
  EXPECT_EQ(graph.findCallSite(intrinsic_call), nullptr);
}

TEST_F(GuardedValueFlowTest, PreservesImportedSemanticConditionIdentity) {
  const char *source = R"(
    define void @callee() {
    entry:
      ret void
    }

    define void @caller() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *callee = module->getFunction("callee");
  Function *caller = module->getFunction("caller");
  ASSERT_NE(callee, nullptr);
  ASSERT_NE(caller, nullptr);

  GuardedValueFlowGraph callee_graph(callee);
  GuardedValueFlowGraph caller_graph(caller);
  auto *source_cond = PathCond::createBlockAtom(&callee->getEntryBlock());
  auto *callee_region = callee_graph.findOrCreateSemanticRegion(
      source_cond, &callee->getEntryBlock());
  ASSERT_NE(callee_region, nullptr);
  ASSERT_NE(callee_region->getConditionNode(), nullptr);

  auto *imported_cond = PathCond::createImportedAtom(caller, source_cond);
  auto *caller_region = caller_graph.findOrCreateSemanticRegion(
      imported_cond, &caller->getEntryBlock(), callee_region->getConditionNode());
  ASSERT_NE(caller_region, nullptr);
  EXPECT_TRUE(caller_region->isInterfaceRegion());
  EXPECT_EQ(caller_region->getConditionNode(), callee_region->getConditionNode());
  EXPECT_TRUE(caller_region->children().empty());
  EXPECT_EQ(caller_region->getConditionNode()->getGraph(), &callee_graph);
  EXPECT_EQ(caller_region->getConditionNode()->getRegion(), callee_region);
}

} // namespace
