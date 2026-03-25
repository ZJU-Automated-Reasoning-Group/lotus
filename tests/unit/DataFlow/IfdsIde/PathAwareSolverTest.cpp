#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>
#include <Dataflow/IFDS/Clients/IFDSTaintAnalysis.h>
#include <Dataflow/IFDS/Solvers/IFDSSolver.h>
#include <Dataflow/IFDS/Solvers/PathAwareIFDSSolver.h>
#include <TestUtils/LLVMHelpers.h>
#include <set>

namespace ifds {
namespace {

struct IntFact {
  int value = 0;

  bool operator==(const IntFact &other) const { return value == other.value; }
  bool operator!=(const IntFact &other) const { return !(*this == other); }
  bool operator<(const IntFact &other) const { return value < other.value; }
};

class CustomZeroProblem : public IFDSProblem<IntFact> {
public:
  bool alias_configured = false;
  bool alias_initialized = false;
  lotus::AAConfig::Implementation alias_impl =
      lotus::AAConfig::Implementation::UnderApprox;

  IntFact zero_fact() const override { return IntFact{-1}; }
  bool auto_add_zero() const override { return false; }
  bool is_zero_fact(const IntFact &fact) const override {
    return fact.value == zero_fact().value;
  }
  void set_alias_analysis(lotus::AliasAnalysisWrapper *aa) override {
    IFDSProblem<IntFact>::set_alias_analysis(aa);
    alias_configured = aa != nullptr;
    alias_initialized = aa != nullptr && aa->isInitialized();
    if (aa != nullptr) {
      alias_impl = aa->getConfig().impl;
    }
  }
  FactSet normal_flow(const llvm::Instruction *,
                      const llvm::Instruction *,
                      const IntFact &fact) override {
    return {fact};
  }
  FactSet call_flow(const llvm::CallBase *, const llvm::Function *,
                    const IntFact &fact) override {
    return {fact};
  }
  FactSet return_flow(const llvm::CallBase *, const llvm::Instruction *,
                      const llvm::Instruction *, const llvm::Function *,
                      const IntFact &exit_fact,
                      const IntFact &) override {
    return {exit_fact};
  }
  FactSet call_to_return_flow(const llvm::CallBase *,
                              const llvm::Instruction *,
                              llvm::ArrayRef<const llvm::Function *>,
                              const IntFact &fact) override {
    return {fact};
  }
  FactSet initial_facts(const llvm::Function *) override {
    return {IntFact{7}};
  }
};

class SummaryProjectionProblem : public IFDSProblem<IntFact> {
public:
  IntFact zero_fact() const override { return IntFact{0}; }
  bool auto_add_zero() const override { return false; }
  bool is_zero_fact(const IntFact &fact) const override {
    return fact.value == 0;
  }
  FactSet normal_flow(const llvm::Instruction *, const llvm::Instruction *,
                      const IntFact &fact) override {
    return {fact};
  }
  FactSet call_flow(const llvm::CallBase *, const llvm::Function *,
                    const IntFact &fact) override {
    return {fact};
  }
  FactSet return_flow(const llvm::CallBase *, const llvm::Instruction *,
                      const llvm::Instruction *, const llvm::Function *,
                      const IntFact &exit_fact,
                      const IntFact &) override {
    if (exit_fact.value == 1) {
      return {IntFact{2}};
    }
    return {exit_fact};
  }
  FactSet call_to_return_flow(const llvm::CallBase *,
                              const llvm::Instruction *,
                              llvm::ArrayRef<const llvm::Function *>,
                              const IntFact &) override {
    return {};
  }
  FactSet initial_facts(const llvm::Function *) override { return {IntFact{1}}; }
};

class PathAwareSolverTest : public ::testing::Test {
protected:
  struct FlowNodes {
    const llvm::CallBase *source_call = nullptr;
    const llvm::CallBase *identity_call = nullptr;
    const llvm::CallBase *sink_call = nullptr;
    const llvm::Instruction *identity_entry = nullptr;
    const llvm::ReturnInst *identity_return = nullptr;
  };

  std::unique_ptr<llvm::LLVMContext> ctx = std::make_unique<llvm::LLVMContext>();

  std::unique_ptr<llvm::Module> createIdentityFlowModule() {
    return lotus::unittest::parseModuleChecked(*ctx, R"(
      declare i32 @source()
      declare void @sink(i32)

      define internal i32 @identity(i32 %arg) {
      entry:
        ret i32 %arg
      }

      define i32 @main() {
      entry:
        %tainted = call i32 @source()
        %pass = call i32 @identity(i32 %tainted)
        call void @sink(i32 %pass)
        ret i32 0
      }
    )", "pathaware_identity");
  }

