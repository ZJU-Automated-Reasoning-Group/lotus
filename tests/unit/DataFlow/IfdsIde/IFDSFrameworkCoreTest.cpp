#include <llvm/IR/Constants.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <gtest/gtest.h>
#include <Dataflow/IFDS/Clients/IFDSTaintAnalysis.h>
#include <Dataflow/IFDS/Core/IFDSFramework.h>
#include <Dataflow/IFDS/Core/SolverGraphContext.h>
#include <TestUtils/LLVMHelpers.h>

namespace ifds {

// ============================================================================
// Simple Test Problem: Fact is just an integer
// ============================================================================

class SimpleIntFact {
public:
  int value;

  SimpleIntFact() : value(0) {}
  explicit SimpleIntFact(int v) : value(v) {}

  bool operator==(const SimpleIntFact &other) const {
    return value == other.value;
  }
  bool operator<(const SimpleIntFact &other) const {
    return value < other.value;
  }
};

class SimpleIFDSProblem : public IFDSProblem<SimpleIntFact> {
public:
  SimpleIFDSProblem() = default;

  SimpleIntFact zero_fact() const override { return SimpleIntFact(0); }

  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ,
                      const SimpleIntFact &fact) override {
    (void)stmt;
    FactSet result;
    result.insert(fact);
    return result;
  }

  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const SimpleIntFact &fact) override {
    (void)call;
    (void)callee;
    FactSet result;
    result.insert(fact);
    return result;
  }

  FactSet return_flow(const llvm::CallBase *call, const llvm::Instruction *exit_inst, const llvm::Instruction *return_site, const llvm::Function *callee,
                      const SimpleIntFact &exit_fact,
                      const SimpleIntFact &call_fact) override {
    (void)call;
    (void)callee;
    (void)call_fact;
    FactSet result;
    result.insert(exit_fact);
    return result;
  }

  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees, const SimpleIntFact &fact) override {
    (void)call;
    FactSet result;
    result.insert(fact);
    return result;
  }

  FactSet initial_facts(const llvm::Function *main) override {
    (void)main;
    FactSet result;
    result.insert(SimpleIntFact(0));
    return result;
  }

  bool is_source(const llvm::Instruction *inst) const override {
    (void)inst;
    return false;
  }
  bool is_sink(const llvm::Instruction *inst) const override {
    (void)inst;
    return false;
  }
};

// ============================================================================
// IFDS Framework Core Tests
// ============================================================================

class IFDSFrameworkCoreTest : public ::testing::Test {
protected:
  void SetUp() override { context = std::make_unique<llvm::LLVMContext>(); }

  std::unique_ptr<llvm::LLVMContext> context;

  std::unique_ptr<llvm::Module> createSimpleModule() {
    auto module = lotus::unittest::parseModuleChecked(*context, R"(
      define i32 @main() {
      entry:
        ret i32 0
      }
    )", "test_module");
    module->setModuleIdentifier("test_module");
    return module;
  }

  std::unique_ptr<llvm::Module> createModuleWithTwoBlocks() {
    auto module = lotus::unittest::parseModuleChecked(*context, R"(
      define i32 @main() {
      entry:
        br i1 true, label %then, label %merge

      then:
        br label %merge

      merge:
        %phi = phi i32 [ 1, %entry ], [ 2, %then ]
        ret i32 %phi
      }
    )", "two_block_module");
    module->setModuleIdentifier("two_block_module");
    return module;
  }

  std::unique_ptr<llvm::Module> createModuleWithCall() {
    auto module = lotus::unittest::parseModuleChecked(*context, R"(
      define i32 @callee(i32 %arg) {
      entry:
        ret i32 %arg
      }

      define i32 @main() {
      entry:
        %call = call i32 @callee(i32 42)
        ret i32 %call
      }
    )", "call_module");
    module->setModuleIdentifier("call_module");
    return module;
  }
};

// ============================================================================
// Test Cases: SimpleIFDSProblem
// ============================================================================

TEST_F(IFDSFrameworkCoreTest, SimpleProblemZeroFact) {
  SimpleIFDSProblem problem;
  auto zero = problem.zero_fact();
  EXPECT_EQ(zero.value, 0);
}

TEST_F(IFDSFrameworkCoreTest, SimpleProblemNormalFlow) {
  auto module = createSimpleModule();
  auto *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  const llvm::Instruction *inst = &*main->begin()->begin();
  SimpleIFDSProblem problem;
  SimpleIntFact fact(5);

  auto result = problem.normal_flow(inst, nullptr, fact);

  EXPECT_EQ(result.size(), 1u);
  EXPECT_TRUE(result.count(SimpleIntFact(5)));
}

