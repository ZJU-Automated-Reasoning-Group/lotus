#include "TestUtils/LLVMHelpers.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

#include <algorithm>

#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#define private public
#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "IR/GSA/GSA.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "IR/GVFG/LotusAdapter.h"
#undef private

using namespace llvm;
using namespace lotus::gvfg;
using namespace lotus::unittest;

namespace {

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

static bool hasDescendantKind(GuardedValueFlowNode *node,
                              GuardedValueFlowNode::Kind kind,
                              unsigned depth = 3) {
  if (!node)
    return false;
  for (const auto &edge : node->children()) {
    if (!edge.target)
      continue;
    if (edge.target->getKind() == kind)
      return true;
    if (depth > 0 && hasDescendantKind(edge.target, kind, depth - 1))
      return true;
  }
  return false;
}

static bool containsDescendantNode(GuardedValueFlowNode *node,
                                   GuardedValueFlowNode *target,
                                   unsigned depth = 3) {
  if (!node || !target)
    return false;
  for (const auto &edge : node->children()) {
    if (!edge.target)
      continue;
    if (edge.target == target)
      return true;
    if (depth > 0 && containsDescendantNode(edge.target, target, depth - 1))
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

struct PipelineResult {
  std::unique_ptr<legacy::PassManager> pm;
  LotusAA *lotus{nullptr};
  GuardedValueFlowGraphBuilderPass *builder{nullptr};
};

PipelineResult runPipeline(Module &M) {
  initializePassInfra();

  PipelineResult result;
  result.pm = std::make_unique<legacy::PassManager>();
  result.lotus = new LotusAA();
  result.builder = new GuardedValueFlowGraphBuilderPass();

  result.pm->add(new gsa::ControlDependenceAnalysisPass());
  result.pm->add(new gsa::GateAnalysisPass());
  result.pm->add(result.lotus);
  result.pm->add(result.builder);
  result.pm->add(new LotusGuardedValueFlowAdapterPass());
  result.pm->run(M);
  return result;
}

PipelineResult runPipelineWithoutAdapter(Module &M) {
  initializePassInfra();

  PipelineResult result;
  result.pm = std::make_unique<legacy::PassManager>();
  result.lotus = new LotusAA();
  result.builder = new GuardedValueFlowGraphBuilderPass();

  result.pm->add(new gsa::ControlDependenceAnalysisPass());
  result.pm->add(new gsa::GateAnalysisPass());
  result.pm->add(result.lotus);
  result.pm->add(result.builder);
  result.pm->run(M);
  return result;
}

TEST(GVFGAdapter, MaterializesPseudoCallInterfaceNodes) {
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
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);

  Instruction *load_call_inst = nullptr;
  Instruction *store_call_inst = nullptr;
  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      Function *callee = CB->getCalledFunction();
      if (!callee)
        continue;
      if (callee->getName() == "load_arg")
        load_call_inst = &I;
      if (callee->getName() == "store_arg")
        store_call_inst = &I;
    }
  }

  auto *load_site = graph.findCallSite(load_call_inst);
  auto *store_site = graph.findCallSite(store_call_inst);
  ASSERT_NE(load_site, nullptr);
  ASSERT_NE(store_site, nullptr);

  Function *load_callee = M->getFunction("load_arg");
  Function *store_callee = M->getFunction("store_arg");
  ASSERT_NE(load_callee, nullptr);
  ASSERT_NE(store_callee, nullptr);

  auto *load_ptg = result.lotus->getPtGraph(load_callee);
  auto *store_ptg = result.lotus->getPtGraph(store_callee);
  ASSERT_NE(load_ptg, nullptr);
  ASSERT_NE(store_ptg, nullptr);

  EXPECT_EQ(load_site->getCallees().size(), 1u);
  EXPECT_EQ(store_site->getCallees().size(), 1u);
  EXPECT_EQ(load_site->getCallees().front(), load_callee);
  EXPECT_EQ(store_site->getCallees().front(), store_callee);
  EXPECT_EQ(load_site->getNumPseudoInputs(load_callee),
            static_cast<unsigned>(load_ptg->getInputs().size()));
  EXPECT_EQ(store_site->getNumPseudoOutputs(store_callee),
            store_ptg->getOutputs().empty()
                ? 0u
                : static_cast<unsigned>(store_ptg->getOutputs().size() - 1));
  EXPECT_NE(load_site->getCommonOutput(), nullptr);

  auto load_input_it = load_ptg->getInputs().begin();
  ASSERT_NE(load_input_it, load_ptg->getInputs().end());
  auto *pseudo_input = load_site->getPseudoInput(load_callee, 0);
  ASSERT_NE(pseudo_input, nullptr);
  ASSERT_NE(pseudo_input->getLLVMValue(), nullptr);
  EXPECT_EQ(pseudo_input->getIndex(), 0u);
  EXPECT_EQ(pseudo_input->getAccessPath().getBase(),
            load_input_it->second.getParentPtr());
  ASSERT_EQ(pseudo_input->getAccessPath().getDepth(), 1);
  EXPECT_EQ(pseudo_input->getAccessPath().getOffset(0),
            load_input_it->second.getOffset());
  ASSERT_EQ(pseudo_input->children().size(), 1u);
  EXPECT_EQ(pseudo_input->children().front().target->getKind(),
            GuardedValueFlowNode::Kind::LoadMemory);
  EXPECT_EQ(graph.findInterfaceNode(pseudo_input->getLLVMValue()), pseudo_input);
  EXPECT_EQ(graph.findNode(pseudo_input->getLLVMValue()), nullptr);

