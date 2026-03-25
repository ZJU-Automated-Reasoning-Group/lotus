#include <Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h>
#include <Dataflow/IFDS/Clients/IFDSConstAnalysis.h>
#include <Dataflow/IFDS/Clients/IFDSReachingDefinitions.h>
#include <Dataflow/IFDS/Clients/IFDSTaintAnalysis.h>
#include <Dataflow/IFDS/Solvers/IFDSSolver.h>
#include <TestUtils/LLVMHelpers.h>

#include <gtest/gtest.h>

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>

namespace {

struct InternalCallIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  llvm::Instruction *AfterCall = nullptr;
  llvm::Instruction *AllocaInst = nullptr;
  llvm::Instruction *CalleeRetInst = nullptr;
  llvm::CallInst *Call = nullptr;
};

InternalCallIR buildInternalCallIR() {
  InternalCallIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod = lotus::unittest::parseModule(*IR.Ctx, R"(
    define i8* @callee(i8* %arg) {
    entry:
      ret i8* %arg
    }

    define i32 @main() {
    entry:
      %local = alloca i8
      %call = call i8* @callee(i8* %local)
      %after_call = ptrtoint i8* %call to i64
      ret i32 0
    }
  )", "IFDSSolverAliasIntegrationTest");
  auto *Main = IR.Mod->getFunction("main");
  auto *Callee = IR.Mod->getFunction("callee");
  IR.AllocaInst = lotus::unittest::findInstructionByName(*Main, "local");
  IR.Call = llvm::cast<llvm::CallInst>(
      lotus::unittest::findInstructionByName(*Main, "call"));
  IR.AfterCall = lotus::unittest::findInstructionByName(*Main, "after_call");
  IR.CalleeRetInst = Callee != nullptr ? Callee->getEntryBlock().getTerminator()
                                       : nullptr;

  return IR;
}

struct ExternalCallIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  llvm::Instruction *AfterExtCall = nullptr;
  llvm::Instruction *GlobalStore = nullptr;
  const llvm::GlobalVariable *Global = nullptr;
};

ExternalCallIR buildExternalCallIR() {
  ExternalCallIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod = lotus::unittest::parseModule(*IR.Ctx, R"(
    @g = global i8 0

    declare void @ext(i8*)

    define i32 @main() {
    entry:
      %local = alloca i8
      store i8 1, i8* @g
      call void @ext(i8* %local)
      %after_ext_call = ptrtoint i8* %local to i64
      ret i32 0
    }
  )", "IFDSSolverAliasIntegrationTest");
  auto *Main = IR.Mod->getFunction("main");
  IR.Global = IR.Mod->getNamedGlobal("g");
  IR.AfterExtCall =
      lotus::unittest::findInstructionByName(*Main, "after_ext_call");
  for (auto &Inst : Main->getEntryBlock()) {
    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
      IR.GlobalStore = Store;
      break;
    }
  }

  return IR;
}

struct TaintAliasIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  llvm::Instruction *LoadInst = nullptr;
  llvm::CallBase *SinkCall = nullptr;
};

TaintAliasIR buildTaintAliasIR() {
  TaintAliasIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod = lotus::unittest::parseModule(*IR.Ctx, R"(
    declare i8 @source()
    declare void @sink(i8)

    define i32 @main() {
    entry:
      %p = alloca i8
      %q = getelementptr i8, i8* %p, i64 0
      %source_val = call i8 @source()
      store i8 %source_val, i8* %p
      %loaded = load i8, i8* %q
      call void @sink(i8 %loaded)
      ret i32 0
    }
  )", "IFDSSolverAliasIntegrationTest");
  auto *Main = IR.Mod->getFunction("main");
  IR.LoadInst = lotus::unittest::findInstructionByName(*Main, "loaded");
  IR.SinkCall = lotus::unittest::findCallTo(*Main, "sink");

  return IR;
}

struct ConstAliasIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  llvm::StoreInst *SecondStore = nullptr;
  llvm::Value *AliasPtr = nullptr;
};

ConstAliasIR buildConstAliasIR() {
  ConstAliasIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod = lotus::unittest::parseModule(*IR.Ctx, R"(
    define i32 @main() {
    entry:
      %p = alloca i8
      %q = getelementptr i8, i8* %p, i64 0
      store i8 1, i8* %p
      store i8 2, i8* %q
      ret i32 0
    }
  )", "IFDSSolverAliasIntegrationTest");
  auto *Main = IR.Mod->getFunction("main");
  IR.AliasPtr = lotus::unittest::findInstructionByName(*Main, "q");
  unsigned storeIndex = 0;
  for (auto &Inst : Main->getEntryBlock()) {
    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
      ++storeIndex;
      if (storeIndex == 2) {
        IR.SecondStore = Store;
        break;
      }
    }
  }

  return IR;
}

bool containsDefinition(
    const ifds::ReachingDefinitionsAnalysis::FactSet &Facts,
    const llvm::Value *Var, const llvm::Instruction *Site) {
  for (const auto &Fact : Facts) {
    if (Fact.is_definition() && Fact.get_variable() == Var &&
        Fact.get_definition_site() == Site) {
      return true;
    }
  }
  return false;
}

bool containsAnyDefinitionFor(
    const ifds::ReachingDefinitionsAnalysis::FactSet &Facts,
    const llvm::Value *Var) {
  for (const auto &Fact : Facts) {
    if (Fact.is_definition() && Fact.get_variable() == Var) {
      return true;
    }
  }
  return false;
}

bool containsTaintedVar(const ifds::TaintAnalysis::FactSet &Facts,
                        const llvm::Value *Val) {
  for (const auto &Fact : Facts) {
    if (Fact.is_tainted_var() && Fact.get_value() == Val) {
      return true;
    }
  }
  return false;
}

