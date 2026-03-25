#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "IR/GSA/GSA.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "IR/GVFG/GuardedValueFlowSolver.h"
#include "IR/GVFG/LotusAdapter.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <gtest/gtest.h>

#include <cctype>

using namespace llvm;
using namespace lotus::gvfg;
using namespace lotus::unittest;

namespace {

static std::string solverSymbolForNode(GuardedValueFlowNode *node) {
  std::string description = node ? node->getDescription() : "";
  for (char &ch : description) {
    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
      ch = '_';
  }
  return ("gvfg_" + std::to_string(node ? node->getNodeId() : 0) + "_" +
          description);
}

struct SummaryProducerChain {
  GuardedValueFlowNode *summary_return{nullptr};
  GuardedValueFlowNode *summary_mem{nullptr};
  GuardedValueFlowNode *producer_mem{nullptr};
  GuardedValueFlowNode *summary_sentinel{nullptr};
};

class GuardedValueFlowSolverTest : public LlvmModuleTest {
protected:
  struct BuilderPipeline {
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

  BuilderPipeline runBuilder(Module &M) {
    initializePassInfra();
    BuilderPipeline pipeline;
    pipeline.pm = std::make_unique<legacy::PassManager>();
    pipeline.builder = new GuardedValueFlowGraphBuilderPass();
    pipeline.pm->add(new gsa::ControlDependenceAnalysisPass());
    pipeline.pm->add(new gsa::GateAnalysisPass());
    pipeline.pm->add(pipeline.builder);
    pipeline.pm->run(M);
    return pipeline;
  }

  AdapterPipeline runAdapter(Module &M) {
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

TEST_F(GuardedValueFlowSolverTest, EncodesArithmeticValueFlow) {
  const char *source = R"(
    define i32 @test(i32 %a, i32 %b) {
    entry:
      %sum = add i32 %a, %b
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

  auto *sum_inst = dyn_cast<Instruction>(F->getEntryBlock().getTerminator()->getPrevNode());
  ASSERT_NE(sum_inst, nullptr);
  auto *sum_node = graph.findNode(sum_inst);
  ASSERT_NE(sum_node, nullptr);

  SMTFactory factory;
  GuardedValueFlowSolver solver(factory, module->getDataLayout());
  solver.addAll(solver.getDataDeps(sum_node));
  solver.add(solver.getOrInsertExpr(graph.findNode(F->getArg(0))) == 4);
  solver.add(solver.getOrInsertExpr(graph.findNode(F->getArg(1))) == 6);
  solver.add(solver.getOrInsertExpr(sum_node) != 10);

  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Unsat);
}

TEST_F(GuardedValueFlowSolverTest, EnforcesControlDependenciesForControlledBlock) {
  const char *source = R"(
    define i32 @test(i1 %cond, i32 %x) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      %inc = add i32 %x, 1
      ret i32 %inc
    else:
      ret i32 %x
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  auto *then_bb = F->getBasicBlockList().getNextNode(F->getEntryBlock());
  ASSERT_NE(then_bb, nullptr);
  auto *inc_inst = dyn_cast<Instruction>(then_bb->begin());
  ASSERT_NE(inc_inst, nullptr);
  auto *inc_node = graph.findNode(inc_inst);
  ASSERT_NE(inc_node, nullptr);

  SMTFactory factory;
  GuardedValueFlowSolver solver(factory, module->getDataLayout());
  solver.addAll(solver.getCtrlDeps(inc_node));
  solver.add(solver.getOrInsertExpr(graph.findNode(F->getArg(0))) == 0);

  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Unsat);
}

TEST_F(GuardedValueFlowSolverTest, EnforcesPhiIncomingGuards) {
  const char *source = R"(
    define i32 @test(i1 %cond, i32 %x) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      %inc = add i32 %x, 1
      br label %merge
    else:
      br label %merge
    merge:
      %phi = phi i32 [ %inc, %then ], [ %x, %else ]
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

  BasicBlock *then_bb = nullptr;
  Instruction *inc_inst = nullptr;
  PHINode *phi_inst = nullptr;
  for (BasicBlock &BB : *F) {
    if (BB.getName() == "then")
      then_bb = &BB;
    for (Instruction &I : BB) {
      if (BB.getName() == "then" && isa<BinaryOperator>(&I))
        inc_inst = &I;
      if (auto *phi = dyn_cast<PHINode>(&I))
        phi_inst = phi;
    }
  }
  ASSERT_NE(then_bb, nullptr);
  ASSERT_NE(inc_inst, nullptr);
  ASSERT_NE(phi_inst, nullptr);

  auto *phi_node = dyn_cast<GuardedValueFlowPhiNode>(graph.findNode(phi_inst));
  auto *inc_node = graph.findNode(inc_inst);
  ASSERT_NE(phi_node, nullptr);
  ASSERT_NE(inc_node, nullptr);

  SMTFactory factory;
  GuardedValueFlowSolver solver(factory, module->getDataLayout());
  solver.addAll(solver.getPhiGated(phi_node, inc_node, then_bb));
  solver.add(solver.getOrInsertExpr(graph.findNode(F->getArg(0))) == 0);

  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Unsat);
}

TEST_F(GuardedValueFlowSolverTest, PropagatesAdaptedLoadStoreValueFlow) {
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

  StoreInst *store_inst = nullptr;
  LoadInst *load_inst = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *store = dyn_cast<StoreInst>(&I))
      store_inst = store;
    if (auto *load = dyn_cast<LoadInst>(&I))
      load_inst = load;
  }
  ASSERT_NE(store_inst, nullptr);
  ASSERT_NE(load_inst, nullptr);