  ASSERT_GT(store_ptg->getOutputs().size(), 1u);
  auto *pseudo_output = store_site->getPseudoOutput(store_callee, 0);
  ASSERT_NE(pseudo_output, nullptr);
  ASSERT_NE(pseudo_output->getLLVMValue(), nullptr);
  EXPECT_EQ(pseudo_output->getIndex(), 0u);
  EXPECT_EQ(pseudo_output->getAccessPath().getBase(),
            store_ptg->getOutputs()[1]->getSymbolicInfo().getParentPtr());
  ASSERT_EQ(pseudo_output->getAccessPath().getDepth(), 1);
  EXPECT_EQ(pseudo_output->getAccessPath().getOffset(0),
            store_ptg->getOutputs()[1]->getSymbolicInfo().getOffset());
  EXPECT_TRUE(pseudo_output->children().empty());
  EXPECT_EQ(graph.findInterfaceNode(pseudo_output->getLLVMValue()), pseudo_output);
  auto *pseudo_output_mem =
      graph.findStoreMemoryNode(pseudo_output->getLLVMValue(), store_call_inst);
  ASSERT_NE(pseudo_output_mem, nullptr);
  EXPECT_TRUE(containsChild(pseudo_output_mem, pseudo_output));
}

TEST(GVFGAdapter, ConditionalLoadPreservesTwoMatchingConditions) {
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
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  LoadInst *load = nullptr;
  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        load = LI;
        break;
      }
    }
  }
  ASSERT_NE(load, nullptr);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);
  auto *load_mem = graph.findLoadMemoryNode(load);
  ASSERT_NE(load_mem, nullptr);
  EXPECT_EQ(load_mem->children().size(), 2u);
  EXPECT_EQ(load_mem->getMatchingRegions().size(), 2u);
  for (const auto &match : load_mem->getMatchingRegions()) {
    EXPECT_EQ(match.provenance.getKind(), ConditionRef::Kind::SemanticPathCond);
    EXPECT_NE(match.region, nullptr);
    EXPECT_FALSE(match.region->isInterfaceRegion());
    EXPECT_EQ(match.region, match.producer ? match.producer->getRegion() : nullptr);
  }
}

TEST(GVFGAdapter, EquivalentLoadsShareLoadMemoryNodes) {
  const char *IR = R"(
    define i32* @test(i32** %p) {
    entry:
      %v1 = load i32*, i32** %p
      %v2 = load i32*, i32** %p
      ret i32* %v2
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  LoadInst *first = nullptr;
  LoadInst *second = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (!first)
        first = LI;
      else
        second = LI;
    }
  }
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);
  auto *first_value = graph.findNode(first);
  auto *second_value = graph.findNode(second);
  auto *first_mem = graph.findLoadMemoryNode(first);
  auto *second_mem = graph.findLoadMemoryNode(second);
  ASSERT_NE(first_value, nullptr);
  ASSERT_NE(second_value, nullptr);
  ASSERT_NE(first_mem, nullptr);
  ASSERT_NE(second_mem, nullptr);
  ASSERT_EQ(first_value->children().size(), 1u);
  ASSERT_EQ(second_value->children().size(), 1u);
  EXPECT_EQ(first_value->children().front().target, first_mem);
  EXPECT_EQ(second_value->children().front().target, second_mem);
  EXPECT_EQ(first_mem, second_mem);
  EXPECT_EQ(first_mem->getMatchingRegions().size(), second_mem->getMatchingRegions().size());
}

TEST(GVFGAdapter, NonPointerLoadsAlsoReceiveMatchedStoreMemoryNodes) {
  const char *IR = R"(
    define i32 @test(i1 %cond, i32* %p) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      store i32 7, i32* %p
      br label %merge
    else:
      store i32 9, i32* %p
      br label %merge
    merge:
      %v = load i32, i32* %p
      ret i32 %v
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  LoadInst *load = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I))
      load = LI;
  }
  ASSERT_NE(load, nullptr);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);
  auto *load_value = graph.findNode(load);
  auto *load_mem = graph.findLoadMemoryNode(load);
  ASSERT_NE(load_value, nullptr);
  ASSERT_NE(load_mem, nullptr);
  ASSERT_EQ(load_value->children().size(), 1u);
  EXPECT_EQ(load_value->children().front().target, load_mem);
  EXPECT_EQ(load_mem->children().size(), 2u);
  EXPECT_EQ(load_mem->getMatchingRegions().size(), 2u);
  for (const auto &match : load_mem->getMatchingRegions()) {
    EXPECT_EQ(match.provenance.getKind(), ConditionRef::Kind::SemanticPathCond);
    EXPECT_NE(match.region, nullptr);
    EXPECT_FALSE(match.region->isInterfaceRegion());
    EXPECT_EQ(match.region, match.producer ? match.producer->getRegion() : nullptr);
  }
}

