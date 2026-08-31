#include "SVFGMemorySSATestSupport.h"

TEST_F(SVFGMemorySSATest, ReadOnlyCalleeDoesNotCreateCallerSideDefs) {
  const char *source = R"(
    define i8 @reader(i8* %p) {
    entry:
      %v = load i8, i8* %p
      ret i8 %v
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      %v = call i8 @reader(i8* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *mainFn = getFunctionChecked(*module, "main");
  const CallBase *call = findCallTo(mainFn, "reader");
  ASSERT_NE(call, nullptr);

  EXPECT_FALSE(svfg->getActualIns(call).empty());
  EXPECT_TRUE(svfg->getActualOuts(call).empty());

  size_t callMuCount = 0;
  size_t callChiCount = 0;
  for (const auto &pair : *svfg) {
    if (pair.second->getNodeKind() == SVFGK::CallMu) {
      if (pair.second->getCallSite() == call)
        ++callMuCount;
    } else if (pair.second->getNodeKind() == SVFGK::CallChi) {
      if (pair.second->getCallSite() == call)
        ++callChiCount;
    }
  }

  EXPECT_EQ(callMuCount, 0u);
  EXPECT_EQ(callChiCount, 0u);
}
TEST_F(SVFGMemorySSATest, SameReachingDefDoesNotCreateMemoryPhi) {
  const char *source = R"(
    define i32 @main(i1 %cond) {
    entry:
      %x = alloca i8
      br i1 %cond, label %join, label %mid

    mid:
      br label %join

    join:
      %v = load i8, i8* %x
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  size_t phiCount = 0;
  for (const auto &pair : *svfg) {
    if (isa<IntraMSSAPhiSVFGNode>(pair.second))
      ++phiCount;
  }

  EXPECT_EQ(phiCount, 0u);
}
TEST_F(SVFGMemorySSATest, DistinctReachingDefsCreateMemoryPhi) {
  const char *source = R"(
    define i32 @main(i1 %cond) {
    entry:
      %x = alloca i8
      br i1 %cond, label %then, label %join

    then:
      store i8 1, i8* %x
      br label %join

    join:
      %v = load i8, i8* %x
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  size_t phiCount = 0;
  bool sawTwoOperandPhi = false;
  for (const auto &pair : *svfg) {
    auto *phi = dyn_cast<IntraMSSAPhiSVFGNode>(pair.second);
    if (!phi)
      continue;
    ++phiCount;
    if (phi->getOpVerNum() == 2)
      sawTwoOperandPhi = true;
  }

  EXPECT_GT(phiCount, 0u);
  EXPECT_TRUE(sawTwoOperandPhi);
}
TEST_F(SVFGMemorySSATest, MemoryPhiIncomingEdgesAreGuardedIndirectFlow) {
  const char *source = R"(
    define i32 @main(i1 %cond) {
    entry:
      %x = alloca i8
      br i1 %cond, label %then, label %join

    then:
      store i8 1, i8* %x
      br label %join

    join:
      %v = load i8, i8* %x
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  bool sawIncomingIndirectPhiEdge = false;
  for (const auto &pair : *svfg) {
    auto *phi = dyn_cast<IntraMSSAPhiSVFGNode>(pair.second);
    if (!phi)
      continue;

    for (SVFGEdge *edge : phi->getInEdges()) {
      ASSERT_NE(edge, nullptr);
      EXPECT_EQ(edge->getEdgeKind(), SVFGEdgeK::IntraIndirect);
      EXPECT_TRUE(isIntraVFGEdge(edge->getEdgeKind()));
      EXPECT_TRUE(isIndirectVFGEdge(edge->getEdgeKind()));
      if (edge->getEdgeKind() == SVFGEdgeK::IntraIndirect)
        sawIncomingIndirectPhiEdge = true;
    }
  }

  EXPECT_TRUE(sawIncomingIndirectPhiEdge);
}
TEST_F(SVFGMemorySSATest, LoadCapturesReachingDefVersion) {
  const char *source = R"(
    @g = global i8* null

    define i32 @main() {
    entry:
      %x = alloca i8
      store i8* %x, i8** @g
      %v = load i8*, i8** @g
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *mainFn = getFunctionChecked(*module, "main");
  const LoadInst *load = findSingleLoad(mainFn);
  ASSERT_NE(load, nullptr);

  auto *loadStmt = dyn_cast_or_null<LoadSVFGNode>(svfg->getDef(load));
  ASSERT_NE(loadStmt, nullptr);
  EXPECT_NE(loadStmt->getMemoryUseReg(), 0u);

  bool versionMatchesIncomingDef = false;
  for (SVFGEdge *edge : loadStmt->getInEdges()) {
    if (!edge || edge->getEdgeKind() != SVFGEdgeK::IntraIndirect)
      continue;
    uint32_t srcVersion = 0;
    if (auto *srcMem = dyn_cast<MSSASVFGNode>(edge->getSrcNode()))
      srcVersion = srcMem->getSSAVersion();
    else if (auto *srcStore = dyn_cast<StoreSVFGNode>(edge->getSrcNode()))
      srcVersion = srcStore->getMemoryDefVersion();
    if (srcVersion == loadStmt->getMemoryUseVersion())
      versionMatchesIncomingDef = true;
  }

  EXPECT_TRUE(versionMatchesIncomingDef);
}
TEST_F(SVFGMemorySSATest, GlobalOnlyCalleeCreatesInterproceduralMemoryNodes) {
  const char *source = R"(
    @g = global i8 0

    define void @writer() {
    entry:
      store i8 1, i8* @g
      ret void
    }

    define i32 @main() {
    entry:
      call void @writer()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *mainFn = module->getFunction("main");
  const Function *writerFn = module->getFunction("writer");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(writerFn, nullptr);
  const CallBase *call = findCallTo(mainFn, "writer");
  ASSERT_NE(call, nullptr);

  // A MOD region is also an input to the callee: weak updates must receive
  // the reaching memory version even when the old value is not explicitly
  // read in the callee body.
  EXPECT_FALSE(svfg->getActualIns(call).empty());
  EXPECT_FALSE(svfg->getActualOuts(call).empty());
  EXPECT_FALSE(svfg->getFormalOuts(writerFn).empty());

  const ICFGNode *globalInitNode = icfg.getGlobalInitICFGNode();
  ASSERT_NE(globalInitNode, nullptr);
  bool sawGlobalEntryChi = false;
  bool sawStoreSeedIntoMain = false;
  for (const auto &pair : *svfg) {
    EXPECT_NE(pair.second->getNodeKind(), SVFGK::CallMu);
    EXPECT_NE(pair.second->getNodeKind(), SVFGK::CallChi);
    EXPECT_NE(pair.second->getNodeKind(), SVFGK::RetMu);
    if (pair.second->getNodeKind() == SVFGK::EntryChi) {
      EXPECT_EQ(pair.second->getICFGNode(), globalInitNode);
      sawGlobalEntryChi = true;
    }
  }
  EXPECT_FALSE(sawGlobalEntryChi);

  for (SVFGNode *formalInNode : svfg->getFormalIns(mainFn)) {
    for (SVFGEdge *edge : formalInNode->getInEdges()) {
      if (edge && edge->getEdgeKind() == SVFGEdgeK::IntraIndirect &&
          isa<StoreSVFGNode>(edge->getSrcNode())) {
        sawStoreSeedIntoMain = true;
        break;
      }
    }
  }
  EXPECT_TRUE(sawStoreSeedIntoMain);
}
TEST_F(SVFGMemorySSATest, CallsiteMemoryNodesTrackOnlyTouchedArguments) {
  const char *source = R"(
    define i8 @touch_first(i8* %a, i8* %b) {
    entry:
      %v = load i8, i8* %a
      ret i8 %v
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      %y = alloca i8
      store i8 1, i8* %x
      %v = call i8 @touch_first(i8* %x, i8* %y)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const CallBase *call = findCallTo(mainFn, "touch_first");
  ASSERT_NE(call, nullptr);

  EXPECT_EQ(svfg->getActualIns(call).size(), 1u);
  EXPECT_TRUE(svfg->getActualOuts(call).empty());

  auto *actualIn =
      dyn_cast<ActualInSVFGNode>(*svfg->getActualIns(call).begin());
  ASSERT_NE(actualIn, nullptr);

  bool actualInVersionMatchesIncomingDef = false;
  for (SVFGEdge *edge : actualIn->getInEdges()) {
    if (!edge)
      continue;
    uint32_t srcVersion = 0;
    if (auto *srcMem = dyn_cast<MSSASVFGNode>(edge->getSrcNode()))
      srcVersion = srcMem->getSSAVersion();
    else if (auto *srcStore = dyn_cast<StoreSVFGNode>(edge->getSrcNode()))
      srcVersion = srcStore->getMemoryDefVersion();
    if (srcVersion == actualIn->getSSAVersion())
      actualInVersionMatchesIncomingDef = true;
  }
  EXPECT_TRUE(actualInVersionMatchesIncomingDef);

  EXPECT_NE(actualIn->getMemReg(), 0u);
}
TEST_F(SVFGMemorySSATest, InterproceduralValueNodesUseEntryExitAndReturnSite) {
  const char *source = R"(
    declare i8* @sink(...)

    define i8* @id(i8* %p, ...) {
    entry:
      ret i8* %p
    }

    define i8* @caller(i8* %q) {
    entry:
      %r = call i8* (i8*, ...) @id(i8* %q, i8* %q)
      br label %exit

    exit:
      ret i8* %r
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = true;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *idFn = module->getFunction("id");
  const Function *callerFn = module->getFunction("caller");
  ASSERT_NE(idFn, nullptr);
  ASSERT_NE(callerFn, nullptr);

  const CallBase *call = findCallTo(callerFn, "id");
  ASSERT_NE(call, nullptr);

  const ICFGNode *entryNode = icfg.getFunEntryICFGNode(idFn);
  const ICFGNode *exitNode = icfg.getFunExitICFGNode(idFn);
  const ICFGNode *returnSiteNode = icfg.getRetICFGNode(call);

  bool sawFormalParm = false;
  bool sawVarArg = false;
  for (SVFGNode *node : svfg->getFormalParms(idFn)) {
    if (auto *formalParm = dyn_cast<FormalParmSVFGNode>(node)) {
      sawFormalParm = true;
      EXPECT_EQ(formalParm->getICFGNode(), entryNode);
    } else if (auto *varArg = dyn_cast<VarArgSVFGNode>(node)) {
      sawVarArg = true;
      EXPECT_EQ(varArg->getICFGNode(), entryNode);
    }
  }
  EXPECT_TRUE(sawFormalParm);
  EXPECT_TRUE(sawVarArg);

  const auto &formalRets = svfg->getFormalRets(idFn);
  ASSERT_EQ(formalRets.size(), 1u);
  auto *formalRet = dyn_cast<FormalRetSVFGNode>(*formalRets.begin());
  ASSERT_NE(formalRet, nullptr);
  EXPECT_EQ(formalRet->getICFGNode(), exitNode);

  const auto &actualRets = svfg->getActualRets(call);
  ASSERT_EQ(actualRets.size(), 1u);
  auto *actualRet = dyn_cast<ActualRetSVFGNode>(*actualRets.begin());
  ASSERT_NE(actualRet, nullptr);
  EXPECT_EQ(actualRet->getICFGNode(), returnSiteNode);
}
TEST_F(SVFGMemorySSATest, InterproceduralMemoryNodesUseExitAndReturnSite) {
  const char *source = R"(
    define void @writer(i8* %p) {
    entry:
      store i8 1, i8* %p
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      call void @writer(i8* %x)
      br label %exit

    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = true;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *mainFn = module->getFunction("main");
  const Function *writerFn = module->getFunction("writer");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(writerFn, nullptr);
  const CallBase *call = findCallTo(mainFn, "writer");
  ASSERT_NE(call, nullptr);

  const ICFGNode *exitNode = icfg.getFunExitICFGNode(writerFn);
  const ICFGNode *returnSiteNode = icfg.getRetICFGNode(call);

  const auto &formalOuts = svfg->getFormalOuts(writerFn);
  ASSERT_FALSE(formalOuts.empty());
  for (SVFGNode *node : formalOuts) {
    auto *formalOut = dyn_cast<FormalOutSVFGNode>(node);
    ASSERT_NE(formalOut, nullptr);
    EXPECT_EQ(formalOut->getICFGNode(), exitNode);
  }

  const auto &actualOuts = svfg->getActualOuts(call);
  ASSERT_FALSE(actualOuts.empty());
  for (SVFGNode *node : actualOuts) {
    auto *actualOut = dyn_cast<ActualOutSVFGNode>(node);
    ASSERT_NE(actualOut, nullptr);
    EXPECT_EQ(actualOut->getICFGNode(), returnSiteNode);
  }
}
TEST_F(SVFGMemorySSATest, GlobalEntryFallbackCoversAllDirectUsersWithoutMain) {
  const char *source = R"(
    @g = global i8 0

    define void @foo() {
    entry:
      store i8 1, i8* @g
      ret void
    }

    define void @bar() {
    entry:
      %v = load i8, i8* @g
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = true;
  cfg.includeGlobals = true;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *fooFn = module->getFunction("foo");
  const Function *barFn = module->getFunction("bar");
  ASSERT_NE(fooFn, nullptr);
  ASSERT_NE(barFn, nullptr);

  ASSERT_FALSE(svfg->getFormalIns(fooFn).empty());
  ASSERT_FALSE(svfg->getFormalIns(barFn).empty());
  const ICFGNode *globalInitNode = icfg.getGlobalInitICFGNode();
  ASSERT_NE(globalInitNode, nullptr);

  std::vector<SVFGNode *> entryChiNodes;
  for (const auto &pair : *svfg) {
    if (pair.second && pair.second->getNodeKind() == SVFGK::EntryChi &&
        pair.second->getICFGNode() == globalInitNode) {
      entryChiNodes.push_back(pair.second);
    }
  }
  ASSERT_FALSE(entryChiNodes.empty());

  auto hasIncomingFromGlobalEntry = [&](const SVFGNodeSet &formalIns) {
    for (SVFGNode *node : formalIns) {
      for (SVFGEdge *edge : node->getInEdges()) {
        if (edge && edge->getEdgeKind() == SVFGEdgeK::IntraIndirect &&
            std::find(entryChiNodes.begin(), entryChiNodes.end(),
                      edge->getSrcNode()) != entryChiNodes.end()) {
          return true;
        }
      }
    }
    return false;
  };

  EXPECT_TRUE(hasIncomingFromGlobalEntry(svfg->getFormalIns(fooFn)));
  EXPECT_TRUE(hasIncomingFromGlobalEntry(svfg->getFormalIns(barFn)));
}
TEST_F(SVFGMemorySSATest, OnTheFlyIndirectCallUpdatesRefinedCallGraph) {
  const char *source = R"(
    define void @target(i8* %p) {
    entry:
      ret void
    }

    define void @apply(void (i8*)* %fp, i8* %arg) {
    entry:
      call void %fp(i8* %arg)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = false;
  cfg.resolveIndirectCalls = false;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *applyFn = module->getFunction("apply");
  const Function *targetFn = module->getFunction("target");
  ASSERT_NE(applyFn, nullptr);
  ASSERT_NE(targetFn, nullptr);

  const CallBase *indCall = findSingleIndirectCall(applyFn);
  ASSERT_NE(indCall, nullptr);

  std::vector<SVFGEdge *> newEdges;
  EXPECT_TRUE(builder.connectCallSiteToCalleeOnTheFly(svfg.get(), indCall,
                                                      targetFn, newEdges));
  bool sawSpeculativeEdge = false;
  for (SVFGEdge *edge : newEdges) {
    if (builder.isSpuriousVFEdgeAtIndCallSite(edge))
      sawSpeculativeEdge = true;
  }
  EXPECT_TRUE(sawSpeculativeEdge);
  builder.markValidVFEdges(newEdges);
  for (SVFGEdge *edge : newEdges)
    EXPECT_FALSE(builder.isSpuriousVFEdgeAtIndCallSite(edge));

  const LTCallGraph *cg = builder.getRefinedCallGraph();
  ASSERT_NE(cg, nullptr);
  EXPECT_TRUE(callGraphHasEdge(*cg, applyFn, indCall, targetFn));
  ASSERT_EQ(cg, svfg->getRefinedCallGraph());
}
TEST_F(SVFGMemorySSATest, UpdateSVFGKeepsBuilderGraphAccessorsValid) {
  const char *source = R"(
    define void @target(i8* %p) {
    entry:
      ret void
    }

    define void @apply(i8* %arg) {
    entry:
      call void @target(i8* %arg)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = false;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *applyFn = module->getFunction("apply");
  const Function *targetFn = module->getFunction("target");
  ASSERT_NE(applyFn, nullptr);
  ASSERT_NE(targetFn, nullptr);
  const CallBase *call = findCallTo(applyFn, "target");
  ASSERT_NE(call, nullptr);

  ASSERT_TRUE(builder.updateSVFG(svfg.get()));

  const LTCallGraph *cg = builder.getRefinedCallGraph();
  ASSERT_NE(cg, nullptr);
  ASSERT_EQ(cg, svfg->getRefinedCallGraph());
  EXPECT_TRUE(callGraphHasEdge(*cg, applyFn, call, targetFn));
}
TEST_F(SVFGMemorySSATest, SelectProducesPhiNodeAndPhiEdges) {
  const char *source = R"(
    define i8* @pick(i1 %cond, i8* %a, i8* %b) {
    entry:
      %sel = select i1 %cond, i8* %a, i8* %b
      ret i8* %sel
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *pickFn = module->getFunction("pick");
  ASSERT_NE(pickFn, nullptr);

  const SelectInst *selectInst = nullptr;
  for (const BasicBlock &BB : *pickFn) {
    for (const Instruction &I : BB) {
      if (const auto *SI = dyn_cast<SelectInst>(&I)) {
        selectInst = SI;
        break;
      }
    }
  }
  ASSERT_NE(selectInst, nullptr);

  SVFGNode *selectNode = svfg->getDef(selectInst);
  ASSERT_NE(selectNode, nullptr);
  EXPECT_TRUE(isa<IntraPhiSVFGNode>(selectNode));

  unsigned phiInEdges = 0;
  for (SVFGEdge *edge : selectNode->getInEdges()) {
    ASSERT_NE(edge, nullptr);
    if (edge->getEdgeKind() == SVFGEdgeK::IntraPhi)
      ++phiInEdges;
  }
  EXPECT_EQ(phiInEdges, 2u);
}
TEST_F(SVFGMemorySSATest, InternalPointerReturningCallDoesNotCopyArgumentIntoResult) {
  const char *source = R"(
    define i8* @id(i8* %p) {
    entry:
      ret i8* %p
    }

    define i8* @main() {
    entry:
      %x = alloca i8
      %r = call i8* @id(i8* %x)
      ret i8* %r
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const CallBase *call = findCallTo(mainFn, "id");
  ASSERT_NE(call, nullptr);

  SVFGNode *callValue = svfg->getDef(call);
  ASSERT_NE(callValue, nullptr);
  ASSERT_EQ(svfg->getActualRets(call).size(), 1u);

  bool sawActualRetBridge = false;
  bool sawArgumentCopy = false;
  for (SVFGEdge *edge : callValue->getInEdges()) {
    ASSERT_NE(edge, nullptr);
    if (isa<ActualRetSVFGNode>(edge->getSrcNode()))
      sawActualRetBridge = true;
    if (isa<AddrSVFGNode>(edge->getSrcNode()) &&
        edge->getEdgeKind() == SVFGEdgeK::IntraCopy)
      sawArgumentCopy = true;
  }

  EXPECT_TRUE(sawActualRetBridge);
  EXPECT_FALSE(sawArgumentCopy);
}
TEST_F(SVFGMemorySSATest, ExternalPointerReturningCallSkipsActualRetNode) {
  const char *source = R"(
    declare i8* @ext(i8*)

    define i8* @main() {
    entry:
      %x = alloca i8
      %r = call i8* @ext(i8* %x)
      ret i8* %r
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const CallBase *call = findCallTo(mainFn, "ext");
  ASSERT_NE(call, nullptr);

  EXPECT_TRUE(svfg->getActualRets(call).empty());
  ASSERT_NE(svfg->getDef(call), nullptr);
}
TEST_F(SVFGMemorySSATest, HeapReachableFromGlobalRemainsVisibleInSummaries) {
  const char *source = R"(
    @gp = global i8* null
    declare noalias i8* @malloc(i64)

    define void @init() {
    entry:
      %p = call i8* @malloc(i64 4)
      store i8* %p, i8** @gp
      ret void
    }

    define i8 @reader() {
    entry:
      %p = load i8*, i8** @gp
      %v = load i8, i8* %p
      ret i8 %v
    }

    define i8 @main() {
    entry:
      call void @init()
      %v = call i8 @reader()
      ret i8 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = true;
  cfg.buildMSSA = true;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *readerFn = module->getFunction("reader");
  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(readerFn, nullptr);
  ASSERT_NE(mainFn, nullptr);

  bool foundHeapSummaryRegion = false;
  for (SVFGNode *node : svfg->getFormalIns(readerFn)) {
    auto *formalIn = dyn_cast<FormalInSVFGNode>(node);
    ASSERT_NE(formalIn, nullptr);
    for (uint32_t objId : formalIn->getDefSVFVars()) {
      if (svfg->isHeapObject(objId)) {
        foundHeapSummaryRegion = true;
        break;
      }
    }
    if (foundHeapSummaryRegion)
      break;
  }

  EXPECT_TRUE(foundHeapSummaryRegion);

  const CallBase *readerCall = findCallTo(mainFn, "reader");
  ASSERT_NE(readerCall, nullptr);
  bool callerSeesHeapSummary = false;
  for (SVFGNode *node : svfg->getActualIns(readerCall)) {
    auto *actualIn = dyn_cast<ActualInSVFGNode>(node);
    ASSERT_NE(actualIn, nullptr);
    for (uint32_t objId : actualIn->getDefSVFVars()) {
      if (svfg->isHeapObject(objId)) {
        callerSeesHeapSummary = true;
        break;
      }
    }
    if (callerSeesHeapSummary)
      break;
  }

  EXPECT_TRUE(callerSeesHeapSummary);
}
TEST_F(SVFGMemorySSATest, MultiReturnFunctionUsesDedicatedExitNode) {
  const char *source = R"(
    define i8* @pick(i1 %cond, i8* %a, i8* %b) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret i8* %a
    else:
      ret i8* %b
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = true;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *pickFn = module->getFunction("pick");
  ASSERT_NE(pickFn, nullptr);

  const ICFGNode *exitNode = icfg.getFunExitICFGNode(pickFn);
  ASSERT_NE(exitNode, nullptr);

  const auto &formalRets = svfg->getFormalRets(pickFn);
  ASSERT_EQ(formalRets.size(), 1u);
  auto *formalRet = dyn_cast<FormalRetSVFGNode>(*formalRets.begin());
  ASSERT_NE(formalRet, nullptr);
  EXPECT_EQ(formalRet->getICFGNode(), exitNode);
}
TEST_F(SVFGMemorySSATest, NoReturnFunctionMemorySummaryUsesDedicatedExitNode) {
  const char *source = R"(
    declare void @abort() noreturn

    define void @die(i8* %p) {
    entry:
      store i8 1, i8* %p
      call void @abort()
      unreachable
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = true;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> svfg(builder.build(&icfg));
  ASSERT_NE(svfg, nullptr);

  const Function *dieFn = module->getFunction("die");
  ASSERT_NE(dieFn, nullptr);

  const ICFGNode *exitNode = icfg.getFunExitICFGNode(dieFn);
  ASSERT_NE(exitNode, nullptr);

  const auto &formalOuts = svfg->getFormalOuts(dieFn);
  ASSERT_FALSE(formalOuts.empty());
  for (SVFGNode *node : formalOuts) {
    auto *formalOut = dyn_cast<FormalOutSVFGNode>(node);
    ASSERT_NE(formalOut, nullptr);
    EXPECT_EQ(formalOut->getICFGNode(), exitNode);
  }
}
TEST_F(SVFGMemorySSATest, UntouchedPointerFormalsDoNotCreateMemoryNodes) {
  const char *source = R"(
    define void @noop(i8* %p) {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      call void @noop(i8* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  std::unique_ptr<SVFG> svfg = buildSVFG(module.get(), icfg);
  ASSERT_NE(svfg, nullptr);

  const Function *noopFn = module->getFunction("noop");
  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(noopFn, nullptr);
  ASSERT_NE(mainFn, nullptr);
  const CallBase *call = findCallTo(mainFn, "noop");
  ASSERT_NE(call, nullptr);

  EXPECT_TRUE(svfg->getFormalIns(noopFn).empty());
  EXPECT_TRUE(svfg->getFormalOuts(noopFn).empty());
  EXPECT_TRUE(svfg->getActualIns(call).empty());
  EXPECT_TRUE(svfg->getActualOuts(call).empty());
}
