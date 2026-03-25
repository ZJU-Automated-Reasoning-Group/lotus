#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "IR/GSA/GSA.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "IR/GVFG/LotusAdapter.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <gtest/gtest.h>

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

static bool containsChild(GuardedValueFlowNode *node, GuardedValueFlowNode *child) {
  if (!node || !child)
    return false;
  for (const auto &edge : node->children()) {
    if (edge.target == child)
      return true;
  }
  return false;
}

static bool hasChildKind(GuardedValueFlowNode *node,
                         GuardedValueFlowNode::Kind kind) {
  if (!node)
    return false;
  for (const auto &edge : node->children()) {
    if (edge.target && edge.target->getKind() == kind)
      return true;
  }
  return false;
}

void initializePassInfra() {
  static bool initialized = false;
  if (initialized)
    return;

  auto &registry = *PassRegistry::getPassRegistry();
  initializeCore(registry);
  initializeAnalysis(registry);
  initializeTransformUtils(registry);
  initialized = true;
}

struct Pipeline {
  std::unique_ptr<legacy::PassManager> pm;
  LotusAA *lotus{nullptr};
  GuardedValueFlowGraphBuilderPass *builder{nullptr};
};

Pipeline runPipeline(Module &M) {
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

TEST(GuardedValueFlowAdapterShape, BuildsMemoryAndPseudoInterfaceShape) {
  const char *IR = R"(
    define i32* @load_arg(i32** %p) {
    entry:
      %v = load i32*, i32** %p
      ret i32* %v
    }

    define void @store_arg(i32** %p, i32* %v) {
    entry:
      store i32* %v, i32** %p
      ret void
    }

    define void @test(i32** %p, i32* %v) {
    entry:
      %ret = call i32* @load_arg(i32** %p)
      call void @store_arg(i32** %p, i32* %v)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR, "GuardedValueFlowAdapterShapeTest");
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("test");
  Function *load_callee = M->getFunction("load_arg");
  Function *store_callee = M->getFunction("store_arg");
  ASSERT_NE(F, nullptr);
  ASSERT_NE(load_callee, nullptr);
  ASSERT_NE(store_callee, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*load_callee));
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*store_callee));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  CallBase *load_call = nullptr;
  CallBase *store_call = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      Function *callee = CB->getCalledFunction();
      if (!callee)
        continue;
      if (callee->getName() == "load_arg")
        load_call = CB;
      else if (callee->getName() == "store_arg")
        store_call = CB;
    }
  }
  ASSERT_NE(load_call, nullptr);
  ASSERT_NE(store_call, nullptr);

  auto *load_site = graph.findCallSite(load_call);
  auto *store_site = graph.findCallSite(store_call);
  ASSERT_NE(load_site, nullptr);
  ASSERT_NE(store_site, nullptr);
  ASSERT_EQ(load_site->getCallees().size(), 1u);
  ASSERT_EQ(store_site->getCallees().size(), 1u);
  ASSERT_EQ(load_site->getCommonInputs().size(), 1u);
  ASSERT_EQ(store_site->getCommonInputs().size(), 2u);

  auto *common_output = load_site->getCommonOutput();
  ASSERT_NE(common_output, nullptr);
  EXPECT_EQ(common_output->getKind(),
            GuardedValueFlowNode::Kind::CallSiteCommonOutput);

  auto *caller_region = graph.findRegion(&F->getEntryBlock());
  ASSERT_NE(caller_region, nullptr);
  for (GuardedValueFlowNode *input : load_site->getCommonInputs()) {
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(containsUseSite(input, load_site));
    EXPECT_EQ(input->getRegion(), caller_region);
  }
  for (GuardedValueFlowNode *input : store_site->getCommonInputs()) {
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(containsUseSite(input, store_site));
    EXPECT_EQ(input->getRegion(), caller_region);
  }

  ASSERT_GT(load_site->getNumPseudoInputs(load_callee), 0u);
  auto *pseudo_input = load_site->getPseudoInput(load_callee, 0);
  ASSERT_NE(pseudo_input, nullptr);
  ASSERT_NE(pseudo_input->getLLVMValue(), nullptr);
  EXPECT_TRUE(containsUseSite(pseudo_input, load_site));
  EXPECT_EQ(pseudo_input->getRegion(), caller_region);
  EXPECT_EQ(pseudo_input->getIndex(), 0u);
  EXPECT_EQ(graph.findInterfaceNode(pseudo_input->getLLVMValue()), pseudo_input);
  EXPECT_EQ(graph.findNode(pseudo_input->getLLVMValue()), nullptr);

  ASSERT_GT(store_site->getNumPseudoOutputs(store_callee), 0u);
  auto *pseudo_output = store_site->getPseudoOutput(store_callee, 0);
  ASSERT_NE(pseudo_output, nullptr);
  ASSERT_NE(pseudo_output->getLLVMValue(), nullptr);
  EXPECT_EQ(pseudo_output->getRegion(), caller_region);
  EXPECT_TRUE(pseudo_output->children().empty());
  EXPECT_EQ(graph.findInterfaceNode(pseudo_output->getLLVMValue()), pseudo_output);
  auto *pseudo_output_mem =
      graph.findStoreMemoryNode(pseudo_output->getLLVMValue(), store_call);
  ASSERT_NE(pseudo_output_mem, nullptr);
  EXPECT_EQ(pseudo_output_mem->getKind(), GuardedValueFlowNode::Kind::StoreMemory);
  EXPECT_TRUE(containsChild(pseudo_output_mem, pseudo_output));
}