TEST(GVFGAdapter, RecordsPerCalleeCallTargetConditions) {
  const char *IR = R"(
    define void @left() {
    entry:
      ret void
    }

    define void @right() {
    entry:
      ret void
    }

    define void @test(i1 %cond) {
    entry:
      %slot = alloca void ()*
      %choice = select i1 %cond, void ()* @left, void ()* @right
      store void ()* %choice, void ()** %slot
      %fp = load void ()*, void ()** %slot
      call void %fp()
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *CB = dyn_cast<CallBase>(&I))
      call = CB;
  }
  ASSERT_NE(call, nullptr);

  auto *site = graph.findCallSite(call);
  ASSERT_NE(site, nullptr);

  auto *ptg = result.lotus->getPtGraph(F);
  ASSERT_NE(ptg, nullptr);
  auto resolved_it = ptg->getResolvedCallTargets().find(call);
  if (site->getCallees().empty() &&
      resolved_it == ptg->getResolvedCallTargets().end()) {
    GTEST_SKIP() << "LotusAA did not materialize indirect call targets for "
                    "this synthetic case";
  }

  Function *left = M->getFunction("left");
  Function *right = M->getFunction("right");
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(site->getCallees().size(), 2u);
  if (resolved_it == ptg->getResolvedCallTargets().end()) {
    EXPECT_FALSE(site->hasCalleeCondition(left));
    EXPECT_FALSE(site->hasCalleeCondition(right));
    EXPECT_EQ(site->getCalleeCondition(left).getKind(), ConditionRef::Kind::None);
    EXPECT_EQ(site->getCalleeCondition(right).getKind(), ConditionRef::Kind::None);
  } else {
    for (const auto &target : resolved_it->second) {
      if (target.second)
        EXPECT_TRUE(site->hasCalleeCondition(target.first));
      else
        EXPECT_FALSE(site->hasCalleeCondition(target.first));
      auto kind = site->getCalleeCondition(target.first).getKind();
      auto *region = site->getCalleeConditionRegion(target.first);
      if (target.second) {
        EXPECT_EQ(kind, ConditionRef::Kind::SemanticPathCond);
        ASSERT_NE(region, nullptr);
        EXPECT_FALSE(region->isInterfaceRegion());
        if (region->isSemantic()) {
          ASSERT_NE(region->getConditionNode(), nullptr);
          EXPECT_EQ(region->getConditionNode()->getRegion(), region);
          EXPECT_EQ(region->getInterfacePathCondition(), target.second);
        }
      } else {
        EXPECT_EQ(kind, ConditionRef::Kind::None);
        EXPECT_EQ(region, nullptr);
      }
    }
  }
}

TEST(GVFGAdapter, MaterializesSummaryNodesAndPseudoOutputIndices) {
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
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  IntraLotusAAConfig::lotus_restrict_ap_level = old_ap_level;
  IntraLotusAAConfig::lotus_restrict_inline_size = old_inline_size;

  Function *F = M->getFunction("test");
  Function *Callee = M->getFunction("callee");
  ASSERT_NE(F, nullptr);
  ASSERT_NE(Callee, nullptr);

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *CB = dyn_cast<CallBase>(&I))
      call = CB;
  }
  ASSERT_NE(call, nullptr);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);
  auto *site = graph.findCallSite(call);
  ASSERT_NE(site, nullptr);

  auto *callee_ptg = result.lotus->getPtGraph(Callee);
  ASSERT_NE(callee_ptg, nullptr);

  bool has_any_summary_input = false;
  for (const auto *bucket : callee_ptg->getSummaryInputs()) {
    if (bucket && !bucket->empty()) {
      has_any_summary_input = true;
      break;
    }
  }
  bool has_any_summary_output = false;
  for (const auto *bucket : callee_ptg->getSummaryOutputs()) {
    if (bucket && !bucket->empty()) {
      has_any_summary_output = true;
      break;
    }
  }
  if (!has_any_summary_input && !has_any_summary_output)
    GTEST_SKIP() << "LotusAA did not materialize summary buckets for this synthetic case";

  int inline_ap_depth = callee_ptg->getInlineApDepth();
  bool saw_input_summary = false;
  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryInputs().size(); ++bucket) {
    const auto *summary_inputs = callee_ptg->getSummaryInputs()[bucket];
    if (!summary_inputs || summary_inputs->empty())
      continue;
    saw_input_summary = true;
    auto *node = site->getInputSummaryNode(Callee, bucket);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->getKind(), GuardedValueFlowNode::Kind::CallSiteArgumentSummary);
    auto *summary = dyn_cast<GuardedValueFlowCallSummaryNode>(node);
    ASSERT_NE(summary, nullptr);
    EXPECT_EQ(summary->getSummaryIndex(), bucket);
    EXPECT_EQ(summary->getType(), PTGraph::DEFAULT_NON_POINTER_TYPE);
    if (static_cast<int>(bucket) > inline_ap_depth) {
      ASSERT_EQ(summary->children().size(), 1u);
      EXPECT_EQ(summary->children().front().target->getKind(),
                GuardedValueFlowNode::Kind::LoadMemory);
      EXPECT_EQ(summary->children().front().target->getType(),
                PTGraph::DEFAULT_NON_POINTER_TYPE);
    } else {
      EXPECT_TRUE(summary->children().empty());
    }
  }

  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryOutputs().size(); ++bucket) {
    const auto *summary_outputs = callee_ptg->getSummaryOutputs()[bucket];
    if (!summary_outputs || summary_outputs->empty())
      continue;
    auto *node = site->getOutputSummaryNode(Callee, bucket);
    EXPECT_EQ(node, nullptr);
  }

  EXPECT_EQ(saw_input_summary, has_any_summary_input);
}