TEST_F(IFDSFrameworkCoreTest, SimpleProblemInitialFacts) {
  auto module = createSimpleModule();
  auto *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  SimpleIFDSProblem problem;
  auto initial = problem.initial_facts(main);

  EXPECT_EQ(initial.size(), 1u);
  EXPECT_TRUE(initial.count(SimpleIntFact(0)));
}

TEST_F(IFDSFrameworkCoreTest, ProblemFactSetType) {
  SimpleIFDSProblem problem;
  using FactSet = decltype(problem.normal_flow(nullptr, nullptr,
                                               SimpleIntFact()));

  FactSet facts;
  facts.insert(SimpleIntFact(1));
  facts.insert(SimpleIntFact(2));

  EXPECT_EQ(facts.size(), 2u);
}

TEST_F(IFDSFrameworkCoreTest, FactSetOperations) {
  using FactSet = std::set<SimpleIntFact>;

  FactSet facts;
  facts.insert(SimpleIntFact(1));
  facts.insert(SimpleIntFact(2));
  facts.insert(SimpleIntFact(3));

  EXPECT_EQ(facts.size(), 3u);

  facts.erase(SimpleIntFact(2));
  EXPECT_EQ(facts.size(), 2u);
  EXPECT_FALSE(facts.count(SimpleIntFact(2)));
  EXPECT_TRUE(facts.count(SimpleIntFact(1)));
  EXPECT_TRUE(facts.count(SimpleIntFact(3)));
}

TEST_F(IFDSFrameworkCoreTest, ModuleCreation) {
  auto module = createSimpleModule();
  ASSERT_NE(module, nullptr);
  EXPECT_STREQ(module->getName().str().c_str(), "test_module");
}

TEST_F(IFDSFrameworkCoreTest, FunctionCreation) {
  auto module = createSimpleModule();
  auto *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  EXPECT_STREQ(main->getName().str().c_str(), "main");
}

TEST_F(IFDSFrameworkCoreTest, InstructionIteration) {
  auto module = createSimpleModule();
  auto *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  int count = 0;
  for (const auto &bb : *main) {
    for (const auto &inst : bb) {
      (void)inst;
      count++;
    }
  }
  EXPECT_EQ(count, 1);
}

TEST_F(IFDSFrameworkCoreTest, ControlFlowBranching) {
  auto module = createModuleWithTwoBlocks();
  auto *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  int bbCount = 0;
  int retCount = 0;
  int phiCount = 0;
  int branchCount = 0;

  for (const auto &bb : *main) {
    bbCount++;
    for (const auto &inst : bb) {
      if (llvm::isa<llvm::ReturnInst>(inst)) {
        retCount++;
      }
      if (llvm::isa<llvm::PHINode>(inst)) {
        phiCount++;
      }
      if (llvm::isa<llvm::BranchInst>(inst)) {
        branchCount++;
      }
    }
  }

  EXPECT_EQ(bbCount, 3);
  EXPECT_EQ(retCount, 1);
  EXPECT_EQ(phiCount, 1);
  EXPECT_GE(branchCount, 1);
}

TEST_F(IFDSFrameworkCoreTest, FunctionCall) {
  auto module = createModuleWithCall();
  auto *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  int callCount = 0;
  for (const auto &bb : *main) {
    for (const auto &inst : bb) {
      if (llvm::isa<llvm::CallInst>(inst)) {
        callCount++;
      }
    }
  }

  EXPECT_EQ(callCount, 1);
}

TEST_F(IFDSFrameworkCoreTest, CalleeFunctionExists) {
  auto module = createModuleWithCall();
  auto *callee = module->getFunction("callee");
  ASSERT_NE(callee, nullptr);
  EXPECT_STREQ(callee->getName().str().c_str(), "callee");
  EXPECT_EQ(callee->arg_size(), 1u);
}

// ============================================================================
// Test Cases: TaintFact
// ============================================================================

class TaintFactTest : public ::testing::Test {
protected:
  void SetUp() override { context = std::make_unique<llvm::LLVMContext>(); }

  std::unique_ptr<llvm::LLVMContext> context;
};

TEST_F(TaintFactTest, TaintFactZero) {
  auto zero = TaintFact::zero();
  EXPECT_TRUE(zero.is_zero());
  EXPECT_EQ(zero.get_type(), TaintFact::ZERO);
}

