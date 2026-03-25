#include <Dataflow/IFDS/Clients/IFDSConstAnalysis.h>
#include <Dataflow/IFDS/Clients/IFDSReachingDefinitions.h>
#include <TestUtils/LLVMHelpers.h>

#include <gtest/gtest.h>

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>

namespace {

struct IFDSFlowFixtureIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  const llvm::Function *Callee = nullptr;
  const llvm::CallBase*Call = nullptr;
  const llvm::CallBase*ExtCall = nullptr;
  const llvm::Instruction *CallReturnSite = nullptr;
  const llvm::Instruction *ExtCallReturnSite = nullptr;
  llvm::Argument *Formal = nullptr;
  const llvm::Value *Actual = nullptr;
  const llvm::Instruction *CalleeEntryInst = nullptr;
  const llvm::GlobalVariable *Global = nullptr;
};

IFDSFlowFixtureIR buildIFDSFlowFixture() {
  IFDSFlowFixtureIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod = lotus::unittest::parseModule(*IR.Ctx, R"(
    @g = global i8 0

    define i8* @callee(i8* %formal) {
    entry:
      ret i8* %formal
    }

    declare void @ext(i8*)

    define i32 @main() {
    entry:
      %actual = alloca i8
      %call = call i8* @callee(i8* %actual)
      call void @ext(i8* %actual)
      ret i32 0
    }
  )", "IFDSClientAliasFlowTest");

  auto *Callee = IR.Mod->getFunction("callee");
  auto *Main = IR.Mod->getFunction("main");
  IR.Global = IR.Mod->getNamedGlobal("g");
  IR.Callee = Callee;
  IR.Formal = Callee != nullptr ? &*Callee->arg_begin() : nullptr;
  IR.CalleeEntryInst = Callee != nullptr ? Callee->getEntryBlock().getTerminator()
                                         : nullptr;
  IR.Actual = lotus::unittest::findInstructionByName(*Main, "actual");
  IR.Call = llvm::cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(*Main, "call"));
  IR.ExtCall = lotus::unittest::findCallTo(*Main, "ext");
  IR.CallReturnSite = IR.ExtCall;
  IR.ExtCallReturnSite = Main->back().getTerminator();

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

} // namespace

TEST(IFDSConstAnalysisFlowTest, CallFlowMapsActualToFormal) {
  auto IR = buildIFDSFlowFixture();
  ifds::ConstAnalysis Analysis;

  auto Out = Analysis.call_flow(IR.Call, IR.Callee,
                                ifds::ConstFact::initialized(IR.Actual));

  EXPECT_EQ(Out.count(ifds::ConstFact::initialized(IR.Formal)), 1U);
}

TEST(IFDSConstAnalysisFlowTest, ReturnFlowMapsFormalBackToActual) {
  auto IR = buildIFDSFlowFixture();
  ifds::ConstAnalysis Analysis;

  auto Out = Analysis.return_flow(IR.Call, IR.CalleeEntryInst,
                                  IR.CallReturnSite, IR.Callee,
                                  ifds::ConstFact::mutable_mem(IR.Formal),
                                  ifds::ConstFact::zero());

  EXPECT_EQ(Out.count(ifds::ConstFact::mutable_mem(IR.Actual)), 1U);
}

TEST(IFDSConstAnalysisFlowTest, CallToReturnKillsPointerArgFact) {
  auto IR = buildIFDSFlowFixture();
  ifds::ConstAnalysis Analysis;

  auto Out = Analysis.call_to_return_flow(
      IR.Call, IR.CallReturnSite,
      llvm::ArrayRef<const llvm::Function *>{IR.Callee},
      ifds::ConstFact::initialized(IR.Actual));

  EXPECT_TRUE(Out.empty());
}