TEST(GVFGAdapter, MaterializesCanonicalFunctionSummaryInterfaceOnCalleeGraphs) {
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
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  IntraLotusAAConfig::lotus_restrict_ap_level = old_ap_level;
  IntraLotusAAConfig::lotus_restrict_inline_size = old_inline_size;

  Function *callee = M->getFunction("callee");
  ASSERT_NE(callee, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*callee));
  auto *callee_ptg = result.lotus->getPtGraph(callee);
  ASSERT_NE(callee_ptg, nullptr);

  bool has_any_summary = false;
  for (const auto *bucket : callee_ptg->getSummaryInputs()) {
    if (bucket && !bucket->empty())
      has_any_summary = true;
  }
  for (const auto *bucket : callee_ptg->getSummaryOutputs()) {
    if (bucket && !bucket->empty())
      has_any_summary = true;
  }
  if (!has_any_summary)
    GTEST_SKIP() << "LotusAA did not materialize summary buckets for this synthetic case";

  GuardedValueFlowGraph &graph = result.builder->getGraph(*callee);
  BasicBlock *entry = &callee->getEntryBlock();

  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryInputs().size(); ++bucket) {
    const auto *summary_inputs = callee_ptg->getSummaryInputs()[bucket];
    if (!summary_inputs || summary_inputs->empty())
      continue;

    auto nodes = graph.getSummaryArgumentNodes(bucket);
    ASSERT_EQ(nodes.size(), summary_inputs->size());
    for (Value *source : *summary_inputs) {
      auto *source_node = graph.findPseudoArgumentBySource(source);
      if (!source_node)
        source_node = graph.findInterfaceNode(source);
      if (!source_node)
        source_node = graph.findNode(source);
      ASSERT_NE(source_node, nullptr);

      bool found_linked_summary = false;
      for (GuardedValueFlowNode *node : nodes) {
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->getKind(), GuardedValueFlowNode::Kind::SimpleOperand);
        EXPECT_EQ(node->getType(), PTGraph::DEFAULT_NON_POINTER_TYPE);
        EXPECT_EQ(node->getParentBasicBlock(), entry);
        if (containsDescendantNode(source_node, node, 2))
          found_linked_summary = true;
      }
      EXPECT_TRUE(found_linked_summary);
    }
  }

  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryOutputs().size(); ++bucket) {
    const auto *summary_outputs = callee_ptg->getSummaryOutputs()[bucket];
    if (!summary_outputs || summary_outputs->empty())
      continue;

    auto nodes = graph.getSummaryReturnNodes(bucket);
    ASSERT_EQ(nodes.size(), 1u);
    auto *summary_node = nodes.front();
    ASSERT_NE(summary_node, nullptr);
    EXPECT_EQ(summary_node->getKind(), GuardedValueFlowNode::Kind::SimpleOperand);
    EXPECT_EQ(summary_node->getType(), PTGraph::DEFAULT_NON_POINTER_TYPE);
    EXPECT_EQ(summary_node->getParentBasicBlock(), entry);
    ASSERT_EQ(summary_node->children().size(), 1u);

    auto *summary_mem = summary_node->children().front().target;
    ASSERT_NE(summary_mem, nullptr);
    EXPECT_EQ(summary_mem->getKind(), GuardedValueFlowNode::Kind::LoadMemory);
    EXPECT_EQ(summary_mem->getType(), PTGraph::DEFAULT_NON_POINTER_TYPE);
    EXPECT_EQ(summary_mem->getParentBasicBlock(), entry);

    bool expects_materialized_matches = false;
    for (const auto &item : *summary_outputs) {
      if (item.val != LocValue::FREE_VARIABLE && item.val != LocValue::NO_VALUE) {
        expects_materialized_matches = true;
        break;
      }
    }
    if (expects_materialized_matches)
      EXPECT_FALSE(summary_mem->getMatchingRegions().empty());
  }
}

TEST(GVFGAdapter, KeepsCallsiteSummaryNodesOutOfCanonicalSummaryRegistries) {
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
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  IntraLotusAAConfig::lotus_restrict_ap_level = old_ap_level;
  IntraLotusAAConfig::lotus_restrict_inline_size = old_inline_size;

  Function *caller = M->getFunction("test");
  Function *callee = M->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*caller));

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*caller)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      call = CB;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*caller);
  auto *site = graph.findCallSite(call);
  ASSERT_NE(site, nullptr);

  auto *callee_ptg = result.lotus->getPtGraph(callee);
  ASSERT_NE(callee_ptg, nullptr);

  bool saw_summary_node = false;
  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryInputs().size(); ++bucket) {
    auto *summary_node = site->getInputSummaryNode(callee, bucket);
    if (!summary_node)
      continue;
    saw_summary_node = true;
    auto nodes = graph.getSummaryArgumentNodes(bucket);
    EXPECT_EQ(std::find(nodes.begin(), nodes.end(), summary_node), nodes.end());
    for (GuardedValueFlowNode *registered : nodes)
      EXPECT_NE(registered, summary_node);
  }
  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryOutputs().size(); ++bucket) {
    auto *summary_node = site->getOutputSummaryNode(callee, bucket);
    EXPECT_EQ(summary_node, nullptr);
  }

  if (!saw_summary_node)
    GTEST_SKIP() << "LotusAA did not materialize callsite-local input summary nodes for this synthetic case";
}

TEST(GVFGAdapter, ReusesCanonicalFunctionSummaryNodesAcrossAdapterReruns) {
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
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipelineWithoutAdapter(*M);
  IntraLotusAAConfig::lotus_restrict_ap_level = old_ap_level;
  IntraLotusAAConfig::lotus_restrict_inline_size = old_inline_size;

  Function *callee = M->getFunction("callee");
  ASSERT_NE(callee, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*callee));
  auto *callee_ptg = result.lotus->getPtGraph(callee);
  ASSERT_NE(callee_ptg, nullptr);

  bool has_any_summary = false;
  for (const auto *bucket : callee_ptg->getSummaryInputs()) {
    if (bucket && !bucket->empty())
      has_any_summary = true;
  }
  for (const auto *bucket : callee_ptg->getSummaryOutputs()) {
    if (bucket && !bucket->empty())
      has_any_summary = true;
  }
  if (!has_any_summary)
    GTEST_SKIP() << "LotusAA did not materialize summary buckets for this synthetic case";

  GuardedValueFlowGraph &graph = result.builder->getGraph(*callee);
  LotusGuardedValueFlowAdapterPass adapter;
  ASSERT_TRUE(adapter.adaptFunction(graph, *callee_ptg, *result.lotus,
                                    *result.builder));

  std::map<unsigned, SmallVector<GuardedValueFlowNode *, 4>> first_summary_args;
  std::map<unsigned, GuardedValueFlowNode *> first_summary_returns;
  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryInputs().size(); ++bucket) {
    const auto *summary_inputs = callee_ptg->getSummaryInputs()[bucket];
    if (!summary_inputs || summary_inputs->empty())
      continue;
    auto nodes = graph.getSummaryArgumentNodes(bucket);
    first_summary_args[bucket].assign(nodes.begin(), nodes.end());
  }
  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryOutputs().size(); ++bucket) {
    const auto *summary_outputs = callee_ptg->getSummaryOutputs()[bucket];
    if (!summary_outputs || summary_outputs->empty())
      continue;
    auto nodes = graph.getSummaryReturnNodes(bucket);
    ASSERT_EQ(nodes.size(), 1u);
    first_summary_returns[bucket] = nodes.front();
  }

  ASSERT_TRUE(adapter.adaptFunction(graph, *callee_ptg, *result.lotus,
                                    *result.builder));

  for (const auto &entry : first_summary_args) {
    auto nodes = graph.getSummaryArgumentNodes(entry.first);
    ASSERT_EQ(nodes.size(), entry.second.size());
    for (size_t idx = 0; idx < entry.second.size(); ++idx)
      EXPECT_EQ(nodes[idx], entry.second[idx]);
  }
  for (const auto &entry : first_summary_returns) {
    auto nodes = graph.getSummaryReturnNodes(entry.first);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes.front(), entry.second);
  }
}