TEST_F(TaintFactTest, TaintFactTaintedVar) {
  auto *i32 = llvm::Type::getInt32Ty(*context);
  auto *value = llvm::ConstantInt::get(i32, 42);

  auto tainted = TaintFact::tainted_var(value);

  EXPECT_TRUE(tainted.is_tainted_var());
  EXPECT_EQ(tainted.get_type(), TaintFact::TAINTED_VAR);
  EXPECT_EQ(tainted.get_value(), value);
}

TEST_F(TaintFactTest, TaintFactTaintedMemory) {
  auto *i32 = llvm::Type::getInt32Ty(*context);
  auto *value = llvm::ConstantInt::get(i32, 42);

  auto tainted = TaintFact::tainted_memory(value);

  EXPECT_TRUE(tainted.is_tainted_memory());
  EXPECT_EQ(tainted.get_type(), TaintFact::TAINTED_MEMORY);
  EXPECT_EQ(tainted.get_memory_location(), value);
}

TEST_F(TaintFactTest, TaintFactEquality) {
  auto zero1 = TaintFact::zero();
  auto zero2 = TaintFact::zero();
  EXPECT_EQ(zero1, zero2);

  auto *i32 = llvm::Type::getInt32Ty(*context);
  auto *value = llvm::ConstantInt::get(i32, 42);
  auto tainted1 = TaintFact::tainted_var(value);
  auto tainted2 = TaintFact::tainted_var(value);

  EXPECT_EQ(tainted1, tainted2);
}

TEST_F(TaintFactTest, TaintFactInequality) {
  auto *i32 = llvm::Type::getInt32Ty(*context);
  auto *value1 = llvm::ConstantInt::get(i32, 42);
  auto *value2 = llvm::ConstantInt::get(i32, 99);

  auto tainted1 = TaintFact::tainted_var(value1);
  auto tainted2 = TaintFact::tainted_var(value2);

  EXPECT_NE(tainted1, tainted2);
}

TEST_F(TaintFactTest, TaintFactOrdering) {
  auto zero = TaintFact::zero();
  auto *i32 = llvm::Type::getInt32Ty(*context);
  auto *value = llvm::ConstantInt::get(i32, 42);
  auto taintedVar = TaintFact::tainted_var(value);
  auto taintedMem = TaintFact::tainted_memory(value);

  EXPECT_LT(zero, taintedVar);
  EXPECT_LT(taintedVar, taintedMem);
}

TEST_F(TaintFactTest, TaintFactNotZero) {
  auto *i32 = llvm::Type::getInt32Ty(*context);
  auto *value = llvm::ConstantInt::get(i32, 42);

  auto tainted = TaintFact::tainted_var(value);

  EXPECT_FALSE(tainted.is_zero());
}

TEST_F(TaintFactTest, TaintFactNotTaintedMemory) {
  auto *i32 = llvm::Type::getInt32Ty(*context);
  auto *value = llvm::ConstantInt::get(i32, 42);

  auto tainted = TaintFact::tainted_var(value);

  EXPECT_FALSE(tainted.is_tainted_memory());
}

// ============================================================================
// Test Cases: TaintAnalysis
// ============================================================================

class TaintAnalysisTest : public ::testing::Test {
protected:
  void SetUp() override { context = std::make_unique<llvm::LLVMContext>(); }

  std::unique_ptr<llvm::LLVMContext> context;
};

TEST_F(TaintAnalysisTest, TaintAnalysisCreation) {
  TaintAnalysis analysis;
  SUCCEED();
}

TEST_F(TaintAnalysisTest, TaintAnalysisConfiguration) {
  TaintAnalysis analysis;
  analysis.add_source_function("source");
  analysis.add_sink_function("sink");
  SUCCEED();
}

TEST_F(TaintAnalysisTest, TaintAnalysisMultipleSources) {
  TaintAnalysis analysis;
  analysis.add_source_function("source");
  analysis.add_source_function("read");
  analysis.add_source_function("getUserInput");
  SUCCEED();
}

TEST_F(TaintAnalysisTest, TaintAnalysisMultipleSinks) {
  TaintAnalysis analysis;
  analysis.add_sink_function("sink");
  analysis.add_sink_function("write");
  analysis.add_sink_function("log");
  SUCCEED();
}

