#include "IR/GSA/GSA.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace llvm;
using namespace gsa;
using namespace lotus::unittest;

namespace {

struct GatePipeline {
  std::unique_ptr<legacy::PassManager> PM;
  ControlDependenceAnalysisPass *CDA{nullptr};
  GateAnalysisPass *GA{nullptr};
};

struct MaterializationPipeline {
  std::unique_ptr<legacy::PassManager> PM;
  ControlDependenceAnalysisPass *CDA{nullptr};
  GateAnalysisPass *GA{nullptr};
  GsaMaterializationPass *GM{nullptr};
};

class GSATest : public LlvmModuleTest {
protected:
  static void initializePassInfra() {
    static bool initialized = false;
    if (initialized)
      return;

    auto &registry = *PassRegistry::getPassRegistry();
    initializeCore(registry);
    initializeAnalysis(registry);
    initializeTransformUtils(registry);
    initialized = true;
  }

  GatePipeline runGateAnalysis(Module &M, bool thinned = true) {
    initializePassInfra();

    GatePipeline pipeline;
    pipeline.PM = std::make_unique<legacy::PassManager>();
    pipeline.CDA = new ControlDependenceAnalysisPass();
    pipeline.GA = new GateAnalysisPass(thinned);
    pipeline.PM->add(pipeline.CDA);
    pipeline.PM->add(pipeline.GA);
    pipeline.PM->run(M);
    return pipeline;
  }

  MaterializationPipeline runMaterialization(Module &M, bool replace_phis,
                                            bool thinned = true) {
    initializePassInfra();

    MaterializationPipeline pipeline;
    pipeline.PM = std::make_unique<legacy::PassManager>();
    pipeline.CDA = new ControlDependenceAnalysisPass();
    pipeline.GA = new GateAnalysisPass(thinned);
    pipeline.GM = new GsaMaterializationPass(replace_phis);
    pipeline.PM->add(pipeline.CDA);
    pipeline.PM->add(pipeline.GA);
    pipeline.PM->add(pipeline.GM);
    pipeline.PM->run(M);
    return pipeline;
  }

  template <typename InstT> InstT *findFirst(Function *F) {
    for (auto &BB : *F)
      for (auto &I : BB)
        if (auto *Match = dyn_cast<InstT>(&I))
          return Match;
    return nullptr;
  }

  unsigned countPhiNodes(Function *F) {
    unsigned count = 0;
    for (auto &BB : *F)
      for (auto &PN : BB.phis()) {
        (void)PN;
        ++count;
      }
    return count;
  }

  unsigned countSelects(Function *F) {
    unsigned count = 0;
    for (auto &BB : *F)
      for (auto &I : BB)
        if (isa<SelectInst>(&I))
          ++count;
    return count;
  }

  SelectInst *findLotusGsaSelect(Function *F) {
    for (auto &BB : *F)
      for (auto &I : BB)
        if (auto *SI = dyn_cast<SelectInst>(&I))
          if (SI->getName().startswith("lotus.gsa."))
            return SI;
    return nullptr;
  }

  std::string renderValue(Value *V) {
    if (auto *CI = dyn_cast<ConstantInt>(V))
      return std::to_string(CI->getSExtValue());
    if (V->hasName())
      return V->getName().str();
    std::string buffer;
    raw_string_ostream os(buffer);
    V->print(os);
    return os.str();
  }

  std::string renderGuard(const GateGuard &guard) {
    switch (guard.getKind()) {
    case GuardKind::Unconditional:
      return "unconditional";
    case GuardKind::BranchTrue:
      return "branch-true";
    case GuardKind::BranchFalse:
      return "branch-false";
    case GuardKind::SwitchCase:
      return "switch-case:" + renderValue(guard.getCaseValue());
    case GuardKind::SwitchDefault:
      return "switch-default";
    case GuardKind::InvokeNormal:
      return "invoke-normal";
    case GuardKind::InvokeUnwind:
      return "invoke-unwind";
    case GuardKind::Opaque:
      return "opaque";
    }
    llvm_unreachable("Unknown guard kind");
  }