TEST(GVFGAdapter, CallsiteInputSummariesRespectInlineApBoundary) {
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
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  IntraLotusAAConfig::lotus_restrict_ap_level = old_ap_level;
  IntraLotusAAConfig::lotus_restrict_inline_size = old_inline_size;

  Function *caller = M->getFunction("test");
  Function *callee = M->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*caller));

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*caller)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      call = CB;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*caller);
  auto *site = graph.findCallSite(call);
  ASSERT_NE(site, nullptr);

  auto *callee_ptg = result.lotus->getPtGraph(callee);
  ASSERT_NE(callee_ptg, nullptr);
  int inline_ap_depth = callee_ptg->getInlineApDepth();

  bool saw_bucket_at_or_below_boundary = false;
  bool saw_bucket_above_boundary = false;
  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryInputs().size(); ++bucket) {
    const auto *summary_inputs = callee_ptg->getSummaryInputs()[bucket];
    if (!summary_inputs || summary_inputs->empty())
      continue;
    auto *node = site->getInputSummaryNode(callee, bucket);
    ASSERT_NE(node, nullptr);
    if (static_cast<int>(bucket) > inline_ap_depth) {
      saw_bucket_above_boundary = true;
      ASSERT_EQ(node->children().size(), 1u);
      EXPECT_EQ(node->children().front().target->getKind(),
                GuardedValueFlowNode::Kind::LoadMemory);
    } else {
      saw_bucket_at_or_below_boundary = true;
      EXPECT_TRUE(node->children().empty());
    }
  }

  if (!saw_bucket_at_or_below_boundary || !saw_bucket_above_boundary) {
    GTEST_SKIP() << "LotusAA did not materialize summary-input buckets on both "
                    "sides of the inline AP boundary for this synthetic case";
  }
}

TEST(GVFGAdapter, FailsWhenSummaryInputBindingsAreIncomplete) {
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

    define void @caller(i32*** %p, i32* %v) {
    entry:
      call void @callee(i32*** %p, i32* %v)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipelineWithoutAdapter(*M);
  IntraLotusAAConfig::lotus_restrict_ap_level = old_ap_level;
  IntraLotusAAConfig::lotus_restrict_inline_size = old_inline_size;

  Function *caller = M->getFunction("caller");
  Function *callee = M->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*caller));
  ASSERT_NE(result.lotus, nullptr);

  auto *caller_ptg = result.lotus->getPtGraph(caller);
  auto *callee_ptg = result.lotus->getPtGraph(callee);
  ASSERT_NE(caller_ptg, nullptr);
  ASSERT_NE(callee_ptg, nullptr);
  int inline_ap_depth = callee_ptg->getInlineApDepth();

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*caller)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      call = CB;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  Value *missing_binding = nullptr;
  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryInputs().size();
       ++bucket) {
    const auto *summary_inputs = callee_ptg->getSummaryInputs()[bucket];
    if (summary_inputs && !summary_inputs->empty() &&
        static_cast<int>(bucket) > inline_ap_depth) {
      missing_binding = *summary_inputs->begin();
      break;
    }
  }
  if (!missing_binding)
    GTEST_SKIP() << "LotusAA did not materialize summary input buckets beyond "
                    "the inline AP depth for this synthetic case";

  auto call_it = caller_ptg->func_arg.find(call);
  ASSERT_NE(call_it, caller_ptg->func_arg.end());
  auto callee_it = call_it->second.find(callee);
  ASSERT_NE(callee_it, call_it->second.end());
  callee_it->second.erase(missing_binding);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*caller);
  LotusGuardedValueFlowAdapterPass adapter;
  EXPECT_FALSE(adapter.adaptFunction(graph, *caller_ptg, *result.lotus,
                                     *result.builder));
  EXPECT_FALSE(result.builder->hasGraphFor(*caller));
}