TEST(GuardedValueFlowAdapterShape,
     SharesRepresentativeLoadMemoryNodesForEquivalentLoads) {
  const char *IR = R"(
    define i32 @test(i32* %p, i32 %v) {
    entry:
      store i32 %v, i32* %p
      %a = load i32, i32* %p
      %b = load i32, i32* %p
      %sum = add i32 %a, %b
      ret i32 %sum
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  SmallVector<LoadInst *, 2> loads;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I))
      loads.push_back(LI);
  }
  ASSERT_EQ(loads.size(), 2u);

  auto *first_load_mem = graph.findLoadMemoryNode(loads[0]);
  auto *second_load_mem = graph.findLoadMemoryNode(loads[1]);
  ASSERT_NE(first_load_mem, nullptr);
  ASSERT_NE(second_load_mem, nullptr);
  EXPECT_EQ(first_load_mem, second_load_mem);

  auto *first_value_node = graph.findNode(loads[0]);
  auto *second_value_node = graph.findNode(loads[1]);
  ASSERT_NE(first_value_node, nullptr);
  ASSERT_NE(second_value_node, nullptr);
  ASSERT_EQ(first_value_node->children().size(), 1u);
  ASSERT_EQ(second_value_node->children().size(), 1u);
  EXPECT_EQ(first_value_node->children().front().target, first_load_mem);
  EXPECT_EQ(second_value_node->children().front().target, second_load_mem);

  EXPECT_FALSE(first_load_mem->getMatchingRegions().empty());
  EXPECT_FALSE(second_load_mem->getMatchingRegions().empty());
  EXPECT_EQ(first_load_mem->getMatchingRegions().size(),
            second_load_mem->getMatchingRegions().size());

  bool first_matches_store = false;
  for (const auto &match : first_load_mem->getMatchingRegions()) {
    ASSERT_NE(match.producer, nullptr);
    if (match.producer->getKind() == GuardedValueFlowNode::Kind::StoreMemory)
      first_matches_store = true;
  }
  EXPECT_TRUE(first_matches_store);

  bool second_matches_store = false;
  for (const auto &match : second_load_mem->getMatchingRegions()) {
    ASSERT_NE(match.producer, nullptr);
    if (match.producer->getKind() == GuardedValueFlowNode::Kind::StoreMemory)
      second_matches_store = true;
  }
  EXPECT_TRUE(second_matches_store);
}