TEST_F(TaintAnalysisTest, TaintAnalysisSourceDetection) {
  auto module = lotus::unittest::parseModule(*context, R"(
    declare i32 @source()

    define i32 @main() {
    entry:
      %source_call = call i32 @source()
      ret i32 0
    }
  )", "IFDSFrameworkCoreTest");
  auto *main = module->getFunction("main");

  TaintAnalysis analysis;
  analysis.add_source_function("source");

  bool foundSource = false;
  for (const auto &bb : *main) {
    for (const auto &inst : bb) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "source") {
          foundSource = analysis.is_source(call);
        }
      }
    }
  }
  EXPECT_TRUE(foundSource);
}

TEST_F(TaintAnalysisTest, TaintAnalysisSinkDetection) {
  auto module = lotus::unittest::parseModule(*context, R"(
    declare void @sink(i32)

    define i32 @main() {
    entry:
      call void @sink(i32 0)
      ret i32 0
    }
  )", "IFDSFrameworkCoreTest");
  auto *main = module->getFunction("main");

  TaintAnalysis analysis;
  analysis.add_sink_function("sink");

  bool foundSink = false;
  for (const auto &bb : *main) {
    for (const auto &inst : bb) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "sink") {
          foundSink = analysis.is_sink(call);
        }
      }
    }
  }
  EXPECT_TRUE(foundSink);
}

TEST_F(TaintAnalysisTest, NonSourceCallNotDetected) {
  auto module = lotus::unittest::parseModule(*context, R"(
    declare i32 @source()
    declare void @sink(i32)

    define i32 @main() {
    entry:
      %source_call = call i32 @source()
      call void @sink(i32 %source_call)
      ret i32 0
    }
  )", "IFDSFrameworkCoreTest");
  auto *main = module->getFunction("main");

  TaintAnalysis analysis;
  analysis.add_source_function("source");

  for (const auto &bb : *main) {
    for (const auto &inst : bb) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "sink") {
          EXPECT_FALSE(analysis.is_source(call));
        }
      }
    }
  }
}

TEST_F(TaintAnalysisTest, NonSinkCallNotDetected) {
  auto module = lotus::unittest::parseModule(*context, R"(
    declare i32 @source()
    declare void @sink(i32)

    define i32 @main() {
    entry:
      %source_call = call i32 @source()
      call void @sink(i32 %source_call)
      ret i32 0
    }
  )", "IFDSFrameworkCoreTest");
  auto *main = module->getFunction("main");

  TaintAnalysis analysis;
  analysis.add_sink_function("sink");

  for (const auto &bb : *main) {
    for (const auto &inst : bb) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "source") {
          EXPECT_FALSE(analysis.is_sink(call));
        }
      }
    }
  }
}

// ============================================================================
// Test Cases: IFDSProblem Interface
// ============================================================================

class IFDSProblemInterfaceTest : public ::testing::Test {
protected:
  void SetUp() override { context = std::make_unique<llvm::LLVMContext>(); }

  std::unique_ptr<llvm::LLVMContext> context;
};

class EmptySeedIFDSProblem : public SimpleIFDSProblem {
public:
  InitialSeeds initial_seeds(const llvm::Module &) override { return {}; }
};

TEST_F(IFDSProblemInterfaceTest, InitialSeedsEmptyByDefault) {
  auto module = std::make_unique<llvm::Module>("test", *context);
  SimpleIFDSProblem problem;

  auto seeds = problem.initial_seeds(*module);
  EXPECT_TRUE(seeds.empty());
}

TEST_F(IFDSProblemInterfaceTest, CustomEmptyInitialSeedsRemainEmpty) {
  auto module = lotus::unittest::parseModule(*context, R"(
    define i32 @main() {
    entry:
      ret i32 0
    }
  )", "IFDSFrameworkCoreTest");

  EmptySeedIFDSProblem problem;
  SolverGraphContext<SimpleIntFact, EmptySeedIFDSProblem> graph_context;

  graph_context.initialize(*module);
  auto seeds = graph_context.build_initial_seeds(problem, *module);

  EXPECT_TRUE(seeds.empty());
}

TEST_F(IFDSProblemInterfaceTest, AutoAddZeroDefault) {
  SimpleIFDSProblem problem;
  EXPECT_TRUE(problem.auto_add_zero());
}

TEST_F(IFDSProblemInterfaceTest, IsZeroFactDefault) {
  SimpleIFDSProblem problem;
  auto zero = problem.zero_fact();
  EXPECT_TRUE(problem.is_zero_fact(zero));

  SimpleIntFact nonZero(1);
  EXPECT_FALSE(problem.is_zero_fact(nonZero));
}

// ============================================================================
// Main function for running tests
// ============================================================================

} // namespace ifds

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