  auto *load_node = graph.findNode(load_inst);
  auto *load_mem = graph.findLoadMemoryNode(load_inst);
  auto *store_mem =
      graph.findStoreMemoryNode(store_inst->getValueOperand(), store_inst);
  ASSERT_NE(load_node, nullptr);
  ASSERT_NE(load_mem, nullptr);
  ASSERT_NE(store_mem, nullptr);
  ASSERT_NE(load_mem->getMatchingRegion(store_mem), nullptr);

  SMTFactory factory;
  GuardedValueFlowSolver solver(factory, module->getDataLayout());
  solver.addAll(solver.getDataDeps(load_node));
  solver.add(solver.getOrInsertExpr(graph.findNode(F->getArg(1))) == 7);
  solver.add(solver.getOrInsertExpr(load_node) != 7);

  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Unsat);
}

TEST_F(GuardedValueFlowSolverTest, ExcludesSummarySentinelChainsFromBaseSolver) {
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
  auto *ret_inst = cast<ReturnInst>(F->getEntryBlock().getTerminator());

  GuardedValueFlowGraph graph(F);
  SummaryProducerChain chain;
  chain.summary_return = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, Type::getInt32Ty(context),
      &graph, &F->getEntryBlock(), nullptr, ret_inst);
  chain.summary_mem = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::LoadMemory, Type::getInt32Ty(context), &graph,
      &F->getEntryBlock(), nullptr, ret_inst);
  chain.producer_mem = graph.createAnonymousStoreMemoryNode(
      Type::getInt32Ty(context), &F->getEntryBlock(), ret_inst,
      "summary.producer.mem");
  chain.summary_sentinel = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteReturnSummary,
      Type::getInt32Ty(context), &graph, &F->getEntryBlock(), ret_inst, F, 0);
  chain.summary_sentinel->setDescription("summary.output.value");

  chain.summary_return->addChild(chain.summary_mem);
  chain.summary_mem->addChild(chain.producer_mem);
  chain.summary_mem->addMatchingRegion(chain.producer_mem,
                                       graph.getAlwaysTrueRegion());
  chain.producer_mem->addChild(chain.summary_sentinel);

  SMTFactory factory;
  GuardedValueFlowSolver solver(factory, module->getDataLayout());
  SMTExprVec deps = solver.getDataDeps(chain.summary_mem);

  std::string deps_str;
  ASSERT_TRUE(deps.SMTExprVecToStream(deps_str));
  EXPECT_EQ(deps_str.find(solverSymbolForNode(chain.summary_sentinel)),
            std::string::npos);

  solver.addAll(deps);
  solver.add(solver.getOrInsertExpr(chain.summary_mem) !=
             solver.getOrInsertExpr(chain.producer_mem));
  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Sat);
}

TEST_F(GuardedValueFlowSolverTest, ExcludesSummarySentinelChainsFromDTSolver) {
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
  auto *ret_inst = cast<ReturnInst>(F->getEntryBlock().getTerminator());

  GuardedValueFlowGraph graph(F);
  SummaryProducerChain chain;
  chain.summary_return = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, Type::getInt32Ty(context),
      &graph, &F->getEntryBlock(), nullptr, ret_inst);
  chain.summary_mem = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::LoadMemory, Type::getInt32Ty(context), &graph,
      &F->getEntryBlock(), nullptr, ret_inst);
  chain.producer_mem = graph.createAnonymousStoreMemoryNode(
      Type::getInt32Ty(context), &F->getEntryBlock(), ret_inst,
      "summary.producer.mem");
  chain.summary_sentinel = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteReturnSummary,
      Type::getInt32Ty(context), &graph, &F->getEntryBlock(), ret_inst, F, 0);
  chain.summary_sentinel->setDescription("summary.output.value");

  chain.summary_return->addChild(chain.summary_mem);
  chain.summary_mem->addChild(chain.producer_mem);
  chain.summary_mem->addMatchingRegion(chain.producer_mem,
                                       graph.getAlwaysTrueRegion());
  chain.producer_mem->addChild(chain.summary_sentinel);

  DominatorTree dt(*F);
  SMTFactory factory;
  DTGuardedValueFlowSolver solver(factory, module->getDataLayout(), &dt);
  GuardedValueFlowSolver::QueryContext context{&F->getEntryBlock()};
  SMTExprVec deps = solver.getDataDeps(chain.summary_mem, &context);

  std::string deps_str;
  ASSERT_TRUE(deps.SMTExprVecToStream(deps_str));
  EXPECT_EQ(deps_str.find(solverSymbolForNode(chain.summary_sentinel)),
            std::string::npos);

  solver.addAll(deps);
  solver.add(solver.getOrInsertExpr(chain.summary_mem) !=
             solver.getOrInsertExpr(chain.producer_mem));
  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Sat);
}

} // namespace