TEST(GuardedValueFlowAdapterShape,
     FallsBackToReachableExactPointerStoresWhenLotusAALoadsAreEmpty) {
  const char *IR = R"(
    define i32 @test() {
    entry:
      store i32 7, i32* inttoptr (i64 42 to i32*)
      %v = load i32, i32* inttoptr (i64 42 to i32*)
      ret i32 %v
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);
  LoadInst *load = nullptr;
  StoreInst *store = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I))
      load = LI;
    else if (auto *SI = dyn_cast<StoreInst>(&I))
      store = SI;
  }
  ASSERT_NE(load, nullptr);
  ASSERT_NE(store, nullptr);

  auto *load_value = graph.findNode(load);
  auto *load_mem = graph.findLoadMemoryNode(load);
  ASSERT_NE(load_value, nullptr);
  ASSERT_NE(load_mem, nullptr);
  ASSERT_EQ(load_value->children().size(), 1u);
  EXPECT_EQ(load_value->children().front().target, load_mem);

  ASSERT_EQ(load_mem->getMatchingRegions().size(), 1u);
  const auto &match = load_mem->getMatchingRegions().front();
  ASSERT_NE(match.producer, nullptr);
  EXPECT_EQ(match.producer->getKind(), GuardedValueFlowNode::Kind::StoreMemory);
  EXPECT_EQ(match.producer->getDebugInstruction(), store);
  EXPECT_EQ(match.region, graph.findRegion(store->getParent()));
  EXPECT_EQ(match.provenance.getKind(), ConditionRef::Kind::None);
  EXPECT_FALSE(hasChildKind(match.producer, GuardedValueFlowNode::Kind::Unknown));
}

TEST(GuardedValueFlowAdapterShape,
     LeavesFreeVariableLoadsWithoutSyntheticProducerChain) {
  const char *IR = R"(
    define i32 @test() {
    entry:
      %v = load i32, i32* inttoptr (i64 42 to i32*)
      ret i32 %v
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);
  LoadInst *load = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      load = LI;
      break;
    }
  }
  ASSERT_NE(load, nullptr);

  auto *load_mem = graph.findLoadMemoryNode(load);
  ASSERT_NE(load_mem, nullptr);
  EXPECT_TRUE(load_mem->children().empty());
  EXPECT_TRUE(load_mem->getMatchingRegions().empty());
}

TEST(GuardedValueFlowAdapterShape,
     IgnoresFreeVariableContributorsWhenConcreteProducerExists) {
  const char *IR = R"(
    define i32 @test(i1 %cond, i32* %p, i32 %v) {
    entry:
      br i1 %cond, label %store, label %merge
    store:
      store i32 %v, i32* %p
      br label %merge
    merge:
      %loaded = load i32, i32* %p
      ret i32 %loaded
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);
  LoadInst *load = nullptr;
  StoreInst *store = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I))
      load = LI;
    else if (auto *SI = dyn_cast<StoreInst>(&I))
      store = SI;
  }
  ASSERT_NE(load, nullptr);
  ASSERT_NE(store, nullptr);

  auto *load_mem = graph.findLoadMemoryNode(load);
  ASSERT_NE(load_mem, nullptr);
  EXPECT_FALSE(load_mem->children().empty());
  EXPECT_FALSE(load_mem->getMatchingRegions().empty());

  bool saw_concrete_store = false;
  for (const auto &edge : load_mem->children()) {
    ASSERT_NE(edge.target, nullptr);
    if (edge.target->getKind() == GuardedValueFlowNode::Kind::StoreMemory &&
        edge.target->getDebugInstruction() == store) {
      saw_concrete_store = true;
      EXPECT_FALSE(hasChildKind(edge.target, GuardedValueFlowNode::Kind::Unknown));
    }
  }
  EXPECT_TRUE(saw_concrete_store);
}

TEST(GuardedValueFlowAdapterShape, ReusesStructuralMatchingRegionsForConditionalLoad) {
  const char *IR = R"(
    define i32* @test(i1 %cond, i32** %p, i32* %a, i32* %b) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      store i32* %a, i32** %p
      br label %merge
    else:
      store i32* %b, i32** %p
      br label %merge
    merge:
      %v = load i32*, i32** %p
      ret i32* %v
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);
  LoadInst *load = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      load = LI;
      break;
    }
  }
  ASSERT_NE(load, nullptr);

  auto *load_mem = graph.findLoadMemoryNode(load);
  ASSERT_NE(load_mem, nullptr);
  ASSERT_EQ(load_mem->getMatchingRegions().size(), 2u);

  SmallPtrSet<GuardedValueFlowRegionNode *, 4> matched_regions;
  for (const auto &match : load_mem->getMatchingRegions()) {
    EXPECT_NE(match.producer, nullptr);
    EXPECT_NE(match.region, nullptr);
    EXPECT_EQ(match.provenance.getKind(), ConditionRef::Kind::SemanticPathCond);
    EXPECT_FALSE(match.region->isInterfaceRegion());
    EXPECT_FALSE(match.region->isSemantic());
    EXPECT_EQ(match.region, match.producer ? match.producer->getRegion() : nullptr);
    matched_regions.insert(match.region);
  }
  EXPECT_EQ(matched_regions.size(), 2u);
}

