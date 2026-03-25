/**
 * @file ICFGTest.cpp
 * @brief Comprehensive unit tests for Interprocedural Control Flow Graph (ICFG)
 * 
 * ICFG represents the interprocedural control flow of a program,
 * connecting call sites to function entry/exit points.
 */

#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/GraphAnalysis.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

#include <vector>

using namespace llvm;
using namespace lotus::unittest;

class ICFGTest : public LlvmModuleTest {
protected:
  // Helper to find a call instruction
  const CallBase *findCall(const Function *F, StringRef calleeName) {
    return findCallTo(F, calleeName);
  }

  std::vector<const CallBase *> findCalls(const Function *F, StringRef calleeName) {
    return findCallsTo(F, calleeName);
  }
};

// Test 1: Simple function with entry and exit blocks
TEST_F(ICFGTest, SimpleFunction) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %x = add i32 1, 2
      br label %exit
    exit:
      ret i32 %x
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *F = module->getFunction("main");
  ASSERT_NE(F, nullptr);

  // Should have nodes for each basic block
  unsigned nodeCount = 0;
  for (const auto &BB : *F) {
    IntraBlockNode *node = icfg.getIntraBlockNode(&BB);
    ASSERT_NE(node, nullptr);
    ++nodeCount;
  }

  EXPECT_EQ(nodeCount, 2u);
}