TEST(GVFGAdapter, LeavesSummaryInputMemoryEmptyWhenBindingsResolveToNoValues) {
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

    define void @caller(i32*** %p, i32* %v) {
    entry:
      call void @callee(i32*** %p, i32* %v)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipelineWithoutAdapter(*M);
  IntraLotusAAConfig::lotus_restrict_ap_level = old_ap_level;
  IntraLotusAAConfig::lotus_restrict_inline_size = old_inline_size;

  Function *caller = M->getFunction("caller");
  Function *callee = M->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*caller));
  ASSERT_NE(result.lotus, nullptr);

  auto *caller_ptg = result.lotus->getPtGraph(caller);
  auto *callee_ptg = result.lotus->getPtGraph(callee);
  ASSERT_NE(caller_ptg, nullptr);
  ASSERT_NE(callee_ptg, nullptr);
  int inline_ap_depth = callee_ptg->getInlineApDepth();

  unsigned target_bucket = 0;
  bool found_bucket = false;
  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryInputs().size();
       ++bucket) {
    auto *summary_inputs = callee_ptg->getSummaryInputs()[bucket];
    if (!summary_inputs || summary_inputs->empty() ||
        static_cast<int>(bucket) <= inline_ap_depth)
      continue;
    target_bucket = bucket;
    found_bucket = true;
    break;
  }
  if (!found_bucket)
    GTEST_SKIP() << "LotusAA did not materialize summary input buckets beyond "
                    "the inline AP depth for this synthetic case";

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*caller)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      call = CB;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  auto call_it = caller_ptg->func_arg.find(call);
  ASSERT_NE(call_it, caller_ptg->func_arg.end());
  auto callee_it = call_it->second.find(callee);
  ASSERT_NE(callee_it, call_it->second.end());
  auto *summary_inputs = callee_ptg->getSummaryInputs()[target_bucket];
  ASSERT_NE(summary_inputs, nullptr);
  for (Value *summary_input : *summary_inputs)
    callee_it->second[summary_input].clear();

  GuardedValueFlowGraph &graph = result.builder->getGraph(*caller);
  LotusGuardedValueFlowAdapterPass adapter;
  ASSERT_TRUE(adapter.adaptFunction(graph, *caller_ptg, *result.lotus,
                                    *result.builder));
  ASSERT_TRUE(result.builder->hasGraphFor(*caller));

  auto *site = graph.findCallSite(call);
  ASSERT_NE(site, nullptr);
  auto *node = site->getInputSummaryNode(callee, target_bucket);
  ASSERT_NE(node, nullptr);
  ASSERT_EQ(node->children().size(), 1u);
  auto *summary_mem = node->children().front().target;
  ASSERT_NE(summary_mem, nullptr);
  EXPECT_EQ(summary_mem->getKind(), GuardedValueFlowNode::Kind::LoadMemory);
  EXPECT_TRUE(summary_mem->children().empty());
  EXPECT_TRUE(summary_mem->getMatchingRegions().empty());
}

TEST(GVFGAdapter, DoesNotCreateFunctionSummaryNodesForClearedSummaryBuckets) {
  int old_ap_level = IntraLotusAAConfig::lotus_restrict_ap_level;
  int old_inline_size = IntraLotusAAConfig::lotus_restrict_inline_size;
  IntraLotusAAConfig::lotus_restrict_ap_level = 0;
  IntraLotusAAConfig::lotus_restrict_inline_size = -1;

  const char *IR = R"(
    define void @test(i32*** %slot, i32** %value) {
    entry:
      store i32** %value, i32*** %slot
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipelineWithoutAdapter(*M);
  IntraLotusAAConfig::lotus_restrict_ap_level = old_ap_level;
  IntraLotusAAConfig::lotus_restrict_inline_size = old_inline_size;

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*F));
  ASSERT_NE(result.lotus, nullptr);

  auto *ptg = result.lotus->getPtGraph(F);
  ASSERT_NE(ptg, nullptr);

  unsigned target_bucket = 0;
  bool found_bucket = false;
  for (unsigned bucket = 0; bucket < ptg->getSummaryOutputs().size(); ++bucket) {
    auto *summary_outputs = ptg->getSummaryOutputs()[bucket];
    if (!summary_outputs || summary_outputs->empty())
      continue;
    summary_outputs->clear();
    target_bucket = bucket;
    found_bucket = true;
    break;
  }
  if (!found_bucket)
    GTEST_SKIP() << "LotusAA did not materialize summary output buckets for this synthetic case";

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);
  LotusGuardedValueFlowAdapterPass adapter;
  ASSERT_TRUE(adapter.adaptFunction(graph, *ptg, *result.lotus, *result.builder));
  ASSERT_TRUE(result.builder->hasGraphFor(*F));

  auto nodes = graph.getSummaryReturnNodes(target_bucket);
  EXPECT_TRUE(nodes.empty());
}

TEST(GVFGAdapter, FailsWhenPseudoInputBindingIsRemoved) {
  const char *IR = R"(
    define void @callee(i32** %p, i32** %q) {
    entry:
      %a = load i32*, i32** %p
      %b = load i32*, i32** %q
      ret void
    }

    define void @caller(i32** %p, i32** %q) {
    entry:
      call void @callee(i32** %p, i32** %q)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipelineWithoutAdapter(*M);
  Function *caller = M->getFunction("caller");
  Function *callee = M->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*caller));
  ASSERT_NE(result.lotus, nullptr);

  auto *callee_ptg = result.lotus->getPtGraph(callee);
  auto *caller_ptg = result.lotus->getPtGraph(caller);
  ASSERT_NE(callee_ptg, nullptr);
  ASSERT_NE(caller_ptg, nullptr);
  ASSERT_EQ(callee_ptg->getInputs().size(), 2u);

  SmallVector<Value *, 2> pseudo_inputs;
  for (const auto &input_item : callee_ptg->getInputs())
    pseudo_inputs.push_back(input_item.first);
  ASSERT_EQ(pseudo_inputs.size(), 2u);
  ASSERT_EQ(callee_ptg->getPseudoInputIndex(pseudo_inputs[0]), 0);
  ASSERT_EQ(callee_ptg->getPseudoInputIndex(pseudo_inputs[1]), 1);

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*caller)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      call = CB;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  auto call_it = caller_ptg->func_arg.find(call);
  ASSERT_NE(call_it, caller_ptg->func_arg.end());
  auto callee_it = call_it->second.find(callee);
  ASSERT_NE(callee_it, call_it->second.end());
  callee_it->second.erase(pseudo_inputs[0]);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*caller);
  LotusGuardedValueFlowAdapterPass adapter;
  EXPECT_FALSE(adapter.adaptFunction(graph, *caller_ptg, *result.lotus,
                                     *result.builder));
  EXPECT_FALSE(result.builder->hasGraphFor(*caller));
}

