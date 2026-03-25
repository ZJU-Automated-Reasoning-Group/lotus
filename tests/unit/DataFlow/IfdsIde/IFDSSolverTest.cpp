#include <llvm/IR/Constants.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <gtest/gtest.h>
#include <Dataflow/IFDS/Clients/IFDSTaintAnalysis.h>
#include <Dataflow/IFDS/Core/IFDSFramework.h>
#include <Dataflow/IFDS/Solvers/IFDSSolver.h>
#include <TestUtils/LLVMHelpers.h>

using namespace ifds;
using namespace llvm;

// ============================================================================
// IFDS Solver Tests - Function Summary Coverage
// ============================================================================

class IFDSSolverTest : public ::testing::Test {
protected:
  void SetUp() override { context = std::make_unique<LLVMContext>(); }

  std::unique_ptr<LLVMContext> context;

  // Helper: Create a module with source -> sink flow (normal flow)
  std::unique_ptr<Module> createLinearFlow() {
    return lotus::unittest::parseModuleChecked(*context, R"(
      declare i32 @source()
      declare void @sink(i32)

      define i32 @main() {
      entry:
        %tainted = call i32 @source()
        call void @sink(i32 %tainted)
        ret i32 0
      }
    )", "linear_flow");
  }

  // Helper: Create module with identity function (pass-through summary)
  std::unique_ptr<Module> createIdentityFlow() {
    return lotus::unittest::parseModuleChecked(*context, R"(
      declare i32 @source()
      declare void @sink(i32)

      define internal i32 @identity(i32 %arg) {
      entry:
        ret i32 %arg
      }

      define i32 @main() {
      entry:
        %tainted = call i32 @source()
        %passed = call i32 @identity(i32 %tainted)
        call void @sink(i32 %passed)
        ret i32 0
      }
    )", "identity_flow");
  }

  // Helper: Create module with summary reuse (multiple call sites)
  std::unique_ptr<Module> createReuseSummary() {
    return lotus::unittest::parseModuleChecked(*context, R"(
      declare i32 @source()
      declare void @sink(i32)

      define internal i32 @process(i32 %arg) {
      entry:
        ret i32 %arg
      }

      define i32 @main() {
      entry:
        %tainted = call i32 @source()
        %result1 = call i32 @process(i32 %tainted)
        %result2 = call i32 @process(i32 %result1)
        call void @sink(i32 %result2)
        ret i32 0
      }
    )", "reuse_summary");
  }

  // Helper: Create module with branching (control flow split)
  std::unique_ptr<Module> createBranchFlow() {
    return lotus::unittest::parseModuleChecked(*context, R"(
      declare i32 @source()
      declare void @sink(i32)

      define i32 @main() {
      entry:
        %tainted = call i32 @source()
        %cond = icmp eq i32 %tainted, 0
        br i1 %cond, label %then, label %else

      then:
        br label %merge

      else:
        br label %merge

      merge:
        %phi = phi i32 [ %tainted, %then ], [ %tainted, %else ]
        call void @sink(i32 %phi)
        ret i32 0
      }
    )", "branch_flow");
  }
};

namespace {

class ExternalSummaryProblem : public IFDSProblem<const llvm::Value *> {
public:
  using Fact = const llvm::Value *;

  Fact zero_fact() const override { return nullptr; }

  FactSet normal_flow(const llvm::Instruction *, const llvm::Instruction *,
                      const Fact &fact) override {
    return {fact};
  }

  FactSet call_flow(const llvm::CallBase *, const llvm::Function *,
                    const Fact &) override {
    return {};
  }

  FactSet return_flow(const llvm::CallBase *, const llvm::Instruction *,
                      const llvm::Instruction *, const llvm::Function *,
                      const Fact &, const Fact &) override {
    return {};
  }

  FactSet call_to_return_flow(const llvm::CallBase *, const llvm::Instruction *,
                              llvm::ArrayRef<const llvm::Function *>,
                              const Fact &fact) override {
    return fact ? FactSet{fact} : FactSet{};
  }

  FactSet summary_flow(const llvm::CallBase *call, const llvm::Function *callee,
                       const Fact &fact) override {
    if (!is_zero_fact(fact) || call == nullptr || callee == nullptr) {
      return {};
    }
    if (callee->getName() == "recv") {
      return {call};
    }
    return {};
  }

  FactSet initial_facts(const llvm::Function *) override {
    return {zero_fact()};
  }
};

} // namespace

// ============================================================================
// Test Cases - IFDS Summary Types
// ============================================================================

TEST_F(IFDSSolverTest, BasicSolverCreation) {
  // Sanity check: solver can be created and initialized
  TaintAnalysis analysis;
  IFDSSolver<TaintAnalysis> solver(analysis);
  EXPECT_TRUE(true);
}

TEST_F(IFDSSolverTest, NormalFlow) {
  // Tests: Normal intra-procedural flow (call-to-return edge bypassing callee)
  auto module = createLinearFlow();
  TaintAnalysis analysis;
  analysis.add_source_function("source");
  analysis.add_sink_function("sink");

  IFDSSolver<TaintAnalysis> solver(analysis);
  solver.solve(*module);

  // Verify taint reaches sink from source
  auto *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  bool foundTaint = false;
  for (auto &bb : *main) {
    for (auto &inst : bb) {
      if (auto *call = dyn_cast<CallInst>(&inst)) {
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "sink") {
          auto facts = solver.get_facts_at_entry(&inst);
          foundTaint = !facts.empty();
        }
      }
    }
  }
  EXPECT_TRUE(foundTaint) << "Taint should flow from source to sink";
}

