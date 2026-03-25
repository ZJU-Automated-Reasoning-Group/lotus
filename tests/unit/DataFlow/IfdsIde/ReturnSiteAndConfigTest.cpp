#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>
#include <Dataflow/IFDS/Clients/IDEConstantPropagation.h>
#include <Dataflow/IFDS/Core/IFDSFramework.h>
#include <Dataflow/IFDS/Solvers/IDESolver.h>
#include <Dataflow/IFDS/Solvers/IFDSSolver.h>
#include <TestUtils/LLVMHelpers.h>

namespace ifds {
namespace {

struct InvokeFixture {
  std::unique_ptr<llvm::LLVMContext> ctx =
      std::make_unique<llvm::LLVMContext>();
  std::unique_ptr<llvm::Module> module;
  const llvm::InvokeInst *invoke = nullptr;
  const llvm::Instruction *normal_site = nullptr;
  const llvm::Instruction *unwind_site = nullptr;
};

InvokeFixture buildInternalInvokeFixture() {
  InvokeFixture fixture;
  fixture.module = lotus::unittest::parseModule(*fixture.ctx, R"(
    declare i32 @__gxx_personality_v0(...)

    define internal i32 @returns_normally() {
    entry:
      ret i32 7
    }

    define i32 @main() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %inv = invoke i32 @returns_normally()
              to label %normal unwind label %lpad

    normal:
      ret i32 0

    lpad:
      %lp = landingpad { i8*, i32 }
              cleanup
      ret i32 1
    }
  )", "ReturnSiteAndConfigTest");

  auto *main_fn = fixture.module->getFunction("main");
  auto normal_bb = std::next(main_fn->begin());
  fixture.invoke = llvm::cast<llvm::InvokeInst>(
      lotus::unittest::findInstructionByName(*main_fn, "inv"));
  fixture.normal_site = normal_bb->getTerminator();
  fixture.unwind_site = main_fn->back().getTerminator();

  return fixture;
}

InvokeFixture buildInvokeFixture() {
  InvokeFixture fixture;
  fixture.module = lotus::unittest::parseModule(*fixture.ctx, R"(
    declare i32 @__gxx_personality_v0(...)
    declare i32 @may_throw()

    define i32 @main() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %inv = invoke i32 @may_throw()
              to label %normal unwind label %lpad

    normal:
      ret i32 0

    lpad:
      %lp = landingpad { i8*, i32 }
              cleanup
      ret i32 1
    }
  )", "ReturnSiteAndConfigTest");

  auto *main_fn = fixture.module->getFunction("main");
  auto normal_bb = std::next(main_fn->begin());
  fixture.invoke = llvm::cast<llvm::InvokeInst>(
      lotus::unittest::findInstructionByName(*main_fn, "inv"));
  fixture.normal_site = normal_bb->getTerminator();
  fixture.unwind_site = main_fn->back().getTerminator();

  return fixture;
}

struct ReturnSiteFact {
  enum Kind { Zero, Normal, Exceptional } kind = Zero;

  bool operator==(const ReturnSiteFact &other) const {
    return kind == other.kind;
  }
  bool operator!=(const ReturnSiteFact &other) const {
    return !(*this == other);
  }
  bool operator<(const ReturnSiteFact &other) const {
    return kind < other.kind;
  }

  static ReturnSiteFact zero() { return {Zero}; }
  static ReturnSiteFact normal() { return {Normal}; }
  static ReturnSiteFact exceptional() { return {Exceptional}; }
};

struct ReturnSiteValue {
  int value = -1;

  bool operator==(const ReturnSiteValue &other) const {
    return value == other.value;
  }

  static ReturnSiteValue top() { return {0}; }
  static ReturnSiteValue bottom() { return {-1}; }
  static ReturnSiteValue normal() { return {1}; }
  static ReturnSiteValue exceptional() { return {2}; }
};

} // namespace
} // namespace ifds

namespace std {
template <> struct hash<ifds::ReturnSiteFact> {
  size_t operator()(const ifds::ReturnSiteFact &fact) const {
    return std::hash<int>{}(fact.kind);
  }
};
} // namespace std

namespace ifds {
namespace {

class ReturnSiteIFDSProblem : public IFDSProblem<ReturnSiteFact> {
public:
  ReturnSiteFact zero_fact() const override { return ReturnSiteFact::zero(); }

