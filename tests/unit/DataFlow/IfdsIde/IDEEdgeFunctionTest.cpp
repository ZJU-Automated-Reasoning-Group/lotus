#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>
#include <Dataflow/IFDS/Solvers/IDESolver.h>
#include <TestUtils/LLVMHelpers.h>

namespace ifds {
namespace {

// Minimal IDEProblem implementation just to test IDEProblem::compose/identity.
class DummyIDEProblem : public IDEProblem<int, int> {
public:
  using Fact = int;
  using Value = int;

  Fact zero_fact() const override { return 0; }

  FactSet normal_flow(const llvm::Instruction * /*stmt*/,
                      const llvm::Instruction * /*succ*/,
                      const Fact &fact) override {
    return {fact};
  }
  FactSet call_flow(const llvm::CallBase * /*call*/,
                    const llvm::Function * /*callee*/,
                    const Fact &fact) override {
    return {fact};
  }
  FactSet return_flow(const llvm::CallBase * /*call*/,
                      const llvm::Instruction * /*exit_inst*/,
                      const llvm::Instruction * /*return_site*/,
                      const llvm::Function * /*callee*/, const Fact &exit_fact,
                      const Fact & /*call_fact*/) override {
    return {exit_fact};
  }
  FactSet
  call_to_return_flow(const llvm::CallBase * /*call*/,
                      const llvm::Instruction * /*return_site*/,
                      llvm::ArrayRef<const llvm::Function *> /*callees*/,
                      const Fact &fact) override {
    return {fact};
  }
  FactSet initial_facts(const llvm::Function * /*main*/) override { return {}; }
  IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) override {
    return this->lift_ifds_initial_seeds(module, bottom_value());
  }

  Value top_value() const override { return 0; }
  Value bottom_value() const override { return 0; }
  Value join(const Value & /*v1*/, const Value &v2) const override {
    return v2;
  }

  EdgeFunction normal_edge_function(const llvm::Instruction * /*stmt*/,
                                    const llvm::Instruction * /*succ*/,
                                    const Fact & /*src_fact*/,
                                    const Fact & /*tgt_fact*/) override {
    return identity();
  }
  EdgeFunction call_edge_function(const llvm::CallBase * /*call*/,
                                  const llvm::Function * /*callee*/,
                                  const Fact & /*src_fact*/,
                                  const Fact & /*tgt_fact*/) override {
    return identity();
  }
  EdgeFunction return_edge_function(const llvm::CallBase * /*call*/,
                                    const llvm::Function * /*callee*/,
                                    const llvm::Instruction * /*exit_inst*/,
                                    const llvm::Instruction * /*return_site*/,
                                    const Fact & /*exit_fact*/,
                                    const Fact & /*ret_fact*/) override {
    return identity();
  }
  EdgeFunction call_to_return_edge_function(
      const llvm::CallBase * /*call*/,
      const llvm::Instruction * /*return_site*/,
      llvm::ArrayRef<const llvm::Function *> /*callees*/,
      const Fact & /*src_fact*/, const Fact & /*tgt_fact*/) override {
    return identity();
  }
};

class SeedValueProblem : public DummyIDEProblem {
public:
  IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) override {
    IDEInitialSeeds seeds;
    auto *main = module.getFunction("main");
    auto *entry = main == nullptr || main->empty()
                      ? nullptr
                      : &main->getEntryBlock().front();
    if (entry != nullptr) {
      seeds.add_seed(entry, zero_fact(), 42);
    }
    return seeds;
  }
};

TEST(IDEEdgeFunctionTest, IdentityIsNeutral) {
  DummyIDEProblem P;
  auto id = P.identity();
  EXPECT_EQ(id(0), 0);
  EXPECT_EQ(id(7), 7);
  EXPECT_EQ(id(-3), -3);
}

TEST(IDEEdgeFunctionTest, ComposeOrderMatchesSolverUsage) {
  // In lotus, compose(f1, f2) is implemented as f1(f2(v)).
  // This mirrors the solver's "new_phi = compose(edge_fn, phi)" usage.
  DummyIDEProblem P;

  auto add2 = [](int x) { return x + 2; };
  auto mul2 = [](int x) { return x * 2; };

  // Expected: add2(mul2(3)) == 8
  auto addAfterMul = P.compose(add2, mul2);
  EXPECT_EQ(addAfterMul(3), 8);

  // Expected: mul2(add2(3)) == 10
  auto mulAfterAdd = P.compose(mul2, add2);
  EXPECT_EQ(mulAfterAdd(3), 10);

  // Recreate PhASAR's ((3 + 2) * 2) + 2 == 12 with explicit composition.
  // Step1: v -> v + 2
  // Step2: v -> v * 2
  // Step3: v -> v + 2
  auto addThenMulThenAdd = P.compose(add2, P.compose(mul2, add2));
  EXPECT_EQ(addThenMulThenAdd(3), 12);
}

TEST(IDEEdgeFunctionTest, EquivalenceIsConservativeWhenTopEqualsBottom) {
  DummyIDEProblem P;
  auto plus1 = [](int x) { return x + 1; };
  auto plus2 = [](int x) { return x + 2; };
  EXPECT_FALSE(P.edge_function_equivalent(plus1, plus2));
}

TEST(IDEEdgeFunctionTest, ExplicitSeedValuesPropagate) {
  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseModule(Ctx, R"(
    define i32 @main() {
    entry:
      ret i32 0
    }
  )", "IDEEdgeFunctionTest");
  auto *Ret = M->getFunction("main")->back().getTerminator();

  SeedValueProblem Problem;
  IDESolver<SeedValueProblem> Solver(Problem);
  Solver.solve(*M);

  EXPECT_EQ(Solver.get_value_at(Ret, 0), 42);
}

} // namespace
} // namespace ifds

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