bool containsConstFact(const ifds::ConstAnalysis::FactSet &Facts,
                       const ifds::ConstFact &Expected) {
  return Facts.count(Expected) > 0;
}

ifds::TaintAnalysis::FactSet
solveTaintAliasScenario(const llvm::Module &Mod,
                        lotus::AliasAnalysisWrapper *AA = nullptr,
                        bool AutoInject = false) {
  ifds::TaintAnalysis Analysis;
  Analysis.add_source_function("source");
  Analysis.add_sink_function("sink");
  if (AA != nullptr) {
    Analysis.set_alias_analysis(AA);
  }

  ifds::IFDSSolver<ifds::TaintAnalysis> Solver(Analysis);
  if (AutoInject) {
    auto Config = Solver.get_solver_config();
    Config.set_auto_inject_alias_analysis(true);
    Solver.set_solver_config(Config);
  }
  Solver.solve(Mod);

  const llvm::Function *Main = Mod.getFunction("main");
  const llvm::CallBase *SinkCall = nullptr;
  const llvm::Instruction *LoadInst = nullptr;
  for (const auto &Inst : Main->getEntryBlock()) {
    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
      LoadInst = Load;
    }
    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
      if (Call->getCalledFunction() &&
          Call->getCalledFunction()->getName() == "sink") {
        SinkCall = Call;
      }
    }
  }
  EXPECT_NE(LoadInst, nullptr);
  EXPECT_NE(SinkCall, nullptr);
  return Solver.get_facts_at_entry(SinkCall);
}

ifds::ConstAnalysis::FactSet
solveConstAliasScenario(const llvm::Module &Mod, lotus::AliasAnalysisWrapper &AA,
                        const llvm::Instruction *SecondStore) {
  ifds::ConstAnalysis Analysis(&AA);
  ifds::IFDSSolver<ifds::ConstAnalysis> Solver(Analysis);
  Solver.solve(Mod);
  return Solver.get_facts_at_exit(SecondStore);
}

} // namespace

TEST(IFDSSolverAliasIntegrationTest,
     InternalCallPropagatesReturnDefAndPreservesCallerLocalDef) {
  auto IR = buildInternalCallIR();
  ifds::ReachingDefinitionsAnalysis Analysis;
  ifds::IFDSSolver<ifds::ReachingDefinitionsAnalysis> Solver(Analysis);

  Solver.solve(*IR.Mod);

  auto FactsAtAfterCall = Solver.get_facts_at_entry(IR.AfterCall);

  EXPECT_TRUE(containsDefinition(FactsAtAfterCall, IR.Call, IR.CalleeRetInst));
  EXPECT_TRUE(containsAnyDefinitionFor(FactsAtAfterCall, IR.AllocaInst));
}

TEST(IFDSSolverAliasIntegrationTest, ExternalCallKillsGlobalDefinition) {
  auto IR = buildExternalCallIR();
  ifds::ReachingDefinitionsAnalysis Analysis;
  ifds::IFDSSolver<ifds::ReachingDefinitionsAnalysis> Solver(Analysis);

  Solver.solve(*IR.Mod);

  auto FactsAfterExt = Solver.get_facts_at_entry(IR.AfterExtCall);

  EXPECT_FALSE(containsDefinition(FactsAfterExt, IR.Global, IR.GlobalStore));
  EXPECT_FALSE(containsAnyDefinitionFor(FactsAfterExt, IR.Global));
}

TEST(IFDSSolverAliasIntegrationTest,
     AutoInjectedAliasDefaultPropagatesTaintAcrossPointerAliases) {
  auto IR = buildTaintAliasIR();
  auto FactsAtSink = solveTaintAliasScenario(*IR.Mod, nullptr, true);

  EXPECT_TRUE(containsTaintedVar(FactsAtSink, IR.LoadInst));
}

TEST(IFDSSolverAliasIntegrationTest,
     ExplicitSparrowAndDyckPropagateTaintAcrossPointerAliases) {
  auto IR = buildTaintAliasIR();

  lotus::AliasAnalysisWrapper SparrowAA(*IR.Mod, lotus::AAConfig::SparrowAA_NoCtx());
  auto SparrowFacts = solveTaintAliasScenario(*IR.Mod, &SparrowAA, false);
  EXPECT_TRUE(containsTaintedVar(SparrowFacts, IR.LoadInst));

  lotus::AliasAnalysisWrapper DyckAA(*IR.Mod, lotus::AAConfig::DyckAA());
  auto DyckFacts = solveTaintAliasScenario(*IR.Mod, &DyckAA, false);
  EXPECT_TRUE(containsTaintedVar(DyckFacts, IR.LoadInst));
}

TEST(IFDSSolverAliasIntegrationTest,
     ExplicitSparrowAndDyckMarkSecondAliasWriteAsMutable) {
  auto IR = buildConstAliasIR();

  lotus::AliasAnalysisWrapper SparrowAA(*IR.Mod, lotus::AAConfig::SparrowAA_NoCtx());
  auto SparrowFacts = solveConstAliasScenario(*IR.Mod, SparrowAA, IR.SecondStore);
  EXPECT_TRUE(
      containsConstFact(SparrowFacts, ifds::ConstFact::mutable_mem(IR.AliasPtr)));

  lotus::AliasAnalysisWrapper DyckAA(*IR.Mod, lotus::AAConfig::DyckAA());
  auto DyckFacts = solveConstAliasScenario(*IR.Mod, DyckAA, IR.SecondStore);
  EXPECT_TRUE(
      containsConstFact(DyckFacts, ifds::ConstFact::mutable_mem(IR.AliasPtr)));
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
