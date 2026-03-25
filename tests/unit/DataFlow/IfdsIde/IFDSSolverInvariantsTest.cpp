#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>
#include <Dataflow/IFDS/Clients/IFDSTaintAnalysis.h>
#include <Dataflow/IFDS/Solvers/IFDSSolver.h>
#include <TestUtils/LLVMHelpers.h>
#include <unordered_set>

namespace ifds {
namespace {

struct SeedFact {
  int value = 0;

  bool operator==(const SeedFact &other) const { return value == other.value; }
  bool operator!=(const SeedFact &other) const { return !(*this == other); }
  bool operator<(const SeedFact &other) const { return value < other.value; }
};

} // namespace
} // namespace ifds

namespace std {
template <> struct hash<ifds::SeedFact> {
  size_t operator()(const ifds::SeedFact &fact) const {
    return std::hash<int>{}(fact.value);
  }
};
} // namespace std

namespace ifds {
namespace {

class SeededCalleeProblem : public IFDSProblem<SeedFact> {
public:
  SeedFact zero_fact() const override { return SeedFact{0}; }

  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ,
                      const SeedFact &fact) override {
    if (fact.value == 0 && !llvm::isa<llvm::ReturnInst>(stmt)) {
      return {};
    }
    return {fact};
  }

  FactSet call_flow(const llvm::CallBase *, const llvm::Function *,
                    const SeedFact &fact) override {
    return {fact};
  }

  FactSet return_flow(const llvm::CallBase *, const llvm::Instruction *,
                      const llvm::Instruction *, const llvm::Function *,
                      const SeedFact &exit_fact,
                      const SeedFact &) override {
    return {exit_fact};
  }

  FactSet call_to_return_flow(const llvm::CallBase *,
                              const llvm::Instruction *,
                              llvm::ArrayRef<const llvm::Function *>,
                              const SeedFact &) override {
    return {};
  }

  FactSet initial_facts(const llvm::Function *) override { return {}; }

  InitialSeeds initial_seeds(const llvm::Module &module) override {
    InitialSeeds seeds;
    auto *seeded = module.getFunction("seeded");
    if (!seeded || seeded->empty()) {
      return seeds;
    }
    seeds.add_seed(&seeded->getEntryBlock().front(), SeedFact{1});
    return seeds;
  }
};

class IFDSSolverInvariantsTest : public ::testing::Test {
protected:
  std::unique_ptr<llvm::LLVMContext> ctx = std::make_unique<llvm::LLVMContext>();

  std::unique_ptr<llvm::Module> createLoopModule() {
    return lotus::unittest::parseModuleChecked(*ctx, R"(
      declare i32 @source()
      declare void @sink(i32)

      define i32 @main() {
      entry:
        %seed = call i32 @source()
        %cond = icmp eq i32 %seed, 0
        br i1 %cond, label %body, label %exit

      body:
        call void @sink(i32 %seed)
        br label %exit

      exit:
        ret i32 0
      }
    )", "ifds_invariants");
  }

  std::unique_ptr<llvm::Module> createSeededCalleeModule() {
    return lotus::unittest::parseModuleChecked(*ctx, R"(
      define internal i32 @seeded() {
      entry:
        %tmp = add i32 1, 2
        ret i32 %tmp
      }

      define i32 @main() {
      entry:
        %call = call i32 @seeded()
        ret i32 0
      }
    )", "ifds_seeded_callee");
  }
};

TEST_F(IFDSSolverInvariantsTest, PathEdgesAreUnique) {
  auto m = createLoopModule();
  TaintAnalysis analysis;
  analysis.add_source_function("source");
  analysis.add_sink_function("sink");

  IFDSSolver<TaintAnalysis> solver(analysis);
  solver.solve(*m);

  std::vector<PathEdge<TaintFact>> path_edges;
  solver.get_path_edges(path_edges);
  std::unordered_set<PathEdge<TaintFact>, PathEdgeHash<TaintFact>> uniq;
  for (const auto &edge : path_edges) {
    uniq.insert(edge);
  }
  EXPECT_EQ(uniq.size(), path_edges.size());
}

TEST_F(IFDSSolverInvariantsTest, StepBoundIsDeterministic) {
  auto m = createLoopModule();
  TaintAnalysis analysis;
  analysis.add_source_function("source");
  analysis.add_sink_function("sink");

  IFDSSolver<TaintAnalysis> solver_a(analysis);
  solver_a.set_max_steps(4);
  solver_a.solve(*m);
  std::vector<PathEdge<TaintFact>> a_edges;
  solver_a.get_path_edges(a_edges);

  TaintAnalysis analysis_b;
  analysis_b.add_source_function("source");
  analysis_b.add_sink_function("sink");
  IFDSSolver<TaintAnalysis> solver_b(analysis_b);
  solver_b.set_max_steps(4);
  solver_b.solve(*m);
  std::vector<PathEdge<TaintFact>> b_edges;
  solver_b.get_path_edges(b_edges);

  EXPECT_TRUE(solver_a.bound_reached());
  EXPECT_TRUE(solver_b.bound_reached());
  EXPECT_EQ(a_edges.size(), b_edges.size());
}

TEST_F(IFDSSolverInvariantsTest, UnbalancedSeededReturnsDoNotSynthesizeZero) {
  auto m = createSeededCalleeModule();
  auto *mainFn = m->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const llvm::Instruction *ret_site = mainFn->getEntryBlock().getTerminator();

  SeededCalleeProblem problem;
  IFDSSolver<SeededCalleeProblem> solver(problem);
  auto config = solver.get_solver_config();
  config.set_follow_returns_past_seeds(true);
  solver.set_solver_config(config);
  solver.solve(*m);

  auto facts = solver.get_facts_at_entry(ret_site);
  EXPECT_TRUE(facts.count(SeedFact{1}) > 0);
  EXPECT_EQ(facts.count(problem.zero_fact()), 0U);
}

} // namespace
} // namespace ifds

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
