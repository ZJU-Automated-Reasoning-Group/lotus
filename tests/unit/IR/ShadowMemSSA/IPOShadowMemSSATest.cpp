#include "IR/ShadowMemSSA/ShadowMemSSA.h"
#include "Optimization/IPO/IPStoreSinking.h"
#include "Optimization/IPO/IPStoreToLoadForwarding.h"
#include "TestUtils/LLVMHelpers.h"

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::unittest;
using namespace previrt::analysis;
using namespace previrt::transforms;

namespace {

class DummyModulePass final : public ModulePass {
public:
  static char ID;

  DummyModulePass() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    (void)M;
    return false;
  }
};

char DummyModulePass::ID = 0;

std::unique_ptr<Module> parseTestModule(LLVMContext &Context, StringRef IR) {
  return parseAssemblyChecked(Context, IR.str(), "ipo-memoryssa-test.ll");
}

} // namespace

TEST(IPOShadowMemSSATest, ShadowMemSSACallsManagerTracksInvokeCallsites) {
  LLVMContext Context;
  auto Module = parseTestModule(Context, R"(
    declare i32 @__gxx_personality_v0(...)
    declare void @callee(i8*)
    declare i32 @shadow.mem.arg.mod(i32, i32, i32, i8*)

    @glob = global i8 0

    define void @caller(i8* %p) personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %m0 = call i32 @shadow.mem.arg.mod(i32 7, i32 11, i32 0, i8* @glob)
      invoke void @callee(i8* %p) to label %cont unwind label %lpad

    cont:
      ret void

    lpad:
      landingpad { i8*, i32 } cleanup
      ret void
    }
  )");

  DummyModulePass Pass;
  ShadowMemSSACallsManager Manager(*Module, Pass, false);
  Function *Caller = Module->getFunction("caller");
  ASSERT_NE(Caller, nullptr);

  CallBase *Invoke = nullptr;
  for (Instruction &Inst : instructions(Caller)) {
    if (auto *CB = dyn_cast<CallBase>(&Inst)) {
      if (CB->getCalledFunction() &&
          CB->getCalledFunction()->getName() == "callee") {
        Invoke = CB;
        break;
      }
    }
  }

  ASSERT_NE(Invoke, nullptr);
  const ShadowMemSSACallSite *CallSite = Manager.getCallSite(Invoke);
  ASSERT_NE(CallSite, nullptr);
  ASSERT_EQ(CallSite->numParams(), 1u);
  EXPECT_TRUE(CallSite->isMod(0));
}

TEST(IPOShadowMemSSATest, StoreToLoadForwardingFollowsFunOutBackToInFormal) {
  LLVMContext Context;
  auto Module = parseTestModule(Context, R"(
    declare i32 @shadow.mem.store(i32, i32, i8*)
    declare void @shadow.mem.load(i32, i32, i8*)
    declare i32 @shadow.mem.arg.mod(i32, i32, i32, i8*)
    declare i32 @shadow.mem.arg.init(i32, i8*)
    declare void @shadow.mem.in(i32, i32, i32, i8*)
    declare void @shadow.mem.out(i32, i32, i32, i8*)

    @glob = global i32 0

    define void @callee() {
    entry:
      %init = call i32 @shadow.mem.arg.init(i32 0, i8* bitcast (i32* @glob to i8*))
      %mem1 = call i32 @shadow.mem.store(i32 0, i32 %init, i8* bitcast (i32* @glob to i8*))
      store i32 42, i32* @glob, align 4
      br label %exit

    exit:
      call void @shadow.mem.in(i32 0, i32 %init, i32 0, i8* bitcast (i32* @glob to i8*))
      call void @shadow.mem.out(i32 0, i32 %mem1, i32 0, i8* bitcast (i32* @glob to i8*))
      ret void
    }

    define i32 @caller() {
    entry:
      %mem.after = call i32 @shadow.mem.arg.mod(i32 0, i32 5, i32 0, i8* bitcast (i32* @glob to i8*))
      call void @callee()
      call void @shadow.mem.load(i32 0, i32 %mem.after, i8* bitcast (i32* @glob to i8*))
      %load = load i32, i32* @glob, align 4
      ret i32 %load
    }
  )");

  auto *Forward = createIPStoreToLoadForwardingPass();
  ASSERT_NE(Forward, nullptr);
  bool Changed = Forward->runOnModule(*Module);
  delete Forward;

  EXPECT_TRUE(Changed);
  Function *Caller = Module->getFunction("caller");
  ASSERT_NE(Caller, nullptr);
  auto *Ret = dyn_cast<ReturnInst>(Caller->getEntryBlock().getTerminator());
  ASSERT_NE(Ret, nullptr);
  auto *ReturnedConst = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(ReturnedConst, nullptr);
  EXPECT_EQ(ReturnedConst->getSExtValue(), 42);
}