TEST(GuardedValueFlowAdapterShape,
     KeepsCommonReturnSSAOnlyAndBuildsPerReturnPseudoInterfaces) {
  const char *IR = R"(
    define i32* @test(i1 %cond, i32** %p, i32* %a, i32* %b) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      store i32* %a, i32** %p
      ret i32* %a
    else:
      store i32* %b, i32** %p
      ret i32* %b
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  GuardedValueFlowReturnNode *common_return = nullptr;
  for (const auto &node_ptr : graph.nodes()) {
    if (node_ptr->getKind() == GuardedValueFlowNode::Kind::CommonReturn) {
      common_return = dyn_cast<GuardedValueFlowReturnNode>(node_ptr.get());
      break;
    }
  }
  ASSERT_NE(common_return, nullptr);
  ASSERT_EQ(common_return->children().size(), 2u);
  for (const auto &edge : common_return->children())
    EXPECT_NE(edge.target->getKind(), GuardedValueFlowNode::Kind::LoadMemory);

  auto *pseudo_return = graph.getPseudoReturn(0);
  ASSERT_NE(pseudo_return, nullptr);
  ASSERT_NE(pseudo_return->getLLVMValue(), nullptr);
  EXPECT_EQ(pseudo_return->getIndex(), 0u);
  ASSERT_EQ(pseudo_return->children().size(), 2u);
  EXPECT_GE(pseudo_return->getAccessPath().getDepth(), 1);
  EXPECT_EQ(graph.findInterfaceNode(pseudo_return->getLLVMValue()), pseudo_return);
  EXPECT_EQ(graph.findNode(pseudo_return->getLLVMValue()), nullptr);

  for (const auto &edge : pseudo_return->children()) {
    ASSERT_NE(edge.target, nullptr);
    EXPECT_EQ(edge.target->getKind(), GuardedValueFlowNode::Kind::LoadMemory);
    auto *site = pseudo_return->getReturnSite(edge.target);
    EXPECT_NE(site, nullptr);
    EXPECT_FALSE(containsUseSite(edge.target, site));
  }
}

