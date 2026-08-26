#include "Analysis/ControlDependence/ControlDependence.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "Analysis/ControlDependence/ICFGControlDependence.h"
#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"

#include <algorithm>
#include <memory>

#include <gtest/gtest.h>

using lotus::cd::Algorithm;
using lotus::cd::ControlDependenceAnalysis;
using lotus::cd::ControlDependenceOptions;
using lotus::cd::ICFGControlDependenceAnalysis;

namespace {

std::unique_ptr<llvm::Module> parseModule(llvm::LLVMContext &context,
                                          llvm::StringRef ir) {
  llvm::SMDiagnostic error;
  auto module = llvm::parseAssemblyString(ir, error, context);
  if (!module)
    error.print("ControlDependenceTest", llvm::errs());
  return module;
}

llvm::BasicBlock *block(llvm::Function &function, llvm::StringRef name) {
  for (llvm::BasicBlock &candidate : function)
    if (candidate.getName() == name)
      return &candidate;
  return nullptr;
}

ControlDependenceAnalysis analyze(llvm::Function &function,
                                  Algorithm algorithm) {
  return ControlDependenceAnalysis(function,
                                   ControlDependenceOptions{algorithm});
}

constexpr llvm::StringLiteral DiamondIR = R"(
  define void @diamond(i1 %condition) {
  entry:
    br i1 %condition, label %left, label %right
  left:
    br label %exit
  right:
    br label %exit
  exit:
    ret void
  }
)";

constexpr llvm::StringLiteral NonterminatingChoiceIR = R"(
  define void @nonterminating_choice(i1 %condition) {
  entry:
    br i1 %condition, label %left, label %right
  left:
    br label %left
  right:
    br label %right
  }
)";

constexpr llvm::StringLiteral DecisiveOrderIR = R"(
  define void @decisive_order(i1 %condition) {
  entry:
    br i1 %condition, label %blue, label %red
  blue:
    br label %blue_body
  blue_body:
    br label %red
  red:
    br label %red_body
  red_body:
    br label %blue
  }
)";

TEST(ControlDependenceTest, StandardComputesDiamondDependencesAndInverse) {
  llvm::LLVMContext context;
  auto module = parseModule(context, DiamondIR);
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("diamond");
  auto analysis = analyze(function, Algorithm::Standard);

  llvm::BasicBlock *entry = block(function, "entry");
  llvm::BasicBlock *left = block(function, "left");
  llvm::BasicBlock *right = block(function, "right");
  llvm::BasicBlock *exit = block(function, "exit");
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  ASSERT_NE(exit, nullptr);

  EXPECT_TRUE(analysis.dependsOn(left, entry));
  EXPECT_TRUE(analysis.dependsOn(right, entry));
  EXPECT_FALSE(analysis.dependsOn(exit, entry));
  ASSERT_EQ(analysis.getDependents(entry).size(), 2u);
  EXPECT_EQ(analysis.getDependents(entry)[0], left);
  EXPECT_EQ(analysis.getDependents(entry)[1], right);
}

TEST(ControlDependenceTest, StandardMatchesDgOnLoopsAndMultipleExits) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define void @loop(i1 %condition) {
    entry:
      br label %header
    header:
      br i1 %condition, label %body, label %exit
    body:
      br label %header
    exit:
      ret void
    }

    define void @multiple_exits(i1 %condition) {
    entry:
      br i1 %condition, label %left, label %right
    left:
      ret void
    right:
      ret void
    }
  )");
  ASSERT_TRUE(module);

  llvm::Function &loop = *module->getFunction("loop");
  llvm::BasicBlock *header = block(loop, "header");
  llvm::BasicBlock *body = block(loop, "body");
  auto loopAnalysis = analyze(loop, Algorithm::Standard);
  EXPECT_TRUE(loopAnalysis.dependsOn(body, header));
  EXPECT_TRUE(loopAnalysis.dependsOn(header, header));

  llvm::Function &multipleExits = *module->getFunction("multiple_exits");
  llvm::BasicBlock *entry = block(multipleExits, "entry");
  llvm::BasicBlock *left = block(multipleExits, "left");
  llvm::BasicBlock *right = block(multipleExits, "right");
  auto exitAnalysis = analyze(multipleExits, Algorithm::Standard);
  EXPECT_TRUE(exitAnalysis.dependsOn(left, entry));
  EXPECT_TRUE(exitAnalysis.dependsOn(right, entry));
}