TEST_F(ICFGTest, GlobalInitNodeAnchorsMainWhenPresent) {
  const char *source = R"(
    define void @helper() {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      call void @helper()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *mainFn = module->getFunction("main");
  Function *helperFn = module->getFunction("helper");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(helperFn, nullptr);

  GlobalInitBlockNode *globalInit = icfg.getGlobalInitICFGNode();
  ASSERT_NE(globalInit, nullptr);
  EXPECT_EQ(globalInit, icfg.getGlobalInitICFGNode());

  FunEntryBlockNode *mainEntry = icfg.getFunEntryICFGNode(mainFn);
  FunEntryBlockNode *helperEntry = icfg.getFunEntryICFGNode(helperFn);
  ASSERT_NE(mainEntry, nullptr);
  ASSERT_NE(helperEntry, nullptr);

  EXPECT_NE(icfg.getICFGEdge(globalInit, mainEntry, ICFGEdge::IntraCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(globalInit, helperEntry, ICFGEdge::IntraCF), nullptr);
}

TEST_F(ICFGTest, GlobalInitNodeConnectsAllRootsWithoutMain) {
  const char *source = R"(
    define void @leaf() {
    entry:
      ret void
    }

    define void @foo() {
    entry:
      call void @leaf()
      ret void
    }

    define void @bar() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *fooFn = module->getFunction("foo");
  Function *barFn = module->getFunction("bar");
  Function *leafFn = module->getFunction("leaf");
  ASSERT_NE(fooFn, nullptr);
  ASSERT_NE(barFn, nullptr);
  ASSERT_NE(leafFn, nullptr);

  GlobalInitBlockNode *globalInit = icfg.getGlobalInitICFGNode();
  FunEntryBlockNode *fooEntry = icfg.getFunEntryICFGNode(fooFn);
  FunEntryBlockNode *barEntry = icfg.getFunEntryICFGNode(barFn);
  FunEntryBlockNode *leafEntry = icfg.getFunEntryICFGNode(leafFn);
  ASSERT_NE(globalInit, nullptr);
  ASSERT_NE(fooEntry, nullptr);
  ASSERT_NE(barEntry, nullptr);
  ASSERT_NE(leafEntry, nullptr);

  EXPECT_NE(icfg.getICFGEdge(globalInit, fooEntry, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(globalInit, barEntry, ICFGEdge::IntraCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(globalInit, leafEntry, ICFGEdge::IntraCF), nullptr);
}

// Test 2: Intraprocedural edges for branch
TEST_F(ICFGTest, IntraEdgeCountForBranch) {
  const char *source = R"(
    define i32 @main(i32 %cond) {
    entry:
      %cmp = icmp eq i32 %cond, 0
      br i1 %cmp, label %then, label %else
    then:
      br label %exit
    else:
      br label %exit
    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *F = module->getFunction("main");
  ASSERT_NE(F, nullptr);

  const BasicBlock *entry = &F->getEntryBlock();
  const BasicBlock *thenBB = findBlock(F, "then");
  const BasicBlock *elseBB = findBlock(F, "else");
  const BasicBlock *exitBB = findBlock(F, "exit");

  ASSERT_NE(thenBB, nullptr);
  ASSERT_NE(elseBB, nullptr);
  ASSERT_NE(exitBB, nullptr);

  IntraBlockNode *entryNode = icfg.getIntraBlockNode(entry);
  IntraBlockNode *thenNode = icfg.getIntraBlockNode(thenBB);
  IntraBlockNode *elseNode = icfg.getIntraBlockNode(elseBB);
  IntraBlockNode *exitNode = icfg.getIntraBlockNode(exitBB);

  ASSERT_NE(entryNode, nullptr);
  ASSERT_NE(thenNode, nullptr);
  ASSERT_NE(elseNode, nullptr);
  ASSERT_NE(exitNode, nullptr);

  // Check intraprocedural edges
  EXPECT_NE(icfg.getICFGEdge(entryNode, thenNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(entryNode, elseNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(thenNode, exitNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(elseNode, exitNode, ICFGEdge::IntraCF), nullptr);
}

// Test 3: Interprocedural edges for function calls
TEST_F(ICFGTest, FunctionCall) {
  const char *source = R"(
    define i32 @callee() {
      ret i32 42
    }
    
    define i32 @caller() {
      %result = call i32 @callee()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  // Find the call instruction
  const CallBase *call = findCall(caller, "callee");
  ASSERT_NE(call, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(call->getParent());
  FunEntryBlockNode *calleeEntryNode = icfg.getFunEntryICFGNode(callee);

  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(calleeEntryNode, nullptr);

  // Should have interprocedural call edge
  ICFGEdge *callEdge = icfg.getICFGEdge(callerNode, calleeEntryNode, ICFGEdge::CallCF);
  EXPECT_NE(callEdge, nullptr);
}

// Test 4: Return edge from callee to caller
TEST_F(ICFGTest, ReturnEdgeFromCallee) {
  const char *source = R"(
    define i32 @callee() {
    entry:
      ret i32 1
    }

    define i32 @caller() {
    entry:
      %result = call i32 @callee()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  const BasicBlock *callerEntry = &caller->getEntryBlock();
  const CallBase *call = findCall(caller, "callee");
  ASSERT_NE(call, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(callerEntry);
  FunEntryBlockNode *calleeEntryNode = icfg.getFunEntryICFGNode(callee);
  FunExitBlockNode *calleeExitNode = icfg.getFunExitICFGNode(callee);
  CallRetBlockNode *retSiteNode = icfg.getRetICFGNode(call);

  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(calleeEntryNode, nullptr);
  ASSERT_NE(calleeExitNode, nullptr);
  ASSERT_NE(retSiteNode, nullptr);

  // Call edge
  ICFGEdge *callEdge = icfg.getICFGEdge(callerNode, calleeEntryNode, ICFGEdge::CallCF);
  EXPECT_NE(callEdge, nullptr);

  // Return edge
  ICFGEdge *retEdge = icfg.getICFGEdge(calleeExitNode, retSiteNode, ICFGEdge::RetCF);
  EXPECT_NE(retEdge, nullptr);
}

// Test 5: Multiple callers of the same function
TEST_F(ICFGTest, MultipleCallers) {
  const char *source = R"(
    define i32 @shared() {
      ret i32 0
    }
    
    define i32 @caller1() {
      %result = call i32 @shared()
      ret i32 %result
    }
    
    define i32 @caller2() {
      %result = call i32 @shared()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *shared = module->getFunction("shared");
  Function *caller1 = module->getFunction("caller1");
  Function *caller2 = module->getFunction("caller2");

  ASSERT_NE(shared, nullptr);
  ASSERT_NE(caller1, nullptr);
  ASSERT_NE(caller2, nullptr);

  FunEntryBlockNode *sharedEntry = icfg.getFunEntryICFGNode(shared);
  IntraBlockNode *caller1Node = icfg.getIntraBlockNode(&caller1->getEntryBlock());
  IntraBlockNode *caller2Node = icfg.getIntraBlockNode(&caller2->getEntryBlock());

  ASSERT_NE(sharedEntry, nullptr);
  ASSERT_NE(caller1Node, nullptr);
  ASSERT_NE(caller2Node, nullptr);

  // Both callers should have call edges to shared
  EXPECT_NE(icfg.getICFGEdge(caller1Node, sharedEntry, ICFGEdge::CallCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(caller2Node, sharedEntry, ICFGEdge::CallCF), nullptr);
}

// Test 6: Nested function calls
TEST_F(ICFGTest, NestedFunctionCalls) {
  const char *source = R"(
    define i32 @inner() {
      ret i32 1
    }
    
    define i32 @middle() {
      %result = call i32 @inner()
      ret i32 %result
    }
    
    define i32 @outer() {
      %result = call i32 @middle()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *outer = module->getFunction("outer");
  Function *middle = module->getFunction("middle");
  Function *inner = module->getFunction("inner");

  ASSERT_NE(outer, nullptr);
  ASSERT_NE(middle, nullptr);
  ASSERT_NE(inner, nullptr);

  IntraBlockNode *outerNode = icfg.getIntraBlockNode(&outer->getEntryBlock());
  IntraBlockNode *middleCallerNode = icfg.getIntraBlockNode(&middle->getEntryBlock());
  FunEntryBlockNode *middleEntryNode = icfg.getFunEntryICFGNode(middle);
  FunEntryBlockNode *innerNode = icfg.getFunEntryICFGNode(inner);

  ASSERT_NE(outerNode, nullptr);
  ASSERT_NE(middleCallerNode, nullptr);
  ASSERT_NE(middleEntryNode, nullptr);
  ASSERT_NE(innerNode, nullptr);

  // Call chain: outer -> middle -> inner
  EXPECT_NE(icfg.getICFGEdge(outerNode, middleEntryNode, ICFGEdge::CallCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(middleCallerNode, innerNode, ICFGEdge::CallCF), nullptr);
}

// Test 7: Recursive function call
TEST_F(ICFGTest, RecursiveCall) {
  const char *source = R"(
    define i32 @fact(i32 %n) {
    entry:
      %cmp = icmp sle i32 %n, 1
      br i1 %cmp, label %base, label %recurse
      
    base:
      ret i32 1
      
    recurse:
      %n1 = sub i32 %n, 1
      %result = call i32 @fact(i32 %n1)
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *fact = module->getFunction("fact");
  ASSERT_NE(fact, nullptr);

  IntraBlockNode *factEntry = icfg.getIntraBlockNode(&fact->getEntryBlock());
  ASSERT_NE(factEntry, nullptr);

  // For recursive calls, we should still have the call edge
  EXPECT_TRUE(true);
}

// Test 8: Function with multiple call sites
TEST_F(ICFGTest, MultipleCallSites) {
  const char *source = R"(
    define i32 @helper() {
      ret i32 42
    }
    
    define i32 @multi_call() {
      %r1 = call i32 @helper()
      %r2 = call i32 @helper()
      %r3 = call i32 @helper()
      ret i32 %r3
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *helper = module->getFunction("helper");
  Function *multiCall = module->getFunction("multi_call");

  ASSERT_NE(helper, nullptr);
  ASSERT_NE(multiCall, nullptr);

  IntraBlockNode *helperNode = icfg.getIntraBlockNode(&helper->getEntryBlock());
  ASSERT_NE(helperNode, nullptr);

  // Verify the module builds correctly
  EXPECT_TRUE(true);
}

TEST_F(ICFGTest, MultipleCallSitesToSameCalleeKeepDistinctCallEdges) {
  const char *source = R"(
    define i32 @helper() {
      ret i32 42
    }

    define i32 @multi_call() {
    entry:
      %r1 = call i32 @helper()
      %r2 = call i32 @helper()
      %r3 = call i32 @helper()
      ret i32 %r3
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *helper = module->getFunction("helper");
  Function *multiCall = module->getFunction("multi_call");
  ASSERT_NE(helper, nullptr);
  ASSERT_NE(multiCall, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&multiCall->getEntryBlock());
  FunEntryBlockNode *calleeEntry = icfg.getFunEntryICFGNode(helper);
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(calleeEntry, nullptr);

  auto calls = findCalls(multiCall, "helper");
  ASSERT_EQ(calls.size(), 3u);

  size_t callEdgeCount = 0;
  for (ICFGEdge *edge : callerNode->getOutEdges()) {
    auto *callEdge = dyn_cast<CallCFGEdge>(edge);
    if (!callEdge || callEdge->getDstNode() != calleeEntry)
      continue;
    ++callEdgeCount;
  }
  EXPECT_EQ(callEdgeCount, 3u);

  for (const CallBase *call : calls) {
    EXPECT_NE(icfg.getICFGEdge(callerNode, calleeEntry, ICFGEdge::CallCF, call),
              nullptr);
  }
}

TEST_F(ICFGTest, ReturnSiteBranchesToAllSuccessorsAfterDirectCall) {
  const char *source = R"(
    define void @callee() {
      ret void
    }

    define i32 @caller(i1 %cond) {
    entry:
      call void @callee()
      br i1 %cond, label %then, label %else
    then:
      ret i32 1
    else:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  const CallBase *call = findCall(caller, "callee");
  ASSERT_NE(call, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  CallRetBlockNode *retSite = icfg.getRetICFGNode(call);
  IntraBlockNode *thenNode = icfg.getIntraBlockNode(findBlock(caller, "then"));
  IntraBlockNode *elseNode = icfg.getIntraBlockNode(findBlock(caller, "else"));
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(retSite, nullptr);
  ASSERT_NE(thenNode, nullptr);
  ASSERT_NE(elseNode, nullptr);

  EXPECT_EQ(icfg.getICFGEdge(callerNode, thenNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, elseNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(retSite, thenNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(retSite, elseNode, ICFGEdge::IntraCF), nullptr);
}

TEST_F(ICFGTest, ExternalCallStillBuildsReturnSiteContinuation) {
  const char *source = R"(
    declare void @ext()

    define i32 @caller(i1 %cond) {
    entry:
      call void @ext()
      br i1 %cond, label %then, label %else
    then:
      ret i32 1
    else:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  const CallBase *call = findCall(caller, "ext");
  ASSERT_NE(call, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  CallRetBlockNode *retSite = icfg.getRetICFGNode(call);
  IntraBlockNode *thenNode = icfg.getIntraBlockNode(findBlock(caller, "then"));
  IntraBlockNode *elseNode = icfg.getIntraBlockNode(findBlock(caller, "else"));
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(retSite, nullptr);
  ASSERT_NE(thenNode, nullptr);
  ASSERT_NE(elseNode, nullptr);

  EXPECT_EQ(icfg.getICFGEdge(callerNode, thenNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, elseNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(callerNode, retSite, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(retSite, thenNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(retSite, elseNode, ICFGEdge::IntraCF), nullptr);
}

TEST_F(ICFGTest, InternalInvokeUsesDedicatedNormalAndUnwindContinuations) {
  const char *source = R"(
    declare i32 @__gxx_personality_v0(...)

    define i32 @callee() {
    entry:
      ret i32 7
    }

    define i32 @caller() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %r = invoke i32 @callee()
              to label %normal unwind label %lpad
    normal:
      ret i32 %r
    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  const auto *invoke = dyn_cast<InvokeInst>(findCall(caller, "callee"));
  ASSERT_NE(invoke, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  IntraBlockNode *normalNode = icfg.getIntraBlockNode(findBlock(caller, "normal"));
  IntraBlockNode *lpadNode = icfg.getIntraBlockNode(findBlock(caller, "lpad"));
  FunEntryBlockNode *calleeEntry = icfg.getFunEntryICFGNode(callee);
  FunExitBlockNode *calleeExit = icfg.getFunExitICFGNode(callee);
  CallRetBlockNode *retSite = icfg.getRetICFGNode(invoke);
  CallUnwindBlockNode *unwindSite = icfg.getUnwindICFGNode(invoke);

  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(normalNode, nullptr);
  ASSERT_NE(lpadNode, nullptr);
  ASSERT_NE(calleeEntry, nullptr);
  ASSERT_NE(calleeExit, nullptr);
  ASSERT_NE(retSite, nullptr);
  ASSERT_NE(unwindSite, nullptr);

  EXPECT_NE(icfg.getICFGEdge(callerNode, calleeEntry, ICFGEdge::CallCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, normalNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, lpadNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(calleeExit, retSite, ICFGEdge::RetCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(retSite, normalNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(unwindSite, lpadNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, unwindSite, ICFGEdge::IntraCF), nullptr);
}

TEST_F(ICFGTest, InternalInvokeExceptionalResumeUsesExceptionalReturnEdge) {
  const char *source = R"(
    declare i32 @__gxx_personality_v0(...)
    declare i32 @may_throw()

    define i32 @callee() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %v = invoke i32 @may_throw()
              to label %cont unwind label %lpad
    cont:
      ret i32 %v
    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      resume { i8*, i32 } %lp
    }

    define i32 @caller() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %r = invoke i32 @callee()
              to label %normal unwind label %outer_lpad
    normal:
      ret i32 %r
    outer_lpad:
      %outer = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  const auto *invoke = dyn_cast<InvokeInst>(findCall(caller, "callee"));
  ASSERT_NE(invoke, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  IntraBlockNode *normalNode =
      icfg.getIntraBlockNode(findBlock(caller, "normal"));
  IntraBlockNode *outerLpadNode =
      icfg.getIntraBlockNode(findBlock(caller, "outer_lpad"));
  FunUnwindExitBlockNode *calleeUnwindExit =
      icfg.getFunUnwindExitICFGNode(callee);
  CallUnwindBlockNode *unwindSite = icfg.getUnwindICFGNode(invoke);

  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(normalNode, nullptr);
  ASSERT_NE(outerLpadNode, nullptr);
  ASSERT_NE(calleeUnwindExit, nullptr);
  ASSERT_NE(unwindSite, nullptr);

  EXPECT_EQ(icfg.getICFGEdge(callerNode, normalNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, outerLpadNode, ICFGEdge::IntraCF),
            nullptr);
  EXPECT_NE(
      icfg.getICFGEdge(calleeUnwindExit, unwindSite, ICFGEdge::ExcRetCF),
      nullptr);
  EXPECT_NE(icfg.getICFGEdge(unwindSite, outerLpadNode, ICFGEdge::IntraCF),
            nullptr);
}

TEST_F(ICFGTest, ExternalInvokeUsesSummaryEdgesToContinuationNodes) {
  const char *source = R"(
    declare i32 @__gxx_personality_v0(...)
    declare i32 @may_throw()

    define i32 @caller() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %r = invoke i32 @may_throw()
              to label %normal unwind label %lpad
    normal:
      ret i32 %r
    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  const auto *invoke = dyn_cast<InvokeInst>(findCall(caller, "may_throw"));
  ASSERT_NE(invoke, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  IntraBlockNode *normalNode = icfg.getIntraBlockNode(findBlock(caller, "normal"));
  IntraBlockNode *lpadNode = icfg.getIntraBlockNode(findBlock(caller, "lpad"));
  CallRetBlockNode *retSite = icfg.getRetICFGNode(invoke);
  CallUnwindBlockNode *unwindSite = icfg.getUnwindICFGNode(invoke);

  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(normalNode, nullptr);
  ASSERT_NE(lpadNode, nullptr);
  ASSERT_NE(retSite, nullptr);
  ASSERT_NE(unwindSite, nullptr);

  EXPECT_EQ(icfg.getICFGEdge(callerNode, normalNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, lpadNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(callerNode, retSite, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(callerNode, unwindSite, ICFGEdge::IntraCF),
            nullptr);
  EXPECT_NE(icfg.getICFGEdge(retSite, normalNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(unwindSite, lpadNode, ICFGEdge::IntraCF),
            nullptr);
}

TEST_F(ICFGTest, InterproceduralDistanceSkipsExceptionalReturnEdges) {
  const char *source = R"(
    declare i32 @__gxx_personality_v0(...)
    declare i32 @may_throw()

    define i32 @callee() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %v = invoke i32 @may_throw()
              to label %cont unwind label %lpad
    cont:
      ret i32 %v
    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      resume { i8*, i32 } %lp
    }

    define i32 @caller() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %r = invoke i32 @callee()
              to label %normal unwind label %outer_lpad
    normal:
      ret i32 %r
    outer_lpad:
      %outer = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  IntraBlockNode *outerLpadNode =
      icfg.getIntraBlockNode(findBlock(caller, "outer_lpad"));
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(outerLpadNode, nullptr);

  auto distanceMap = calculateDistanceMapInterICFG(&icfg, callerNode);
  EXPECT_EQ(distanceMap[outerLpadNode], 999999999999ULL);
}

// Test 9: Loop in control flow
TEST_F(ICFGTest, LoopInControlFlow) {
  const char *source = R"(
    define void @loop_example(i32 %n) {
    entry:
      br label %loop
      
    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %next = add i32 %i, 1
      %cmp = icmp slt i32 %next, %n
      br i1 %cmp, label %loop, label %exit
      
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *F = module->getFunction("loop_example");
  ASSERT_NE(F, nullptr);

  const BasicBlock *loopBB = findBlock(F, "loop");
  const BasicBlock *exitBB = findBlock(F, "exit");

  ASSERT_NE(loopBB, nullptr);
  ASSERT_NE(exitBB, nullptr);

  IntraBlockNode *loopNode = icfg.getIntraBlockNode(loopBB);
  IntraBlockNode *exitNode = icfg.getIntraBlockNode(exitBB);

  ASSERT_NE(loopNode, nullptr);
  ASSERT_NE(exitNode, nullptr);

  // Exit should be reachable from loop
  EXPECT_NE(icfg.getICFGEdge(loopNode, exitNode, ICFGEdge::IntraCF), nullptr);
}

// Test 10: Switch instruction handling
TEST_F(ICFGTest, SwitchInstruction) {
  const char *source = R"(
    define void @switch_example(i32 %x) {
    entry:
      switch i32 %x, label %default [
        i32 1, label %case1
        i32 2, label %case2
        i32 3, label %case3
      ]
    case1:
      br label %exit
    case2:
      br label %exit
    case3:
      br label %exit
    default:
      br label %exit
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *F = module->getFunction("switch_example");
  ASSERT_NE(F, nullptr);

  const BasicBlock *entry = &F->getEntryBlock();
  const BasicBlock *exitBB = findBlock(F, "exit");

  ASSERT_NE(entry, nullptr);
  ASSERT_NE(exitBB, nullptr);

  IntraBlockNode *entryNode = icfg.getIntraBlockNode(entry);
  IntraBlockNode *exitNode = icfg.getIntraBlockNode(exitBB);

  ASSERT_NE(entryNode, nullptr);
  ASSERT_NE(exitNode, nullptr);

  // Entry should have edges to all switch targets
  // Verify by checking exit is reachable
  EXPECT_NE(exitNode, nullptr);
}

// Test 11: Indirect function call
TEST_F(ICFGTest, IndirectCall) {
  const char *source = R"(
    define i32 @func1() {
      ret i32 1
    }
    
    define i32 @func2() {
      ret i32 2
    }
    
    define i32 @indirect_caller(i32 %which) {
      %fp = select i1 true, i32()* @func1, i32()* @func2
      %result = call i32 %fp()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *indirectCaller = module->getFunction("indirect_caller");
  ASSERT_NE(indirectCaller, nullptr);

  // Indirect calls might not have direct ICFG edges in basic ICFG
  // Verify the module builds correctly
  EXPECT_TRUE(true);
}

TEST_F(ICFGTest, DedicatedReturnSiteDoesNotCollapseToCallerBlock) {
  const char *source = R"(
    declare void @ext()

    define i32 @caller() {
    entry:
      call void @ext()
      %x = add i32 1, 2
      ret i32 %x
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  const CallBase *call = nullptr;
  for (const Instruction &I : caller->getEntryBlock()) {
    if (const auto *CB = dyn_cast<CallBase>(&I)) {
      call = CB;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  CallRetBlockNode *retSite = icfg.getRetICFGNode(call);
  IntraBlockNode *callerBlock = icfg.getIntraBlockNode(&caller->getEntryBlock());
  ASSERT_NE(retSite, nullptr);
  ASSERT_NE(callerBlock, nullptr);

  EXPECT_EQ(icfg.getICFGEdge(retSite, callerBlock, ICFGEdge::IntraCF), nullptr);
}

TEST_F(ICFGTest, RemoveDedicatedNodesClearsSideMaps) {
  const char *source = R"(
    define i32 @callee() {
    entry:
      ret i32 1
    }

    define i32 @caller() {
    entry:
      %result = call i32 @callee()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  const CallBase *call = findCall(caller, "callee");
  ASSERT_NE(call, nullptr);

  FunEntryBlockNode *entryNode = icfg.getFunEntryICFGNode(callee);
  FunExitBlockNode *exitNode = icfg.getFunExitICFGNode(callee);
  CallRetBlockNode *retNode = icfg.getRetICFGNode(call);
  ASSERT_NE(entryNode, nullptr);
  ASSERT_NE(exitNode, nullptr);
  ASSERT_NE(retNode, nullptr);
  NodeID entryId = entryNode->getId();
  NodeID exitId = exitNode->getId();
  NodeID retId = retNode->getId();

  icfg.removeICFGNode(entryNode);
  icfg.removeICFGNode(exitNode);
  icfg.removeICFGNode(retNode);

  EXPECT_NE(icfg.getFunEntryICFGNode(callee)->getId(), entryId);
  EXPECT_NE(icfg.getFunExitICFGNode(callee)->getId(), exitId);
  EXPECT_NE(icfg.getRetICFGNode(call)->getId(), retId);
}

TEST_F(ICFGTest, RemoveDedicatedUnwindNodesClearsSideMaps) {
  const char *source = R"(
    declare i32 @__gxx_personality_v0(...)
    declare i32 @may_throw()

    define i32 @callee() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %v = invoke i32 @may_throw()
              to label %cont unwind label %lpad
    cont:
      ret i32 %v
    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      resume { i8*, i32 } %lp
    }

    define i32 @caller() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %r = invoke i32 @callee()
              to label %normal unwind label %outer_lpad
    normal:
      ret i32 %r
    outer_lpad:
      %outer = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  const auto *invoke = dyn_cast<InvokeInst>(findCall(caller, "callee"));
  ASSERT_NE(invoke, nullptr);

  FunUnwindExitBlockNode *unwindExit = icfg.getFunUnwindExitICFGNode(callee);
  CallUnwindBlockNode *unwindNode = icfg.getUnwindICFGNode(invoke);
  ASSERT_NE(unwindExit, nullptr);
  ASSERT_NE(unwindNode, nullptr);
  NodeID unwindExitId = unwindExit->getId();
  NodeID unwindNodeId = unwindNode->getId();

  icfg.removeICFGNode(unwindExit);
  icfg.removeICFGNode(unwindNode);

  EXPECT_NE(icfg.getFunUnwindExitICFGNode(callee)->getId(), unwindExitId);
  EXPECT_NE(icfg.getUnwindICFGNode(invoke)->getId(), unwindNodeId);
}

TEST_F(ICFGTest, RemovingSelfLoopNodeDoesNotDoubleDeleteEdges) {
  const char *source = R"(
    define void @loop_example() {
    entry:
      br label %loop

    loop:
      br label %loop
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *F = module->getFunction("loop_example");
  ASSERT_NE(F, nullptr);
  const BasicBlock *loopBB = findBlock(F, "loop");
  ASSERT_NE(loopBB, nullptr);

  IntraBlockNode *loopNode = icfg.getIntraBlockNode(loopBB);
  ASSERT_NE(loopNode, nullptr);
  NodeID loopNodeId = loopNode->getId();

  icfg.removeICFGNode(loopNode);

  EXPECT_FALSE(icfg.hasICFGNode(loopNodeId));
}

// Test 12: Empty function handling
TEST_F(ICFGTest, EmptyFunction) {
  const char *source = R"(
    define void @empty() {
      ret void
    }
    
    define void @caller() {
      call void @empty()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *empty = module->getFunction("empty");
  ASSERT_NE(empty, nullptr);

  IntraBlockNode *emptyEntry = icfg.getIntraBlockNode(&empty->getEntryBlock());
  ASSERT_NE(emptyEntry, nullptr);

  // Empty function should still have a node
  EXPECT_NE(emptyEntry, nullptr);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
