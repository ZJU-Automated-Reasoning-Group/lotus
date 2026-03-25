#include <memory>
#include <set>

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>
#include <gtest/gtest.h>
#include <Dataflow/IFDS/Utils/LLVMFlowHelpers.h>
#include <TestUtils/LLVMHelpers.h>

namespace {

struct CallFixtureIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  const llvm::CallBase *Call = nullptr;
  const llvm::Value *Arg = nullptr;
  const llvm::GlobalVariable *Global = nullptr;
};

CallFixtureIR buildCallFixture() {
  CallFixtureIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod = lotus::unittest::parseModule(*IR.Ctx, R"(
    @g = global i32 0

    declare void @callee(i32*)

    define i32 @main() {
    entry:
      %local = alloca i32
      call void @callee(i32* %local)
      ret i32 0
    }
  )", "LLVMFlowHelpersTest");
  IR.Global = IR.Mod->getNamedGlobal("g");
  IR.Arg =
      lotus::unittest::findInstructionByName(*IR.Mod->getFunction("main"),
                                             "local");
  IR.Call = lotus::unittest::findCallTo(*IR.Mod->getFunction("main"), "callee");

  return IR;
}

} // namespace

TEST(LLVMFlowHelpersTest, PolicyPropagatesZeroWhenEnabled) {
  auto IR = buildCallFixture();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_alongside_callsite_with_policies(
      IR.Call, nullptr, Out,
      [](const llvm::Value *, const llvm::Value *) { return true; },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); },
      /*PropagateGlobals=*/true,
      /*PropagateZero=*/true);

  EXPECT_EQ(Out.size(), 1U);
  EXPECT_TRUE(Out.count(nullptr));
}

TEST(LLVMFlowHelpersTest, PolicyKillsZeroWhenDisabled) {
  auto IR = buildCallFixture();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_alongside_callsite_with_policies(
      IR.Call, nullptr, Out,
      [](const llvm::Value *, const llvm::Value *) { return false; },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); },
      /*PropagateGlobals=*/true,
      /*PropagateZero=*/false);

  EXPECT_TRUE(Out.empty());
}

TEST(LLVMFlowHelpersTest, PolicyPropagatesGlobalWhenEnabled) {
  auto IR = buildCallFixture();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_alongside_callsite_with_policies(
      IR.Call, IR.Global, Out,
      [](const llvm::Value *, const llvm::Value *) { return true; },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); },
      /*PropagateGlobals=*/true,
      /*PropagateZero=*/true);

  EXPECT_EQ(Out.size(), 1U);
  EXPECT_TRUE(Out.count(IR.Global));
}

TEST(LLVMFlowHelpersTest, PolicyKillsGlobalWhenDisabled) {
  auto IR = buildCallFixture();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_alongside_callsite_with_policies(
      IR.Call, IR.Global, Out,
      [](const llvm::Value *, const llvm::Value *) { return false; },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); },
      /*PropagateGlobals=*/false,
      /*PropagateZero=*/true);

  EXPECT_TRUE(Out.empty());
}

TEST(LLVMFlowHelpersTest, PolicyKillsLocalWhenPredicateMatches) {
  auto IR = buildCallFixture();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_alongside_callsite_with_policies(
      IR.Call, IR.Arg, Out,
      [](const llvm::Value *Arg, const llvm::Value *Source) {
        return Arg == Source;
      },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); },
      /*PropagateGlobals=*/true,
      /*PropagateZero=*/true);

  EXPECT_TRUE(Out.empty());
}

TEST(LLVMFlowHelpersTest, PolicyPropagatesLocalWhenPredicateDoesNotMatch) {
  auto IR = buildCallFixture();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_alongside_callsite_with_policies(
      IR.Call, IR.Arg, Out,
      [](const llvm::Value *, const llvm::Value *) { return false; },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); },
      /*PropagateGlobals=*/true,
      /*PropagateZero=*/true);

  EXPECT_EQ(Out.size(), 1U);
  EXPECT_TRUE(Out.count(IR.Arg));
}