  std::string renderExpr(const GateExpr *Expr) {
    switch (Expr->getKind()) {
    case GateExpr::Kind::Bottom:
      return "bottom";
    case GateExpr::Kind::LeafValue:
      return "leaf(" + renderValue(Expr->getLeafValue()) + ")";
    case GateExpr::Kind::Select:
      return "select(" + renderGuard(Expr->getTrueGuard()) + "," +
             renderExpr(Expr->getTrueExpr()) + "," +
             renderExpr(Expr->getFalseExpr()) + ")";
    case GateExpr::Kind::Switch: {
      std::string rendered = "switch(";
      bool first = true;
      for (const auto &Arm : Expr->getSwitchArms()) {
        if (!first)
          rendered += ";";
        first = false;
        rendered += renderGuard(Arm.guard) + "->" + renderExpr(Arm.expr);
      }
      rendered += ")";
      return rendered;
    }
    }
    llvm_unreachable("Unknown gate expression kind");
  }
};

TEST_F(GSATest, DiamondBuildsGammaSelect) {
  const char *source = R"(
    define i32 @test(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %phi = phi i32 [ 1, %then ], [ 2, %else ]
      ret i32 %phi
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runGateAnalysis(*module);
  GateAnalysis &analysis = pipeline.GA->getGateAnalysis(*F);
  PHINode *phi = findPhi(F, "phi");
  ASSERT_NE(phi, nullptr);
  ASSERT_TRUE(analysis.hasGate(*phi));
  ASSERT_EQ(analysis.gates().size(), 1u);

  const GateNode &gate = analysis.getGate(*phi);
  EXPECT_EQ(gate.getKind(), GateKind::Gamma);
  EXPECT_TRUE(gate.isLowerable());

  const GateExpr *root = gate.getRootExpr();
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->getKind(), GateExpr::Kind::Select);
  EXPECT_EQ(root->getTrueGuard().getKind(), GuardKind::BranchTrue);
  EXPECT_EQ(root->getFalseGuard().getKind(), GuardKind::BranchFalse);
  EXPECT_EQ(root->getTrueGuard().getControlBlock()->getName(), "entry");
  ASSERT_EQ(root->getTrueExpr()->getKind(), GateExpr::Kind::LeafValue);
  ASSERT_EQ(root->getFalseExpr()->getKind(), GateExpr::Kind::LeafValue);
  EXPECT_EQ(cast<ConstantInt>(root->getTrueExpr()->getLeafValue())->getSExtValue(),
            1);
  EXPECT_EQ(
      cast<ConstantInt>(root->getFalseExpr()->getLeafValue())->getSExtValue(),
      2);
}

TEST_F(GSATest, NestedBranchShapeIsDeterministic) {
  const char *source = R"(
    define i32 @test(i1 %a, i1 %b) {
    entry:
      br i1 %a, label %left, label %right
    left:
      br i1 %b, label %left.true, label %left.false
    left.true:
      br label %merge
    left.false:
      br label %merge
    right:
      br label %merge
    merge:
      %phi = phi i32 [ 1, %left.true ], [ 2, %left.false ], [ 3, %right ]
      ret i32 %phi
    }
  )";

  auto module1 = parseModule(source);
  auto module2 = parseModule(source);
  ASSERT_NE(module1, nullptr);
  ASSERT_NE(module2, nullptr);

  Function *F1 = module1->getFunction("test");
  Function *F2 = module2->getFunction("test");
  ASSERT_NE(F1, nullptr);
  ASSERT_NE(F2, nullptr);

  auto pipeline1 = runGateAnalysis(*module1);
  auto pipeline2 = runGateAnalysis(*module2);

  const GateNode &gate1 =
      pipeline1.GA->getGateAnalysis(*F1).getGate(*findPhi(F1, "phi"));
  const GateNode &gate2 =
      pipeline2.GA->getGateAnalysis(*F2).getGate(*findPhi(F2, "phi"));

  EXPECT_EQ(renderExpr(gate1.getRootExpr()), renderExpr(gate2.getRootExpr()));
}