TEST(ControlDependenceTest, NTSCDImplementationsAgreeOnDiamond) {
  llvm::LLVMContext context;
  auto module = parseModule(context, DiamondIR);
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("diamond");
  llvm::BasicBlock *entry = block(function, "entry");
  llvm::BasicBlock *left = block(function, "left");
  llvm::BasicBlock *right = block(function, "right");

  for (Algorithm algorithm :
       {Algorithm::NTSCD, Algorithm::NTSCD2, Algorithm::NTSCDLegacy,
        Algorithm::NTSCDRanganath, Algorithm::NTSCDRanganathOriginal}) {
    auto analysis = analyze(function, algorithm);
    EXPECT_TRUE(analysis.dependsOn(left, entry));
    EXPECT_TRUE(analysis.dependsOn(right, entry));
  }
}

TEST(ControlDependenceTest, NTSCDHandlesFunctionsWithoutExits) {
  llvm::LLVMContext context;
  auto module = parseModule(context, NonterminatingChoiceIR);
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("nonterminating_choice");
  llvm::BasicBlock *entry = block(function, "entry");
  llvm::BasicBlock *left = block(function, "left");
  llvm::BasicBlock *right = block(function, "right");

  auto ntscd = analyze(function, Algorithm::NTSCD);
  auto ntscd2 = analyze(function, Algorithm::NTSCD2);
  EXPECT_TRUE(ntscd.dependsOn(left, entry));
  EXPECT_TRUE(ntscd.dependsOn(right, entry));
  EXPECT_TRUE(ntscd2.dependsOn(left, entry));
  EXPECT_TRUE(ntscd2.dependsOn(right, entry));
}

TEST(ControlDependenceTest, NTSCD2DoesNotReenqueueColoredSelfLoopTarget) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define void @self_loop_target(i1 %condition) {
    entry:
      br i1 %condition, label %loop, label %entry
    loop:
      br label %loop
    }
  )");
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("self_loop_target");
  llvm::BasicBlock *entry = block(function, "entry");
  llvm::BasicBlock *loop = block(function, "loop");

  auto ntscd2 = analyze(function, Algorithm::NTSCD2);
  auto combined = analyze(function, Algorithm::DODNTSCD);
  EXPECT_TRUE(ntscd2.dependsOn(loop, entry));
  EXPECT_TRUE(combined.dependsOn(loop, entry));
}

TEST(ControlDependenceTest, DODFindsDecisiveOrderOnInfiniteCycle) {
  llvm::LLVMContext context;
  auto module = parseModule(context, DecisiveOrderIR);
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("decisive_order");
  llvm::BasicBlock *entry = block(function, "entry");
  llvm::BasicBlock *blueBody = block(function, "blue_body");
  llvm::BasicBlock *redBody = block(function, "red_body");

  auto dod = analyze(function, Algorithm::DOD);
  auto ranganath = analyze(function, Algorithm::DODRanganath);
  auto combined = analyze(function, Algorithm::DODNTSCD);
  EXPECT_TRUE(dod.dependsOn(blueBody, entry));
  EXPECT_TRUE(dod.dependsOn(redBody, entry));
  EXPECT_TRUE(ranganath.dependsOn(blueBody, entry));
  EXPECT_TRUE(ranganath.dependsOn(redBody, entry));
  EXPECT_TRUE(combined.dependsOn(blueBody, entry));
  EXPECT_TRUE(combined.dependsOn(redBody, entry));
}

TEST(ControlDependenceTest, EveryBinaryAlgorithmMaintainsInverseRelation) {
  llvm::LLVMContext context;
  auto module = parseModule(context, DecisiveOrderIR);
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("decisive_order");

  for (Algorithm algorithm :
       {Algorithm::Standard, Algorithm::NTSCD, Algorithm::NTSCD2,
        Algorithm::NTSCDLegacy, Algorithm::NTSCDRanganath,
        Algorithm::NTSCDRanganathOriginal, Algorithm::DOD,
        Algorithm::DODRanganath, Algorithm::DODNTSCD}) {
    auto analysis = analyze(function, algorithm);
    for (llvm::BasicBlock &dependent : function)
      for (const llvm::BasicBlock *predicate :
           analysis.getDependencies(&dependent)) {
        auto inverse = analysis.getDependents(predicate);
        EXPECT_NE(std::find(inverse.begin(), inverse.end(), &dependent),
                  inverse.end());
      }
  }
}