  FactSet normal_flow(const llvm::Instruction *, const llvm::Instruction *,
                      const ReturnSiteFact &fact) override {
    return {fact};
  }

  FactSet call_flow(const llvm::CallBase *, const llvm::Function *,
                    const ReturnSiteFact &fact) override {
    return {fact};
  }

  FactSet return_flow(const llvm::CallBase *, const llvm::Instruction *,
                      const llvm::Instruction *return_site,
                      const llvm::Function *, const ReturnSiteFact &exit_fact,
                      const ReturnSiteFact &) override {
    return call_to_return_flow(nullptr, return_site, {}, exit_fact);
  }

  FactSet call_to_return_flow(const llvm::CallBase *,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *>,
                              const ReturnSiteFact &fact) override {
    FactSet out{fact};
    if (return_site && return_site->getParent()->getName() == "normal") {
      out.insert(ReturnSiteFact::normal());
    } else {
      out.insert(ReturnSiteFact::exceptional());
    }
    return out;
  }

  FactSet initial_facts(const llvm::Function *) override {
    return {ReturnSiteFact::zero()};
  }
};

class SummaryReturnSiteIFDSProblem : public IFDSProblem<ReturnSiteFact> {
public:
  ReturnSiteFact zero_fact() const override { return ReturnSiteFact::zero(); }

  FactSet normal_flow(const llvm::Instruction *, const llvm::Instruction *,
                      const ReturnSiteFact &fact) override {
    return {fact};
  }

  FactSet call_flow(const llvm::CallBase *, const llvm::Function *,
                    const ReturnSiteFact &fact) override {
    return {fact};
  }

  FactSet return_flow(const llvm::CallBase *, const llvm::Instruction *,
                      const llvm::Instruction *return_site,
                      const llvm::Function *, const ReturnSiteFact &exit_fact,
                      const ReturnSiteFact &) override {
    FactSet out{exit_fact};
    if (return_site && return_site->getParent()->getName() == "normal") {
      out.insert(ReturnSiteFact::normal());
    } else {
      out.insert(ReturnSiteFact::exceptional());
    }
    return out;
  }

  FactSet call_to_return_flow(const llvm::CallBase *, const llvm::Instruction *,
                              llvm::ArrayRef<const llvm::Function *>,
                              const ReturnSiteFact &) override {
    return {};
  }

  FactSet initial_facts(const llvm::Function *) override {
    return {ReturnSiteFact::zero()};
  }
};

class ReturnSiteIDEProblem
    : public IDEProblem<ReturnSiteFact, ReturnSiteValue> {
public:
  ReturnSiteFact zero_fact() const override { return ReturnSiteFact::zero(); }

  FactSet normal_flow(const llvm::Instruction *, const llvm::Instruction *,
                      const ReturnSiteFact &fact) override {
    return {fact};
  }

  FactSet call_flow(const llvm::CallBase *, const llvm::Function *,
                    const ReturnSiteFact &) override {
    return {};
  }

  FactSet return_flow(const llvm::CallBase *, const llvm::Instruction *,
                      const llvm::Instruction *return_site,
                      const llvm::Function *, const ReturnSiteFact &exit_fact,
                      const ReturnSiteFact &) override {
    return call_to_return_flow(nullptr, return_site, {}, exit_fact);
  }

  FactSet call_to_return_flow(const llvm::CallBase *,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *>,
                              const ReturnSiteFact &fact) override {
    FactSet out{fact};
    if (return_site && return_site->getParent()->getName() == "normal") {
      out.insert(ReturnSiteFact::normal());
    } else {
      out.insert(ReturnSiteFact::exceptional());
    }
    return out;
  }

  FactSet initial_facts(const llvm::Function *) override {
    return {ReturnSiteFact::zero()};
  }
  IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) override {
    return this->lift_ifds_initial_seeds(module, bottom_value());
  }

  EdgeFunction normal_edge_function(const llvm::Instruction *,
                                    const llvm::Instruction *,
                                    const ReturnSiteFact &,
                                    const ReturnSiteFact &) override {
    return identity();
  }

  EdgeFunction call_edge_function(const llvm::CallBase *,
                                  const llvm::Function *,
                                  const ReturnSiteFact &,
                                  const ReturnSiteFact &) override {
    return identity();
  }

