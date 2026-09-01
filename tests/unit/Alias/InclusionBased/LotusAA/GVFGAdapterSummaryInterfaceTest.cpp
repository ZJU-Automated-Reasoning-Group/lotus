#include "GVFGAdapterTestSupport.h"

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
  EXPECT_TRUE(result.builder->hasGraphFor(*caller));
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
  EXPECT_TRUE(result.builder->hasGraphFor(*caller));
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
  EXPECT_TRUE(result.builder->hasGraphFor(*caller));
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
TEST(GVFGAdapter, SafeLinkUsesEffectiveChildAndPreservesInvalidTypes) {
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
  ASSERT_NE(bridge, nullptr);
  EXPECT_EQ(bridge->getKind(), GuardedValueFlowNode::Kind::Unknown);
  EXPECT_EQ(bridge->getDescription(), "adapter.coercion");
  ASSERT_EQ(aggregate_parent->children().size(), 1u);
  EXPECT_EQ(aggregate_parent->children().front().target, bridge);
  EXPECT_TRUE(bridge->containsParent(aggregate_parent));
  ASSERT_EQ(bridge->children().size(), 1u);
  EXPECT_EQ(bridge->children().front().target, scalar_child);
  EXPECT_TRUE(scalar_child->containsParent(bridge));
}