TEST_F(IFDSSolverTest, CallReturnSummary) {
  // Tests: Call flow -> Return flow (summary edge through identity function)
  auto module = createIdentityFlow();
  TaintAnalysis analysis;
  analysis.add_source_function("source");
  analysis.add_sink_function("sink");

  IFDSSolver<TaintAnalysis> solver(analysis);
  solver.solve(*module);

  // Verify summary edge was created for identity function
  std::vector<SummaryEdge<TaintFact>> summaries;
  solver.get_summary_edges(summaries);
  EXPECT_GT(summaries.size(), 0) << "Should create summary edges";
}

TEST_F(IFDSSolverTest, SummaryReuse) {
  // Tests: Same function called multiple times reuses computed summary
  auto module = createReuseSummary();
  TaintAnalysis analysis;
  analysis.add_source_function("source");
  analysis.add_sink_function("sink");

  IFDSSolver<TaintAnalysis> solver(analysis);
  solver.solve(*module);

  // Count path edges vs summary edges - summaries reduce redundant computation
  std::vector<PathEdge<TaintFact>> paths;
  std::vector<SummaryEdge<TaintFact>> summaries;
  solver.get_path_edges(paths);
  solver.get_summary_edges(summaries);

  EXPECT_GT(summaries.size(), 0) << "Should reuse summaries for repeated calls";
  EXPECT_LT(summaries.size(), paths.size())
      << "Summaries should be fewer than paths";
}

TEST_F(IFDSSolverTest, BranchMerge) {
  // Tests: Flow-sensitive merge at phi nodes (branch convergence)
  auto module = createBranchFlow();
  TaintAnalysis analysis;
  analysis.add_source_function("source");
  analysis.add_sink_function("sink");

  IFDSSolver<TaintAnalysis> solver(analysis);
  solver.solve(*module);

  // Verify taint propagates through both branches and merges
  auto *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  int phiCount = 0, sinkCallCount = 0;
  for (auto &bb : *main) {
    for (auto &inst : bb) {
      if (isa<PHINode>(&inst)) {
        auto facts = solver.get_facts_at_exit(&inst);
        if (!facts.empty())
          phiCount++;
      }
      if (auto *call = dyn_cast<CallInst>(&inst)) {
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "sink") {
          if (!solver.get_facts_at_entry(&inst).empty())
            sinkCallCount++;
        }
      }
    }
  }
  EXPECT_GT(phiCount, 0) << "Taint should reach phi node";
  EXPECT_GT(sinkCallCount, 0) << "Taint should reach sink after merge";
}

TEST_F(IFDSSolverTest, BoundedSolver) {
  // Bounded solver: set a small step limit; solver stops and returns partial
  // result.
  auto module = createLinearFlow();
  TaintAnalysis analysis;
  analysis.add_source_function("source");
  analysis.add_sink_function("sink");

  IFDSSolver<TaintAnalysis> solver(analysis);
  solver.set_max_steps(3);
  solver.solve(*module);

  EXPECT_TRUE(solver.bound_reached()) << "Step bound should have been reached";
  EXPECT_EQ(solver.get_steps_performed(), 3u)
      << "Should have performed exactly 3 steps";
  EXPECT_EQ(solver.get_max_steps(), 3u);

  // Partial result: we should have at least some path edges from the first few
  // steps
  std::vector<PathEdge<typename TaintAnalysis::FactType>> paths;
  solver.get_path_edges(paths);
  EXPECT_GT(paths.size(), 0)
      << "Bounded run should still produce partial path edges";
}

TEST_F(IFDSSolverTest, UnboundedSolver) {
  // Default (unbounded) behavior unchanged
  auto module = createLinearFlow();
  TaintAnalysis analysis;
  analysis.add_source_function("source");
  analysis.add_sink_function("sink");

  IFDSSolver<TaintAnalysis> solver(analysis);
  solver.solve(*module);

  EXPECT_FALSE(solver.bound_reached());
  EXPECT_GT(solver.get_steps_performed(), 0u);
  EXPECT_EQ(solver.get_max_steps(), 0u);
}

TEST_F(IFDSSolverTest, ExternalSummaryFlowBypassesUnknownCallee) {
  auto module = lotus::unittest::parseModuleChecked(*context, R"(
    declare i32 @recv()

    define i32 @main() {
    entry:
      %recv_value = call i32 @recv()
      ret i32 %recv_value
    }
  )", "external_summary");
  auto *recvCall =
      lotus::unittest::findInstructionByName(*module->getFunction("main"),
                                             "recv_value");
  auto *ret = module->getFunction("main")->back().getTerminator();

  ExternalSummaryProblem problem;
  IFDSSolver<ExternalSummaryProblem> solver(problem);
  solver.solve(*module);

  auto facts = solver.get_facts_at_entry(ret);
  EXPECT_EQ(facts.count(recvCall), 1u);
}

// ============================================================================
// Main function for running tests
// ============================================================================

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