  EdgeFunction return_edge_function(const llvm::CallBase *,
                                    const llvm::Function *,
                                    const llvm::Instruction *,
                                    const llvm::Instruction *return_site,
                                    const ReturnSiteFact &,
                                    const ReturnSiteFact &) override {
    return call_to_return_edge_function(nullptr, return_site, {},
                                        ReturnSiteFact::zero(),
                                        ReturnSiteFact::zero());
  }

  EdgeFunction call_to_return_edge_function(
      const llvm::CallBase *, const llvm::Instruction *return_site,
      llvm::ArrayRef<const llvm::Function *>, const ReturnSiteFact &,
      const ReturnSiteFact &) override {
    if (return_site && return_site->getParent()->getName() == "normal") {
      return [](const ReturnSiteValue &) { return ReturnSiteValue::normal(); };
    }
    return
        [](const ReturnSiteValue &) { return ReturnSiteValue::exceptional(); };
  }

  ReturnSiteValue top_value() const override { return ReturnSiteValue::top(); }
  ReturnSiteValue bottom_value() const override {
    return ReturnSiteValue::bottom();
  }
  ReturnSiteValue join(const ReturnSiteValue &lhs,
                       const ReturnSiteValue &rhs) const override {
    if (lhs.value == -1) {
      return rhs;
    }
    if (rhs.value == -1) {
      return lhs;
    }
    return lhs.value >= rhs.value ? lhs : rhs;
  }
};

TEST(ReturnSiteAwareSolverTest, IFDSDistinguishesInvokeReturnSites) {
  auto fixture = buildInvokeFixture();
  ReturnSiteIFDSProblem problem;
  IFDSSolver<ReturnSiteIFDSProblem> solver(problem);
  solver.solve(*fixture.module);

  auto normal_facts = solver.get_facts_at_entry(fixture.normal_site);
  auto unwind_facts = solver.get_facts_at_entry(fixture.unwind_site);

  EXPECT_EQ(normal_facts.count(ReturnSiteFact::normal()), 1U);
  EXPECT_EQ(normal_facts.count(ReturnSiteFact::exceptional()), 0U);
  EXPECT_EQ(unwind_facts.count(ReturnSiteFact::exceptional()), 1U);
  EXPECT_EQ(unwind_facts.count(ReturnSiteFact::normal()), 0U);
}

TEST(ReturnSiteAwareSolverTest, IDEDistinguishesInvokeReturnSites) {
  auto fixture = buildInvokeFixture();
  ReturnSiteIDEProblem problem;
  IDESolver<ReturnSiteIDEProblem> solver(problem);
  solver.solve(*fixture.module);

  auto normal_value =
      solver.get_value_at(fixture.normal_site, ReturnSiteFact::zero());
  auto unwind_value =
      solver.get_value_at(fixture.unwind_site, ReturnSiteFact::zero());

  EXPECT_EQ(normal_value.value, 1);
  EXPECT_EQ(unwind_value.value, 2);
}

TEST(ReturnSiteAwareSolverTest, SummaryEdgesRetainReturnSiteIdentity) {
  auto fixture = buildInternalInvokeFixture();
  SummaryReturnSiteIFDSProblem problem;
  IFDSSolver<SummaryReturnSiteIFDSProblem> solver(problem);
  solver.solve(*fixture.module);

  std::vector<SummaryEdge<ReturnSiteFact>> summaries;
  solver.get_summary_edges(summaries);

  bool saw_normal_site = false;
  bool saw_unwind_site = false;
  for (const auto &summary : summaries) {
    if (summary.call_site != fixture.invoke) {
      continue;
    }
    if (summary.return_site && summary.return_site->getParent()) {
      saw_normal_site |=
          summary.return_site->getParent()->getName() == "normal";
      saw_unwind_site |= summary.return_site->getParent()->getName() == "lpad";
    }
  }

  EXPECT_TRUE(saw_normal_site);
  EXPECT_TRUE(saw_unwind_site);
}

TEST(ReturnSiteAwareSolverTest, IFDSSummaryEdgesUseCallerReturnFact) {
  auto fixture = buildInternalInvokeFixture();
  SummaryReturnSiteIFDSProblem problem;
  IFDSSolver<SummaryReturnSiteIFDSProblem> solver(problem);
  solver.solve(*fixture.module);

  std::vector<SummaryEdge<ReturnSiteFact>> summaries;
  solver.get_summary_edges(summaries);

  bool saw_normal_fact = false;
  bool saw_exceptional_fact = false;
  for (const auto &summary : summaries) {
    if (summary.call_site != fixture.invoke) {
      continue;
    }
    if (summary.return_site && summary.return_site->getParent() &&
        summary.return_site->getParent()->getName() == "normal") {
      saw_normal_fact |= summary.return_fact == ReturnSiteFact::normal();
    }
    if (summary.return_site && summary.return_site->getParent() &&
        summary.return_site->getParent()->getName() == "lpad") {
      saw_exceptional_fact |=
          summary.return_fact == ReturnSiteFact::exceptional();
    }
  }

  EXPECT_TRUE(saw_normal_fact);
  EXPECT_TRUE(saw_exceptional_fact);
}

TEST(ReturnSiteAwareSolverTest, IDESummaryEdgesUseCallerReturnFact) {
  auto fixture = buildInternalInvokeFixture();
  ReturnSiteIDEProblem problem;
  IDESolver<ReturnSiteIDEProblem> solver(problem);
  auto config = solver.get_solver_config();
  config.set_record_edges(true);
  solver.set_solver_config(config);
  solver.solve(*fixture.module);

  std::vector<SummaryEdge<ReturnSiteFact>> summaries;
  solver.get_summary_edges(summaries);

  bool saw_normal_fact = false;
  bool saw_exceptional_fact = false;
  for (const auto &summary : summaries) {
    if (summary.call_site != fixture.invoke) {
      continue;
    }
    if (summary.return_site && summary.return_site->getParent() &&
        summary.return_site->getParent()->getName() == "normal") {
      saw_normal_fact |= summary.return_fact == ReturnSiteFact::normal();
    }
    if (summary.return_site && summary.return_site->getParent() &&
        summary.return_site->getParent()->getName() == "lpad") {
      saw_exceptional_fact |=
          summary.return_fact == ReturnSiteFact::exceptional();
    }
  }

  EXPECT_TRUE(saw_normal_fact);
  EXPECT_TRUE(saw_exceptional_fact);
}

TEST(IDEConfigTest, ComputeValuesFalseSkipsValueMaterialization) {
  llvm::LLVMContext ctx;
  auto module = lotus::unittest::parseModule(ctx, R"(
    define i32 @main() {
    entry:
      %x = alloca i32
      store i32 4, i32* %x
      %load = load i32, i32* %x
      ret i32 %load
    }
  )", "ReturnSiteAndConfigTest");
  auto *main_fn = module->getFunction("main");
  auto *load = lotus::unittest::findInstructionByName(*main_fn, "load");
  auto *ret = main_fn->back().getTerminator();

  IDEConstantPropagation problem;
  IDESolver<IDEConstantPropagation> solver(problem);
  auto config = solver.get_solver_config();
  config.set_compute_values(false);
  solver.set_solver_config(config);
  solver.solve(*module);

  EXPECT_TRUE(solver.get_all_values().empty());
  auto value = solver.get_value_at(ret, load);
  EXPECT_EQ(value.kind, LCPValue::Bottom);
}

TEST(IDEConfigTest, DisablingCachesPreservesResults) {
  llvm::LLVMContext ctx;
  auto module = lotus::unittest::parseModule(ctx, R"(
    define i32 @main() {
    entry:
      %x = alloca i32
      store i32 5, i32* %x
      %load = load i32, i32* %x
      %sum = add i32 %load, 7
      ret i32 %sum
    }
  )", "ReturnSiteAndConfigTest");
  auto *main_fn = module->getFunction("main");
  auto *add = lotus::unittest::findInstructionByName(*main_fn, "sum");
  auto *ret = main_fn->back().getTerminator();

  IDEConstantPropagation baseline_problem;
  IDESolver<IDEConstantPropagation> baseline(baseline_problem);
  baseline.solve(*module);

  IDEConstantPropagation uncached_problem;
  IDESolver<IDEConstantPropagation> uncached(uncached_problem);
  auto uncached_config = uncached.get_solver_config();
  uncached_config.set_enable_flow_function_caching(false);
  uncached_config.set_enable_edge_function_caching(false);
  uncached.set_solver_config(uncached_config);
  uncached.solve(*module);

  auto baseline_value = baseline.get_value_at(ret, add);
  auto uncached_value = uncached.get_value_at(ret, add);
  EXPECT_EQ(baseline_value.kind, uncached_value.kind);
  EXPECT_EQ(baseline_value.value, uncached_value.value);
}

} // namespace
} // namespace ifds

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