  std::unique_ptr<llvm::Module> createMultiReturnIdentityModule() {
    return lotus::unittest::parseModuleChecked(*ctx, R"(
      declare i32 @source()
      declare void @sink(i32)

      define internal i32 @identity(i32 %arg) {
      entry:
        %cond = icmp eq i32 %arg, 0
        br i1 %cond, label %then, label %else

      then:
        ret i32 %arg

      else:
        ret i32 %arg
      }

      define i32 @main() {
      entry:
        %tainted = call i32 @source()
        %pass = call i32 @identity(i32 %tainted)
        call void @sink(i32 %pass)
        ret i32 0
      }
    )", "pathaware_multireturn");
  }

  std::unique_ptr<llvm::Module> createSimplePropagationModule() {
    return lotus::unittest::parseModuleChecked(*ctx, R"(
      define i32 @main() {
      entry:
        %tmp = add i32 1, 2
        ret i32 %tmp
      }
    )", "pathaware_simple");
  }

  std::unique_ptr<llvm::Module> createSummaryProjectionModule() {
    return lotus::unittest::parseModuleChecked(*ctx, R"(
      define internal i32 @project(i32 %arg) {
      entry:
        ret i32 %arg
      }

      define i32 @main() {
      entry:
        %projected = call i32 @project(i32 1)
        ret i32 0
      }
    )", "pathaware_summary_projection");
  }

  FlowNodes collectFlowNodes(const llvm::Module &m) {
    FlowNodes nodes;
    auto *mainFn = m.getFunction("main");
    auto *identityFn = m.getFunction("identity");
    EXPECT_NE(mainFn, nullptr);
    EXPECT_NE(identityFn, nullptr);

    if (identityFn && !identityFn->empty()) {
      nodes.identity_entry = &identityFn->getEntryBlock().front();
      nodes.identity_return =
          llvm::dyn_cast<llvm::ReturnInst>(identityFn->getEntryBlock().getTerminator());
    }

    for (const auto &bb : *mainFn) {
      for (const auto &inst : bb) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (!call || !call->getCalledFunction()) {
          continue;
        }
        auto name = call->getCalledFunction()->getName();
        if (name == "source") {
          nodes.source_call = call;
        } else if (name == "identity") {
          nodes.identity_call = call;
        } else if (name == "sink") {
          nodes.sink_call = call;
        }
      }
    }
    return nodes;
  }
};

} // namespace
} // namespace ifds

namespace std {
template <> struct hash<ifds::IntFact> {
  size_t operator()(const ifds::IntFact &fact) const {
    return std::hash<int>{}(fact.value);
  }
};
} // namespace std