TEST(GVFGAdapter, FailsWhenPseudoOutputBindingIsRemoved) {
  const char *IR = R"(
    define void @callee(i32** %p, i32* %v) {
    entry:
      store i32* %v, i32** %p
      ret void
    }

    define void @caller(i32** %p, i32* %v) {
    entry:
      call void @callee(i32** %p, i32* %v)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipelineWithoutAdapter(*M);
  Function *caller = M->getFunction("caller");
  Function *callee = M->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*caller));
  ASSERT_NE(result.lotus, nullptr);

  auto *caller_ptg = result.lotus->getPtGraph(caller);
  auto *callee_ptg = result.lotus->getPtGraph(callee);
  ASSERT_NE(caller_ptg, nullptr);
  ASSERT_NE(callee_ptg, nullptr);
  ASSERT_GT(callee_ptg->getOutputs().size(), 1u);

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*caller)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      call = CB;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  auto call_it = caller_ptg->func_ret.find(call);
  ASSERT_NE(call_it, caller_ptg->func_ret.end());
  auto callee_it = call_it->second.find(callee);
  ASSERT_NE(callee_it, call_it->second.end());
  ASSERT_GT(callee_it->second.size(), 1u);
  callee_it->second[1] = nullptr;

  GuardedValueFlowGraph &graph = result.builder->getGraph(*caller);
  LotusGuardedValueFlowAdapterPass adapter;
  EXPECT_FALSE(adapter.adaptFunction(graph, *caller_ptg, *result.lotus,
                                     *result.builder));
  EXPECT_FALSE(result.builder->hasGraphFor(*caller));
}

TEST(GVFGAdapter, FailsWhenPseudoInputIndicesAreMalformed) {
  const char *IR = R"(
    define void @callee(i32** %p, i32** %q) {
    entry:
      %a = load i32*, i32** %p
      %b = load i32*, i32** %q
      ret void
    }

    define void @caller(i32** %p, i32** %q) {
    entry:
      call void @callee(i32** %p, i32** %q)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipelineWithoutAdapter(*M);
  Function *caller = M->getFunction("caller");
  Function *callee = M->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*caller));
  ASSERT_NE(result.lotus, nullptr);

  auto *callee_ptg = result.lotus->getPtGraph(callee);
  auto *caller_ptg = result.lotus->getPtGraph(caller);
  ASSERT_NE(callee_ptg, nullptr);
  ASSERT_NE(caller_ptg, nullptr);
  ASSERT_EQ(callee_ptg->getInputs().size(), 2u);

  SmallVector<Value *, 2> pseudo_inputs;
  for (const auto &input_item : callee_ptg->getInputs())
    pseudo_inputs.push_back(input_item.first);
  ASSERT_EQ(pseudo_inputs.size(), 2u);
  callee_ptg->pseudo_input_indices[pseudo_inputs[0]] = 0;
  callee_ptg->pseudo_input_indices[pseudo_inputs[1]] = 0;

  GuardedValueFlowGraph &graph = result.builder->getGraph(*caller);
  LotusGuardedValueFlowAdapterPass adapter;
  EXPECT_FALSE(adapter.adaptFunction(graph, *caller_ptg, *result.lotus,
                                     *result.builder));
  EXPECT_FALSE(result.builder->hasGraphFor(*caller));
}

TEST(GVFGAdapter, KeepsPseudoArgumentsDistinctWhenInterfaceOverlapsFormal) {
  const char *IR = R"(
    @g = global i8 0

    define void @test(i32** %p) {
    entry:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipelineWithoutAdapter(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*F));
  ASSERT_NE(result.lotus, nullptr);

  auto *ptg = result.lotus->getPtGraph(F);
  ASSERT_NE(ptg, nullptr);
  Argument *formal = F->arg_empty() ? nullptr : &*F->arg_begin();
  GlobalVariable *global = M->getNamedGlobal("g");
  ASSERT_NE(formal, nullptr);
  ASSERT_NE(global, nullptr);
  ptg->getInputs()[formal] = IntraLotusAA::AccessPath(global, 0);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);
  auto *common_arg = graph.findNode(formal);
  ASSERT_NE(common_arg, nullptr);
  EXPECT_EQ(common_arg->getKind(), GuardedValueFlowNode::Kind::CommonArgument);

  LotusGuardedValueFlowAdapterPass adapter;
  ASSERT_TRUE(adapter.adaptFunction(graph, *ptg, *result.lotus, *result.builder));

  GuardedValueFlowNode *pseudo_arg = nullptr;
  for (GuardedValueFlowNode *node : graph.pseudoArguments()) {
    if (node && node->getLLVMValue() == formal) {
      pseudo_arg = node;
      break;
    }
  }

  ASSERT_NE(pseudo_arg, nullptr);
  EXPECT_EQ(pseudo_arg->getKind(), GuardedValueFlowNode::Kind::PseudoArgument);
  EXPECT_NE(pseudo_arg, common_arg);
  EXPECT_EQ(graph.findPseudoArgumentBySource(formal), pseudo_arg);
  EXPECT_EQ(graph.findNode(formal), common_arg);
}