TEST_F(GSATest, SwitchPreservesExplicitArms) {
  const char *source = R"(
    define i32 @test(i32 %tag) {
    entry:
      switch i32 %tag, label %default [
        i32 0, label %case0
        i32 1, label %case1
      ]
    case0:
      br label %merge
    case1:
      br label %merge
    default:
      br label %merge
    merge:
      %phi = phi i32 [ 10, %case0 ], [ 20, %case1 ], [ 30, %default ]
      ret i32 %phi
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runGateAnalysis(*module);
  const GateNode &gate =
      pipeline.GA->getGateAnalysis(*F).getGate(*findPhi(F, "phi"));
  const GateExpr *root = gate.getRootExpr();
  ASSERT_EQ(root->getKind(), GateExpr::Kind::Switch);
  ASSERT_EQ(root->getSwitchArms().size(), 3u);
  EXPECT_EQ(root->getSwitchArms()[0].guard.getKind(), GuardKind::SwitchCase);
  EXPECT_EQ(root->getSwitchArms()[1].guard.getKind(), GuardKind::SwitchCase);
  EXPECT_EQ(root->getSwitchArms()[2].guard.getKind(), GuardKind::SwitchDefault);
}

TEST_F(GSATest, LoopHeaderPhiIsMu) {
  const char *source = R"(
    define i32 @test() {
    entry:
      br label %header
    header:
      %i = phi i32 [ 0, %entry ], [ %inc, %latch ]
      %cmp = icmp slt i32 %i, 4
      br i1 %cmp, label %latch, label %exit
    latch:
      %inc = add i32 %i, 1
      br label %header
    exit:
      ret i32 %i
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runGateAnalysis(*module);
  const GateNode &gate =
      pipeline.GA->getGateAnalysis(*F).getGate(*findPhi(F, "i"));
  EXPECT_EQ(gate.getKind(), GateKind::Mu);
}

TEST_F(GSATest, LoopExitPhiIsEta) {
  const char *source = R"(
    define i32 @test() {
    entry:
      br label %header
    header:
      %i = phi i32 [ 0, %entry ], [ %inc, %latch ]
      %cmp = icmp slt i32 %i, 4
      br i1 %cmp, label %latch, label %exit
    latch:
      %inc = add i32 %i, 1
      br label %header
    exit:
      %out = phi i32 [ %i, %header ]
      ret i32 %out
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runGateAnalysis(*module);
  const GateNode &gate =
      pipeline.GA->getGateAnalysis(*F).getGate(*findPhi(F, "out"));
  EXPECT_EQ(gate.getKind(), GateKind::Eta);
}

TEST_F(GSATest, UnreachablePhiIsIgnoredAndCdaReturnsEmpty) {
  const char *source = R"(
    define i32 @test(i1 %cond) {
    entry:
      br i1 %cond, label %live, label %exit
    live:
      br label %exit
    dead1:
      br label %dead2
    dead2:
      %deadphi = phi i32 [ 7, %dead1 ]
      ret i32 %deadphi
    exit:
      %livephi = phi i32 [ 1, %entry ], [ 2, %live ]
      ret i32 %livephi
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runGateAnalysis(*module);
  GateAnalysis &ga = pipeline.GA->getGateAnalysis(*F);
  ControlDependenceAnalysis &cda =
      pipeline.CDA->getControlDependenceAnalysis(*F);

  PHINode *deadphi = findPhi(F, "deadphi");
  PHINode *livephi = findPhi(F, "livephi");
  BasicBlock *dead1 = findBlock(F, "dead1");
  ASSERT_NE(deadphi, nullptr);
  ASSERT_NE(livephi, nullptr);
  ASSERT_NE(dead1, nullptr);

  EXPECT_FALSE(ga.hasGate(*deadphi));
  EXPECT_TRUE(ga.hasGate(*livephi));
  EXPECT_FALSE(cda.isTracked(*dead1));
  EXPECT_TRUE(cda.getCDBlocks(dead1).empty());
}

TEST_F(GSATest, InvokeGateDistinguishesNormalAndUnwind) {
  const char *source = R"(
    declare i32 @may_throw()
    declare i32 @__gxx_personality_v0(...)

    define i32 @test() personality i8* bitcast (i32 (...)* @__gxx_personality_v0 to i8*) {
    entry:
      invoke i32 @may_throw() to label %normal unwind label %lpad
    normal:
      br label %merge
    lpad:
      %lp = landingpad { i8*, i32 }
              cleanup
      br label %merge
    merge:
      %phi = phi i32 [ 1, %normal ], [ 2, %lpad ]
      ret i32 %phi
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runGateAnalysis(*module);
  const GateNode &gate =
      pipeline.GA->getGateAnalysis(*F).getGate(*findPhi(F, "phi"));
  ASSERT_NE(gate.getRootExpr(), nullptr);
  ASSERT_EQ(gate.getRootExpr()->getKind(), GateExpr::Kind::Switch);
  EXPECT_FALSE(gate.isLowerable());
  ASSERT_EQ(gate.getRootExpr()->getSwitchArms().size(), 2u);
  EXPECT_EQ(gate.getRootExpr()->getSwitchArms()[0].guard.getKind(),
            GuardKind::InvokeNormal);
  EXPECT_EQ(gate.getRootExpr()->getSwitchArms()[1].guard.getKind(),
            GuardKind::InvokeUnwind);
}

TEST_F(GSATest, OpaqueTerminatorBuildsNonLowerableGateAndMaterializerSkipsPhi) {
  const char *source = R"(
    define i32 @test() {
    entry:
      indirectbr i8* blockaddress(@test, %left), [label %left, label %right]
    left:
      br label %merge
    right:
      br label %merge
    merge:
      %phi = phi i32 [ 1, %left ], [ 2, %right ]
      ret i32 %phi
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runMaterialization(*module, true);
  GateAnalysis &analysis = pipeline.GA->getGateAnalysis(*F);
  PHINode *phi = findPhi(F, "phi");
  ASSERT_NE(phi, nullptr);
  const GateNode &gate = analysis.getGate(*phi);
  EXPECT_FALSE(gate.isLowerable());
  ASSERT_EQ(gate.getRootExpr()->getKind(), GateExpr::Kind::Switch);
  ASSERT_EQ(gate.getRootExpr()->getSwitchArms().size(), 2u);
  EXPECT_EQ(gate.getRootExpr()->getSwitchArms()[0].guard.getKind(),
            GuardKind::Opaque);
  EXPECT_EQ(gate.getRootExpr()->getSwitchArms()[1].guard.getKind(),
            GuardKind::Opaque);
  EXPECT_EQ(countPhiNodes(F), 1u);
}

TEST_F(GSATest, GateAnalysisPassLeavesIrUnchanged) {
  const char *source = R"(
    define i32 @test(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %phi = phi i32 [ 1, %then ], [ 2, %else ]
      ret i32 %phi
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  std::string before;
  raw_string_ostream before_os(before);
  module->print(before_os, nullptr);
  before_os.flush();

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);
  unsigned phi_count_before = countPhiNodes(F);

  auto pipeline = runGateAnalysis(*module);
  (void)pipeline;

  std::string after;
  raw_string_ostream after_os(after);
  module->print(after_os, nullptr);
  after_os.flush();

  EXPECT_EQ(before, after);
  EXPECT_EQ(phi_count_before, countPhiNodes(F));
}

TEST_F(GSATest, MaterializationUsesLotusNamesAndOptionalPhiReplacement) {
  const char *source = R"(
    define i32 @test(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %phi = phi i32 [ 1, %then ], [ 2, %else ]
      ret i32 %phi
    }
  )";

  auto module_keep = parseModule(source);
  auto module_replace = parseModule(source);
  ASSERT_NE(module_keep, nullptr);
  ASSERT_NE(module_replace, nullptr);

  Function *keep = module_keep->getFunction("test");
  Function *replace = module_replace->getFunction("test");
  ASSERT_NE(keep, nullptr);
  ASSERT_NE(replace, nullptr);

  auto keep_pipeline = runMaterialization(*module_keep, false);
  EXPECT_EQ(countPhiNodes(keep), 1u);
  ASSERT_EQ(countSelects(keep), 1u);
  SelectInst *keep_select = findLotusGsaSelect(keep);
  ASSERT_NE(keep_select, nullptr);
  EXPECT_TRUE(keep_select->getName().startswith("lotus.gsa."));

  auto replace_pipeline = runMaterialization(*module_replace, true);
  EXPECT_EQ(countPhiNodes(replace), 0u);
  ASSERT_EQ(countSelects(replace), 1u);
  SelectInst *replace_select = findLotusGsaSelect(replace);
  ASSERT_NE(replace_select, nullptr);
  EXPECT_TRUE(replace_select->getName().startswith("lotus.gsa."));
}

TEST_F(GSATest, GateAnalysisPassPreservesValidSsaForDownstreamUsers) {
  const char *source = R"(
    define i32 @test(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %phi = phi i32 [ 1, %then ], [ 2, %else ]
      ret i32 %phi
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runGateAnalysis(*module);
  (void)pipeline;

  EXPECT_FALSE(verifyModule(*module, &errs()));
  EXPECT_EQ(countPhiNodes(F), 1u);
}

} // namespace