TEST(IPOShadowMemSSATest, StoreSinkingDoesNotTreatBenignPointerOperandUseAsHazard) {
  LLVMContext Context;
  auto Module = parseTestModule(Context, R"(
    declare i32 @shadow.mem.store(i32, i32, i8*)
    declare void @shadow.mem.load(i32, i32, i8*)

    @glob = global i32 0

    define void @test() {
    entry:
      %m1 = call i32 @shadow.mem.store(i32 0, i32 1, i8* bitcast (i32* @glob to i8*))
      store i32 7, i32* @glob, align 4
      %cmp = icmp eq i32* @glob, null
      call void @shadow.mem.load(i32 0, i32 %m1, i8* bitcast (i32* @glob to i8*))
      %v = load i32, i32* @glob, align 4
      %cmp.i32 = zext i1 %cmp to i32
      %use = add i32 %v, %cmp.i32
      call void asm sideeffect "", "r"(i32 %use)
      ret void
    }
  )");

  Function *Test = Module->getFunction("test");
  ASSERT_NE(Test, nullptr);
  StoreInst *StoreBefore = nullptr;
  ICmpInst *Cmp = nullptr;
  LoadInst *Load = nullptr;
  for (Instruction &Inst : instructions(Test)) {
    if (!StoreBefore) {
      StoreBefore = dyn_cast<StoreInst>(&Inst);
    }
    if (!Cmp) {
      Cmp = dyn_cast<ICmpInst>(&Inst);
    }
    if (auto *LI = dyn_cast<LoadInst>(&Inst)) {
      Load = LI;
      break;
    }
  }
  ASSERT_NE(StoreBefore, nullptr);
  ASSERT_NE(Cmp, nullptr);
  ASSERT_NE(Load, nullptr);
  EXPECT_TRUE(StoreBefore->comesBefore(Cmp));
  EXPECT_TRUE(StoreBefore->comesBefore(Load));

  auto *Sink = createIPStoreSinkingPass();
  ASSERT_NE(Sink, nullptr);
  bool Changed = Sink->runOnModule(*Module);
  delete Sink;

  EXPECT_TRUE(Changed);

  StoreInst *StoreAfter = nullptr;
  ICmpInst *CmpAfter = nullptr;
  LoadInst *LoadAfter = nullptr;
  for (Instruction &Inst : instructions(Test)) {
    if (!CmpAfter) {
      CmpAfter = dyn_cast<ICmpInst>(&Inst);
    }
    if (auto *LI = dyn_cast<LoadInst>(&Inst)) {
      LoadAfter = LI;
      break;
    }
    if (auto *SI = dyn_cast<StoreInst>(&Inst)) {
      StoreAfter = SI;
    }
  }

  ASSERT_NE(StoreAfter, nullptr);
  ASSERT_NE(CmpAfter, nullptr);
  ASSERT_NE(LoadAfter, nullptr);
  EXPECT_TRUE(CmpAfter->comesBefore(StoreAfter));
  EXPECT_TRUE(StoreAfter->comesBefore(LoadAfter));
}