namespace ifds {
namespace {

TEST_F(PathAwareSolverTest, RecordsImmediateInterproceduralEdges) {
  auto m = createIdentityFlowModule();
  auto nodes = collectFlowNodes(*m);
  TaintAnalysis taint;
  taint.add_source_function("source");
  taint.add_sink_function("sink");

  PathAwareIFDSSolver<TaintAnalysis> solver(taint);
  solver.solve(*m);

  bool has_call = false;
  bool has_return = false;
  bool has_summary = false;
  bool has_long_range_source_to_sink = false;
  for (const auto &edge : solver.get_esg().get_all_edges()) {
    if (edge.kind == ESGEdgeKind::Call &&
        edge.source.instruction == nodes.identity_call &&
        edge.target.instruction == nodes.identity_entry) {
      has_call = true;
    }
    if (edge.kind == ESGEdgeKind::Return &&
        edge.source.instruction == nodes.identity_return &&
        edge.target.instruction == nodes.sink_call) {
      has_return = true;
    }
    if (edge.kind == ESGEdgeKind::Summary &&
        edge.source.instruction == nodes.identity_call &&
        edge.target.instruction == nodes.sink_call) {
      has_summary = true;
    }
    if (edge.source.instruction == nodes.source_call &&
        edge.target.instruction == nodes.sink_call) {
      has_long_range_source_to_sink = true;
    }
  }

  EXPECT_TRUE(has_call);
  EXPECT_TRUE(has_return);
  EXPECT_TRUE(has_summary);
  EXPECT_FALSE(has_long_range_source_to_sink);
}

TEST_F(PathAwareSolverTest, FindPathsReturnsRealInterproceduralPath) {
  auto m = createIdentityFlowModule();
  auto nodes = collectFlowNodes(*m);
  TaintAnalysis taint;
  taint.add_source_function("source");
  taint.add_sink_function("sink");

  PathAwareIFDSSolver<TaintAnalysis> path_solver(taint);
  path_solver.solve(*m);

  bool checked_path = false;
  for (const auto &call_edge : path_solver.get_esg().get_all_edges()) {
    if (call_edge.kind != ESGEdgeKind::Call ||
        call_edge.source.instruction != nodes.identity_call ||
        call_edge.target.instruction != nodes.identity_entry) {
      continue;
    }
    auto paths = path_solver.find_paths(
        call_edge.source.instruction, call_edge.source.fact, nodes.sink_call,
        call_edge.source.fact, 4, 8);
    if (paths.empty()) {
      continue;
    }
    checked_path = true;
    bool saw_call = false;
    bool saw_return = false;
    for (const auto &path : paths) {
      for (const auto &edge : path) {
        saw_call |= edge.kind == ESGEdgeKind::Call;
        saw_return |= edge.kind == ESGEdgeKind::Return;
      }
    }
    EXPECT_TRUE(saw_call);
    EXPECT_TRUE(saw_return);
    break;
  }

  EXPECT_TRUE(checked_path);
}

TEST_F(PathAwareSolverTest, IFDSAndPathAwareParityAtSinkCall) {
  auto m = createIdentityFlowModule();
  auto nodes = collectFlowNodes(*m);
  TaintAnalysis taint1;
  taint1.add_source_function("source");
  taint1.add_sink_function("sink");

  TaintAnalysis taint2;
  taint2.add_source_function("source");
  taint2.add_sink_function("sink");

  IFDSSolver<TaintAnalysis> ifds_solver(taint1);
  ifds_solver.solve(*m);

  PathAwareIFDSSolver<TaintAnalysis> path_solver(taint2);
  path_solver.solve(*m);

  auto baseline = ifds_solver.get_facts_at_entry(nodes.sink_call);
  auto pathaware = path_solver.get_facts_at_entry(nodes.sink_call);
  EXPECT_EQ(baseline, pathaware);
}

TEST_F(PathAwareSolverTest, MultiReturnSummaryReuseUsesRealExitNode) {
  auto m = createMultiReturnIdentityModule();
  auto nodes = collectFlowNodes(*m);
  auto *identityFn = m->getFunction("identity");
  ASSERT_NE(identityFn, nullptr);

  const llvm::ReturnInst *thenRet = nullptr;
  const llvm::ReturnInst *elseRet = nullptr;
  for (const auto &bb : *identityFn) {
    if (bb.getName() == "then") {
      thenRet = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
    } else if (bb.getName() == "else") {
      elseRet = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
    }
  }
  ASSERT_NE(thenRet, nullptr);
  ASSERT_NE(elseRet, nullptr);

  TaintAnalysis taint;
  taint.add_source_function("source");
  taint.add_sink_function("sink");

  PathAwareIFDSSolver<TaintAnalysis> solver(taint);
  solver.solve(*m);

  bool has_precise_return = false;
  bool has_fabricated_return = false;
  for (const auto &edge : solver.get_esg().get_all_edges()) {
    if (edge.kind != ESGEdgeKind::Return ||
        edge.target.instruction != nodes.sink_call) {
      continue;
    }
    if (edge.source.instruction == thenRet || edge.source.instruction == elseRet) {
      has_precise_return = true;
    }
    if (edge.source.instruction == &identityFn->back().back() &&
        edge.source.instruction != thenRet && edge.source.instruction != elseRet) {
      has_fabricated_return = true;
    }
  }

  EXPECT_TRUE(has_precise_return);
  EXPECT_FALSE(has_fabricated_return);
}

TEST_F(PathAwareSolverTest, FactsRemainQueryableWhenComputeValuesDisabled) {
  auto m = createIdentityFlowModule();
  auto nodes = collectFlowNodes(*m);
  TaintAnalysis taint;
  taint.add_source_function("source");
  taint.add_sink_function("sink");

  PathAwareIFDSSolver<TaintAnalysis> solver(taint);
  auto config = solver.get_solver_config();
  config.set_compute_values(false);
  solver.set_solver_config(config);
  solver.solve(*m);

  auto facts = solver.get_facts_at_entry(nodes.sink_call);
  EXPECT_FALSE(facts.empty());
}

TEST_F(PathAwareSolverTest, SolvePreservesRecordEdgesConfig) {
  auto m = createIdentityFlowModule();
  TaintAnalysis taint;
  taint.add_source_function("source");
  taint.add_sink_function("sink");

  PathAwareIFDSSolver<TaintAnalysis> solver(taint);
  auto config = solver.get_solver_config();
  config.set_record_edges(false);
  solver.set_solver_config(config);

  solver.solve(*m);

  EXPECT_FALSE(solver.get_solver_config().record_edges());
}

TEST_F(PathAwareSolverTest, PathAwareWrapperForwardsCustomZeroPolicy) {
  auto m = createSimplePropagationModule();
  auto *main_fn = m->getFunction("main");
  ASSERT_NE(main_fn, nullptr);
  const llvm::Instruction *entry = &main_fn->getEntryBlock().front();

  CustomZeroProblem base_problem;
  IFDSSolver<CustomZeroProblem> ifds_solver(base_problem);
  ifds_solver.solve(*m);

  CustomZeroProblem path_problem;
  PathAwareIFDSSolver<CustomZeroProblem> path_solver(path_problem);
  path_solver.solve(*m);

  auto baseline = ifds_solver.get_facts_at_entry(entry);
  auto pathaware = path_solver.get_facts_at_entry(entry);
  EXPECT_EQ(baseline, pathaware);
  EXPECT_EQ(pathaware.count(path_problem.zero_fact()), 0U);
  EXPECT_EQ(pathaware.count(IntFact{7}), 1U);
}

TEST_F(PathAwareSolverTest, PathAwareWrapperForwardsAliasInjection) {
  auto m = createSimplePropagationModule();
  CustomZeroProblem problem;
  PathAwareIFDSSolver<CustomZeroProblem> solver(problem);
  auto config = solver.get_solver_config();
  config.set_auto_inject_alias_analysis(true);
  solver.set_solver_config(config);

  solver.solve(*m);

  EXPECT_TRUE(problem.alias_configured);
  EXPECT_TRUE(problem.alias_initialized);
  EXPECT_EQ(problem.alias_impl, lotus::AAConfig::Implementation::SparrowAA);
}

TEST_F(PathAwareSolverTest, AutoInjectedAliasIsClearedWhenIFDSSolverDies) {
  auto m = createSimplePropagationModule();
  CustomZeroProblem problem;
  {
    IFDSSolver<CustomZeroProblem> solver(problem);
    auto config = solver.get_solver_config();
    config.set_auto_inject_alias_analysis(true);
    solver.set_solver_config(config);
    solver.solve(*m);
    EXPECT_TRUE(problem.has_alias_analysis_configured());
  }

  EXPECT_FALSE(problem.has_alias_analysis_configured());
  EXPECT_FALSE(problem.alias_configured);
}

TEST_F(PathAwareSolverTest, AutoInjectedAliasIsClearedWhenPathAwareSolverDies) {
  auto m = createSimplePropagationModule();
  CustomZeroProblem problem;
  {
    PathAwareIFDSSolver<CustomZeroProblem> solver(problem);
    auto config = solver.get_solver_config();
    config.set_auto_inject_alias_analysis(true);
    solver.set_solver_config(config);
    solver.solve(*m);
    EXPECT_TRUE(problem.has_alias_analysis_configured());
  }

  EXPECT_FALSE(problem.has_alias_analysis_configured());
  EXPECT_FALSE(problem.alias_configured);
}

TEST_F(PathAwareSolverTest, SummaryEdgesTrackReturnFactsNotCalleeExitFacts) {
  auto m = createSummaryProjectionModule();
  SummaryProjectionProblem problem;
  PathAwareIFDSSolver<SummaryProjectionProblem> solver(problem);
  solver.solve(*m);

  const llvm::CallBase *project_call = nullptr;
  const llvm::Instruction *ret_site = nullptr;
  for (const auto &inst : m->getFunction("main")->getEntryBlock()) {
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
      project_call = call;
    }
    if (llvm::isa<llvm::ReturnInst>(&inst)) {
      ret_site = &inst;
    }
  }
  ASSERT_NE(project_call, nullptr);
  ASSERT_NE(ret_site, nullptr);

  bool saw_projected_fact = false;
  bool saw_exit_fact = false;
  for (const auto &edge : solver.get_esg().get_all_edges()) {
    if (edge.kind != ESGEdgeKind::Summary ||
        edge.source.instruction != project_call ||
        edge.target.instruction != ret_site) {
      continue;
    }
    saw_projected_fact |= edge.target.fact == IntFact{2};
    saw_exit_fact |= edge.target.fact == IntFact{1};
  }

  EXPECT_TRUE(saw_projected_fact);
  EXPECT_FALSE(saw_exit_fact);
}

} // namespace
} // namespace ifds

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