TEST(IFDSConstAnalysisFlowTest, CallToReturnPreservesZeroAndGlobalFacts) {
  auto IR = buildIFDSFlowFixture();
  ifds::ConstAnalysis Analysis;

  auto ZeroOut =
      Analysis.call_to_return_flow(IR.Call, IR.CallReturnSite,
                                   llvm::ArrayRef<const llvm::Function *>{IR.Callee},
                                   ifds::ConstFact::zero());
  EXPECT_EQ(ZeroOut.count(ifds::ConstFact::zero()), 1U);

  auto GlobalOut = Analysis.call_to_return_flow(
      IR.Call, IR.CallReturnSite,
      llvm::ArrayRef<const llvm::Function *>{IR.Callee},
      ifds::ConstFact::initialized(IR.Global));
  EXPECT_EQ(GlobalOut.count(ifds::ConstFact::initialized(IR.Global)), 1U);
}

TEST(IFDSReachingDefinitionsFlowTest, CallFlowMapsActualDefToFormalDef) {
  auto IR = buildIFDSFlowFixture();
  ifds::ReachingDefinitionsAnalysis Analysis;

  auto Out = Analysis.call_flow(
      IR.Call, IR.Callee,
      ifds::DefinitionFact::definition(IR.Actual, IR.Call));

  EXPECT_TRUE(containsDefinition(Out, IR.Formal, IR.CalleeEntryInst));
}

TEST(IFDSReachingDefinitionsFlowTest, ReturnFlowMapsReturnValueToCallResult) {
  auto IR = buildIFDSFlowFixture();
  ifds::ReachingDefinitionsAnalysis Analysis;

  auto Out = Analysis.return_flow(
      IR.Call, IR.CalleeEntryInst, IR.CallReturnSite, IR.Callee,
      ifds::DefinitionFact::definition(IR.Formal, IR.CalleeEntryInst),
      ifds::DefinitionFact::zero());

  EXPECT_TRUE(containsDefinition(Out, IR.Call, IR.CalleeEntryInst));
}

TEST(IFDSReachingDefinitionsFlowTest, CallToReturnKillsCalleeNonLocalFacts) {
  auto IR = buildIFDSFlowFixture();
  ifds::ReachingDefinitionsAnalysis Analysis;

  auto Out = Analysis.call_to_return_flow(
      IR.Call, IR.CallReturnSite,
      llvm::ArrayRef<const llvm::Function *>{IR.Callee},
      ifds::DefinitionFact::definition(IR.Formal, IR.CalleeEntryInst));

  EXPECT_TRUE(Out.empty());
}

TEST(IFDSReachingDefinitionsFlowTest, CallToReturnKeepsCallerLocalFacts) {
  auto IR = buildIFDSFlowFixture();
  ifds::ReachingDefinitionsAnalysis Analysis;

  auto InFact = ifds::DefinitionFact::definition(IR.Actual, IR.Call);
  auto Out = Analysis.call_to_return_flow(
      IR.Call, IR.CallReturnSite,
      llvm::ArrayRef<const llvm::Function *>{IR.Callee}, InFact);

  EXPECT_EQ(Out.count(InFact), 1U);
}

TEST(IFDSReachingDefinitionsFlowTest, ExternalCallKillsGlobalsAndKeepsZero) {
  auto IR = buildIFDSFlowFixture();
  ifds::ReachingDefinitionsAnalysis Analysis;

  auto GlobalFact = ifds::DefinitionFact::definition(IR.Global, IR.Call);
  auto GlobalOut =
      Analysis.call_to_return_flow(
          IR.ExtCall, IR.ExtCallReturnSite,
          llvm::ArrayRef<const llvm::Function *>{IR.ExtCall->getCalledFunction()},
          GlobalFact);
  EXPECT_TRUE(GlobalOut.empty());

  auto ZeroOut = Analysis.call_to_return_flow(IR.ExtCall, IR.ExtCallReturnSite,
                                              llvm::ArrayRef<const llvm::Function *>{IR.ExtCall->getCalledFunction()},
                                              ifds::DefinitionFact::zero());
  EXPECT_EQ(ZeroOut.count(ifds::DefinitionFact::zero()), 1U);
}