TEST(ControlDependenceTest, StrongClosureIsStableAndFunctionOrdered) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define void @closure(i1 %condition) {
    b2:
      br label %b3
    b3:
      br i1 %condition, label %b0, label %b1
    b1:
      br label %b0
    b0:
      br label %b0
    }
  )");
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("closure");
  llvm::BasicBlock *b2 = block(function, "b2");
  llvm::BasicBlock *b3 = block(function, "b3");
  llvm::BasicBlock *b1 = block(function, "b1");

  auto analysis = analyze(function, Algorithm::StrongControlClosure);
  auto closure = analysis.getClosure({b1, b2, b1});
  ASSERT_EQ(closure.size(), 3u);
  EXPECT_EQ(closure[0], b2);
  EXPECT_EQ(closure[1], b3);
  EXPECT_EQ(closure[2], b1);

  auto closedAgain = analysis.getClosure(closure);
  EXPECT_EQ(closedAgain, closure);
  EXPECT_TRUE(analysis.getDependencies(b1).empty());
}

TEST(ControlDependenceTest, StrongClosureHandlesColoredTargetCycles) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define void @closure_cycle(i1 %condition) {
    start:
      br label %predicate
    predicate:
      br i1 %condition, label %loop, label %predicate
    loop:
      br label %loop
    }
  )");
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("closure_cycle");
  llvm::BasicBlock *start = block(function, "start");
  llvm::BasicBlock *predicate = block(function, "predicate");
  llvm::BasicBlock *loop = block(function, "loop");

  auto analysis = analyze(function, Algorithm::StrongControlClosure);
  auto closure = analysis.getClosure({start, loop});
  ASSERT_EQ(closure.size(), 3u);
  EXPECT_EQ(closure[0], start);
  EXPECT_EQ(closure[1], predicate);
  EXPECT_EQ(closure[2], loop);
}

TEST(ControlDependenceTest, ICFGAdapterRunsGraphAlgorithmsOnLotusICFG) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @main(i1 %condition) {
    entry:
      br i1 %condition, label %left, label %right
    left:
      br label %exit
    right:
      br label %exit
    exit:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());
  llvm::Function &function = *module->getFunction("main");
  ICFGNode *entry = icfg.getIntraBlockNode(block(function, "entry"));
  ICFGNode *left = icfg.getIntraBlockNode(block(function, "left"));
  ICFGNode *right = icfg.getIntraBlockNode(block(function, "right"));

  ICFGControlDependenceAnalysis analysis(
      icfg, ControlDependenceOptions{Algorithm::NTSCD2});
  EXPECT_TRUE(analysis.dependsOn(left, entry));
  EXPECT_TRUE(analysis.dependsOn(right, entry));
  ASSERT_EQ(analysis.getDependents(entry).size(), 2u);
  EXPECT_LT(analysis.getDependents(entry)[0]->getId(),
            analysis.getDependents(entry)[1]->getId());
}

TEST(ControlDependenceTest, ICFGAdapterFindsNoReturnCallDependence) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    declare void @abort() noreturn

    define void @foo(i1 %condition) {
    entry:
      br i1 %condition, label %normal, label %die
    normal:
      ret void
    die:
      call void @abort()
      unreachable
    }

    define i32 @main(i1 %condition) {
    entry:
      call void @foo(i1 %condition)
      br label %after
    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());
  llvm::Function &foo = *module->getFunction("foo");
  llvm::Function &main = *module->getFunction("main");
  ICFGNode *fooEntry = icfg.getIntraBlockNode(block(foo, "entry"));
  ICFGNode *after = icfg.getIntraBlockNode(block(main, "after"));

  ICFGControlDependenceAnalysis analysis(
      icfg, ControlDependenceOptions{Algorithm::NTSCD});
  EXPECT_TRUE(analysis.dependsOn(after, fooEntry));
}

} // namespace