TEST(GuardedValueFlowAdapterShape,
     PreservesIndexedPseudoArgumentsAndNestedAccessPaths) {
  const char *IR = R"(
    define void @deep(i32**** %root, i32* %value) {
    entry:
      %ppp = load i32***, i32**** %root
      %pp = load i32**, i32*** %ppp
      store i32* %value, i32** %pp
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("deep");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  bool saw_nested_path = false;
  for (unsigned idx = 0; idx < graph.pseudoArguments().size(); ++idx) {
    auto *pseudo_arg = graph.getPseudoArgument(idx);
    if (!pseudo_arg)
      continue;
    EXPECT_EQ(pseudo_arg->getIndex(), idx);
    EXPECT_GE(pseudo_arg->getAccessPath().getDepth(), 1);
    if (pseudo_arg->getAccessPath().getDepth() > 1)
      saw_nested_path = true;
  }

  EXPECT_TRUE(saw_nested_path);
}

TEST(GuardedValueFlowAdapterShape,
     KeepsPseudoArgumentsDistinctFromCommonArguments) {
  const char *IR = R"(
    define void @deep(i32**** %root, i32* %value) {
    entry:
      %ppp = load i32***, i32**** %root
      %pp = load i32**, i32*** %ppp
      store i32* %value, i32** %pp
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("deep");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);
  Argument *root_arg = F->arg_empty() ? nullptr : &*F->arg_begin();
  ASSERT_NE(root_arg, nullptr);
  auto *common_arg = graph.findNode(root_arg);
  ASSERT_NE(common_arg, nullptr);
  EXPECT_EQ(common_arg->getKind(), GuardedValueFlowNode::Kind::CommonArgument);

  GuardedValueFlowNode *pseudo_arg = nullptr;
  for (GuardedValueFlowNode *node : graph.pseudoArguments()) {
    if (node && node->getLLVMValue() == root_arg) {
      pseudo_arg = node;
      break;
    }
  }

  if (!pseudo_arg)
    GTEST_SKIP() << "LotusAA did not expose a pseudo input that overlaps the "
                    "direct formal in this synthetic case";
  EXPECT_EQ(pseudo_arg->getKind(), GuardedValueFlowNode::Kind::PseudoArgument);
  EXPECT_NE(pseudo_arg, common_arg);
  EXPECT_EQ(graph.findPseudoArgumentBySource(root_arg), pseudo_arg);
  EXPECT_EQ(graph.findNode(root_arg), common_arg);
}

TEST(GuardedValueFlowAdapterShape, AnchorsPseudoOutputsToEntryRegion) {
  const char *IR = R"(
    define void @store_arg(i32*** %slot, i32** %value) {
    entry:
      store i32** %value, i32*** %slot
      ret void
    }

    define void @branchy(i1 %cond, i32*** %slot, i32** %pp) {
    entry:
      br i1 %cond, label %then, label %exit
    then:
      call void @store_arg(i32*** %slot, i32** %pp)
      ret void
    exit:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("branchy");
  Function *output_callee = M->getFunction("store_arg");
  ASSERT_NE(F, nullptr);
  ASSERT_NE(output_callee, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  CallBase *output_call = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      if (CB->getCalledFunction() == output_callee)
        output_call = CB;
    }
  }
  ASSERT_NE(output_call, nullptr);

  auto *output_site = graph.findCallSite(output_call);
  ASSERT_NE(output_site, nullptr);
  ASSERT_GT(output_site->getNumPseudoOutputs(output_callee), 0u);
  auto *pseudo_output = output_site->getPseudoOutput(output_callee, 0);
  ASSERT_NE(pseudo_output, nullptr);
  EXPECT_EQ(pseudo_output->getParentBasicBlock(), &F->getEntryBlock());
  EXPECT_EQ(pseudo_output->getRegion(), graph.findRegion(&F->getEntryBlock()));

  auto *pseudo_output_mem =
      graph.findStoreMemoryNode(pseudo_output->getLLVMValue(), output_call);
  ASSERT_NE(pseudo_output_mem, nullptr);
  EXPECT_TRUE(containsChild(pseudo_output_mem, pseudo_output));
  EXPECT_EQ(pseudo_output_mem->getParentBasicBlock(), output_call->getParent());
  EXPECT_EQ(pseudo_output_mem->getRegion(), graph.findRegion(output_call->getParent()));
}

TEST(GuardedValueFlowAdapterShape,
     SkipsRecursivePseudoInterfacesAndSummariesForBackEdges) {
  const char *IR = R"(
    define i32* @recur(i32** %p, i32* %v, i1 %cond) {
    entry:
      br i1 %cond, label %rec, label %exit
    rec:
      %rv = call i32* @recur(i32** %p, i32* %v, i1 false)
      ret i32* %rv
    exit:
      store i32* %v, i32** %p
      ret i32* %v
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("recur");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);
  CallBase *recursive_call = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      recursive_call = CB;
      break;
    }
  }
  ASSERT_NE(recursive_call, nullptr);

  auto *site = graph.findCallSite(recursive_call);
  ASSERT_NE(site, nullptr);
  EXPECT_TRUE(site->isBackEdge(F));

  EXPECT_EQ(site->getNumPseudoInputs(F), 0u);
  EXPECT_EQ(site->getNumPseudoOutputs(F), 0u);

  if (auto *callee_ptg = pipeline.lotus->getPtGraph(F)) {
    for (unsigned bucket = 0; bucket < callee_ptg->getSummaryInputs().size();
         ++bucket) {
      const auto *summary_inputs = callee_ptg->getSummaryInputs()[bucket];
      if (!summary_inputs || summary_inputs->empty())
        continue;

      auto *summary_node = site->getInputSummaryNode(F, bucket);
      EXPECT_EQ(summary_node, nullptr);
    }

    for (unsigned bucket = 0; bucket < callee_ptg->getSummaryOutputs().size();
         ++bucket) {
      const auto *summary_outputs = callee_ptg->getSummaryOutputs()[bucket];
      if (!summary_outputs || summary_outputs->empty())
        continue;

      auto *summary_node = site->getOutputSummaryNode(F, bucket);
      EXPECT_EQ(summary_node, nullptr);
    }
  }
}