TEST(GVFGAdapter, ReusesPseudoArgumentNodesAsInterfaceMemoryProducers) {
  const char *IR = R"(
    define void @test(i32**** %root, i32* %value) {
    entry:
      %ppp = load i32***, i32**** %root
      %pp = load i32**, i32*** %ppp
      store i32* %value, i32** %pp
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);

  auto *pseudo_arg = graph.findPseudoArgumentBySource(F->getArg(0));
  if (!pseudo_arg)
    GTEST_SKIP() << "LotusAA did not materialize a pseudo argument for this synthetic case";

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
  bool saw_pseudo_argument_producer = false;
  for (const auto &match : load_mem->getMatchingRegions()) {
    if (!match.producer)
      continue;
    if (hasDescendantKind(match.producer,
                          GuardedValueFlowNode::Kind::PseudoArgument)) {
      saw_pseudo_argument_producer = true;
      EXPECT_EQ(match.region, match.producer->getRegion());
    }
  }
  EXPECT_TRUE(saw_pseudo_argument_producer);
}

TEST(GVFGAdapter, UsesSummaryReturnProducerForUnprovenancedSummaryValue) {
  const char *IR = R"(
    define void @test(i32*** %slot, i32** %value) {
    entry:
      store i32** %value, i32*** %slot
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipelineWithoutAdapter(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*F));
  ASSERT_NE(result.lotus, nullptr);

  auto *ptg = result.lotus->getPtGraph(F);
  ASSERT_NE(ptg, nullptr);
  ASSERT_GT(ptg->getOutputs().size(), 1u);

  ReturnInst *ret = cast<ReturnInst>(F->getEntryBlock().getTerminator());
  auto &ret_vals = ptg->getOutputs()[1]->getVal()[ret];
  ret_vals.clear();
  ret_vals.emplace_back(nullptr, nullptr, LocValue::SUMMARY_VALUE, 1.0f);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);
  LotusGuardedValueFlowAdapterPass adapter;
  ASSERT_TRUE(adapter.adaptFunction(graph, *ptg, *result.lotus, *result.builder));

  auto *pseudo_return = graph.getPseudoReturn(0);
  ASSERT_NE(pseudo_return, nullptr);
  ASSERT_NE(pseudo_return->getLLVMValue(), nullptr);
  EXPECT_EQ(graph.findInterfaceNode(pseudo_return->getLLVMValue()), pseudo_return);
  EXPECT_EQ(graph.findNode(pseudo_return->getLLVMValue()), nullptr);
  ASSERT_FALSE(pseudo_return->children().empty());

  auto *summary_mem = pseudo_return->children().front().target;
  ASSERT_NE(summary_mem, nullptr);
  ASSERT_FALSE(summary_mem->getMatchingRegions().empty());
  const auto &match = summary_mem->getMatchingRegions().front();
  ASSERT_NE(match.producer, nullptr);
  EXPECT_TRUE(
      hasChildKind(match.producer, GuardedValueFlowNode::Kind::CallSiteReturnSummary));
  EXPECT_FALSE(hasChildKind(match.producer, GuardedValueFlowNode::Kind::Unknown));
}

TEST(GVFGAdapter, SafeLinkUsesEffectiveChildAndDropsInvalidTypes) {
  const char *IR = R"(
    define void @test() {
    entry:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  GuardedValueFlowGraph graph(F);
  BasicBlock *entry = &F->getEntryBlock();
  auto *ret = cast<ReturnInst>(entry->getTerminator());

  auto *load_mem = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::LoadMemory, Type::getInt64Ty(Ctx), &graph,
      entry, nullptr, ret);
  auto *store_mem = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::StoreMemory, Type::getInt32Ty(Ctx), &graph,
      entry, nullptr, ret);
  auto *cond = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, Type::getInt1Ty(Ctx), &graph,
      entry, nullptr, ret);
  auto *region =
      graph.findOrCreateUnitRegion(cond, true, entry, ConditionRef::none());
  ASSERT_NE(region, nullptr);

  auto *linked = LotusGuardedValueFlowAdapterPass::safeLink(
      graph, load_mem, store_mem, 0.75f, ConditionRef::none());
  ASSERT_NE(linked, nullptr);
  EXPECT_NE(linked, store_mem);
  EXPECT_EQ(linked->getKind(), GuardedValueFlowNode::Kind::CastOpcode);
  ASSERT_EQ(load_mem->children().size(), 1u);
  EXPECT_EQ(load_mem->children().front().target, linked);
  EXPECT_FLOAT_EQ(load_mem->children().front().confidence, 0.75f);
  ASSERT_EQ(linked->children().size(), 1u);
  EXPECT_EQ(linked->children().front().target, store_mem);
  EXPECT_TRUE(linked->containsParent(load_mem));
  EXPECT_TRUE(store_mem->containsParent(linked));
  EXPECT_FALSE(store_mem->containsParent(load_mem));

  load_mem->addMatchingRegion(linked, region, ConditionRef::none());
  EXPECT_EQ(load_mem->getMatchingRegion(linked), region);
  EXPECT_EQ(load_mem->getMatchingRegion(store_mem), nullptr);

  auto *aggregate_parent = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand,
      StructType::get(Ctx, {Type::getInt32Ty(Ctx), Type::getInt32Ty(Ctx)}),
      &graph,
      entry, nullptr, ret);
  auto *scalar_child = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, Type::getInt32Ty(Ctx), &graph,
      entry, nullptr, ret);
  auto *bridge = LotusGuardedValueFlowAdapterPass::safeLink(graph, aggregate_parent,
                                                            scalar_child);
  EXPECT_EQ(bridge, nullptr);
  EXPECT_TRUE(aggregate_parent->children().empty());
  EXPECT_TRUE(scalar_child->parents().empty());
}

} // namespace