TEST(GuardedValueFlowAdapterShape,
     UsesCallsiteReturnSummaryNodesForFunctionSummarySentinels) {
  const char *IR = R"(
    define i32* @recur(i32** %p, i32* %v, i1 %cond) {
    entry:
      br i1 %cond, label %rec, label %exit
    rec:
      %rv = call i32* @recur(i32** %p, i32* %v, i1 false)
      ret i32* %rv
    exit:
      store i32* %v, i32** %p
      ret i32* %v
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("recur");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  ASSERT_NE(pipeline.lotus, nullptr);

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);
  auto *pta = pipeline.lotus->getPtGraph(F);
  ASSERT_NE(pta, nullptr);

  unsigned summary_bucket = 0;
  bool saw_summary_sentinel = false;
  for (unsigned bucket = 0; bucket < pta->getSummaryOutputs().size(); ++bucket) {
    const auto *summary_outputs = pta->getSummaryOutputs()[bucket];
    if (!summary_outputs)
      continue;
    for (const auto &item : *summary_outputs) {
      if (item.val == LocValue::SUMMARY_VALUE) {
        summary_bucket = bucket;
        saw_summary_sentinel = true;
        break;
      }
    }
    if (saw_summary_sentinel)
      break;
  }

  if (!saw_summary_sentinel)
    GTEST_SKIP() << "LotusAA did not materialize summary sentinels for this recursive case";

  bool saw_callsite_return_summary = false;
  for (GuardedValueFlowNode *summary_node : graph.getSummaryReturnNodes(summary_bucket)) {
    if (!summary_node)
      continue;
    for (const auto &mem_edge : summary_node->children()) {
      auto *summary_mem = mem_edge.target;
      if (!summary_mem)
        continue;
      for (const auto &producer_edge : summary_mem->children()) {
        auto *producer_mem = producer_edge.target;
        if (!producer_mem)
          continue;
        for (const auto &value_edge : producer_mem->children()) {
          auto *producer_value = value_edge.target;
          if (producer_value &&
              producer_value->getKind() ==
                  GuardedValueFlowNode::Kind::CallSiteReturnSummary) {
            saw_callsite_return_summary = true;
          }
        }
      }
    }
  }

  EXPECT_TRUE(saw_callsite_return_summary);
}

TEST(GuardedValueFlowAdapterShape,
     DoesNotMaterializeCallsiteOutputSummaryWrappers) {
  int old_ap_level = IntraLotusAAConfig::lotus_restrict_ap_level;
  int old_inline_size = IntraLotusAAConfig::lotus_restrict_inline_size;
  IntraLotusAAConfig::lotus_restrict_ap_level = 0;
  IntraLotusAAConfig::lotus_restrict_inline_size = -1;

  const char *IR = R"(
    define void @callee(i32*** %p, i32* %v) {
    entry:
      %slot = load i32**, i32*** %p
      store i32* %v, i32** %slot
      ret void
    }

    define void @test(i32*** %p, i32* %v) {
    entry:
      call void @callee(i32*** %p, i32* %v)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  IntraLotusAAConfig::lotus_restrict_ap_level = old_ap_level;
  IntraLotusAAConfig::lotus_restrict_inline_size = old_inline_size;

  Function *F = M->getFunction("test");
  Function *callee = M->getFunction("callee");
  ASSERT_NE(F, nullptr);
  ASSERT_NE(callee, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  ASSERT_NE(pipeline.lotus, nullptr);

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      call = CB;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  auto *site = graph.findCallSite(call);
  ASSERT_NE(site, nullptr);
  auto *callee_ptg = pipeline.lotus->getPtGraph(callee);
  ASSERT_NE(callee_ptg, nullptr);

  bool saw_summary_bucket = false;
  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryOutputs().size(); ++bucket) {
    const auto *summary_outputs = callee_ptg->getSummaryOutputs()[bucket];
    if (!summary_outputs || summary_outputs->empty())
      continue;
    saw_summary_bucket = true;

    auto *node = site->getOutputSummaryNode(callee, bucket);
    EXPECT_EQ(node, nullptr);
  }

  if (!saw_summary_bucket)
    GTEST_SKIP() << "LotusAA did not materialize summary output buckets for this synthetic case";
}

} // namespace
