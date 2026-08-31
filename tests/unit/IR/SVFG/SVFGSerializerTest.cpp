#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGNode.h"
#include "IR/SVFG/SVFGOPT.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <llvm/IR/Instructions.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::unittest;

namespace {

class SVFGSerializerTest : public LlvmModuleTest {
protected:
  static const LoadInst *findSingleLoad(const Function *F) {
    for (const BasicBlock &BB : *F)
      for (const Instruction &I : BB)
        if (const auto *LI = dyn_cast<LoadInst>(&I))
          return LI;
    return nullptr;
  }

  static const StoreInst *findSingleStore(const Function *F) {
    for (const BasicBlock &BB : *F)
      for (const Instruction &I : BB)
        if (const auto *SI = dyn_cast<StoreInst>(&I))
          return SI;
    return nullptr;
  }
};

TEST_F(SVFGSerializerTest, RoundTripsSemanticBindings) {
  const char *source = R"(
    define i8* @id(i8* %p) {
    entry:
      %v = load i8, i8* %p
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      %p = alloca i8*
      store i8* %x, i8** %p
      %q = load i8*, i8** %p
      %r = call i8* @id(i8* %q)
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
  std::unique_ptr<SVFG> original(builder.build(&icfg));
  ASSERT_NE(original, nullptr);

  SmallString<256> path;
  int fd = -1;
  ASSERT_FALSE(
      sys::fs::createTemporaryFile("svfg-serializer", "txt", fd, path));
  ::close(fd);

  ASSERT_TRUE(original->writeToFile(path.str().str()));

  SVFG reloaded;
  reloaded.setICFG(&icfg);
  ASSERT_TRUE(reloaded.readFromFile(path.str().str()));
  sys::fs::remove(path);

  Function *mainFn = module->getFunction("main");
  Function *idFn = module->getFunction("id");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(idFn, nullptr);

  const LoadInst *load = findSingleLoad(mainFn);
  const StoreInst *store = findSingleStore(mainFn);
  const CallBase *call = findCallTo(mainFn, "id");
  ASSERT_NE(load, nullptr);
  ASSERT_NE(store, nullptr);
  ASSERT_NE(call, nullptr);

  SVFGNode *loadNode = reloaded.getDef(load);
  ASSERT_NE(loadNode, nullptr);
  EXPECT_EQ(loadNode->getNodeKind(), SVFGK::Load);
  EXPECT_EQ(loadNode->getInstruction(), load);
  ASSERT_NE(loadNode->getICFGNode(), nullptr);
  auto *loadStmt = dyn_cast<LoadSVFGNode>(loadNode);
  ASSERT_NE(loadStmt, nullptr);
  EXPECT_NE(loadStmt->getMemoryUseReg(), 0u);
  EXPECT_FALSE(loadStmt->getMemoryPointsTo().empty());

  SVFGNode *storeNode = reloaded.getDef(store);
  ASSERT_NE(storeNode, nullptr);
  auto *storeStmt = dyn_cast<StoreSVFGNode>(storeNode);
  ASSERT_NE(storeStmt, nullptr);
  EXPECT_NE(storeStmt->getMemoryDefReg(), 0u);
  EXPECT_FALSE(storeStmt->getMemoryPointsTo().empty());

  const auto &actualParms = reloaded.getActualParms(call);
  ASSERT_FALSE(actualParms.empty());
  auto *actualParm = dyn_cast<ActualParmSVFGNode>(*actualParms.begin());
  ASSERT_NE(actualParm, nullptr);
  EXPECT_EQ(actualParm->getCallSite(), call);
  ASSERT_NE(actualParm->getICFGNode(), nullptr);
  EXPECT_NE(actualParm->getValueId(), 0u);

  const auto &actualIns = reloaded.getActualIns(call);
  ASSERT_FALSE(actualIns.empty());
  auto *actualIn = dyn_cast<ActualInSVFGNode>(*actualIns.begin());
  ASSERT_NE(actualIn, nullptr);
  EXPECT_EQ(actualIn->getCallSite(), call);

  const auto &formalParms = reloaded.getFormalParms(idFn);
  ASSERT_FALSE(formalParms.empty());
  auto *formalParm = dyn_cast<FormalParmSVFGNode>(*formalParms.begin());
  ASSERT_NE(formalParm, nullptr);
  EXPECT_EQ(formalParm->getFunction(), idFn);
  EXPECT_NE(formalParm->getValueId(), 0u);

  const Argument &arg0 = *idFn->arg_begin();
  EXPECT_EQ(reloaded.getValueNode(&arg0), formalParm);

  bool sawStoreToLoadFlow = false;
  for (SVFGEdge *edge : loadStmt->getInEdges()) {
    if (edge && edge->getSrcNode() == storeStmt &&
        edge->getEdgeKind() == SVFGEdgeK::IntraIndirect) {
      sawStoreToLoadFlow = true;
      break;
    }
  }
  EXPECT_TRUE(sawStoreToLoadFlow);

  std::vector<SVFGEdge *> interEdges;
  reloaded.getInterVFEdgesForIndirectCallSite(call, idFn, interEdges);
  EXPECT_FALSE(interEdges.empty());

  const AllocaInst *allocaX = nullptr;
  for (const BasicBlock &BB : *mainFn) {
    for (const Instruction &I : BB) {
      const auto *AI = dyn_cast<AllocaInst>(&I);
      if (!AI || !AI->getAllocatedType()->isIntegerTy(8))
        continue;
      allocaX = AI;
      break;
    }
    if (allocaX)
      break;
  }
  ASSERT_NE(allocaX, nullptr);

  auto *addrNode = dyn_cast_or_null<AddrSVFGNode>(reloaded.getValueNode(allocaX));
  ASSERT_NE(addrNode, nullptr);
  ASSERT_NE(addrNode->getObjectId(), 0u);
  const auto *info = reloaded.getObjectInfo(addrNode->getObjectId());
  ASSERT_NE(info, nullptr);
  EXPECT_TRUE(info->isStack);
  EXPECT_EQ(reloaded.getObjectValue(addrNode->getObjectId()), allocaX);
}

TEST_F(SVFGSerializerTest, RoundTripsInterPhiOperands) {
  const char *source = R"(
    define i8* @id(i8* %p) {
    entry:
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      %r = call i8* @id(i8* %x)
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

  SVFGOPT optimized;
  ASSERT_NE(optimized.buildAndOptimize(&icfg, cfg), nullptr);

  std::vector<uint32_t> originalInterPhiOperandCounts;
  for (const auto &pair : optimized) {
    auto *interPhi = dyn_cast<InterPhiSVFGNode>(pair.second);
    if (!interPhi)
      continue;
    originalInterPhiOperandCounts.push_back(interPhi->getOpVerNum());
  }
  std::sort(originalInterPhiOperandCounts.begin(),
            originalInterPhiOperandCounts.end());
  ASSERT_FALSE(originalInterPhiOperandCounts.empty());

  SmallString<256> path;
  int fd = -1;
  ASSERT_FALSE(
      sys::fs::createTemporaryFile("svfgopt-serializer", "txt", fd, path));
  ::close(fd);

  ASSERT_TRUE(optimized.writeToFile(path.str().str()));

  SVFGOPT reloaded;
  reloaded.setICFG(&icfg);
  ASSERT_TRUE(reloaded.readFromFile(path.str().str()));
  sys::fs::remove(path);

  bool foundInterPhi = false;
  std::vector<uint32_t> reloadedInterPhiOperandCounts;
  for (const auto &pair : reloaded) {
    auto *interPhi = dyn_cast<InterPhiSVFGNode>(pair.second);
    if (!interPhi)
      continue;
    foundInterPhi = true;
    reloadedInterPhiOperandCounts.push_back(interPhi->getOpVerNum());
    if (interPhi->isFormalParmPHI())
      EXPECT_NE(interPhi->getFunction(), nullptr);
    if (interPhi->isActualRetPHI())
      EXPECT_NE(interPhi->getCallSite(), nullptr);
  }

  EXPECT_TRUE(foundInterPhi);
  std::sort(reloadedInterPhiOperandCounts.begin(),
            reloadedInterPhiOperandCounts.end());
  EXPECT_EQ(reloadedInterPhiOperandCounts, originalInterPhiOperandCounts);
}

TEST_F(SVFGSerializerTest, DumpHonorsSimpleMode) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %p = alloca i8*
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
  cfg.buildMSSA = false;

  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  SmallString<256> simplePath;
  SmallString<256> verbosePath;
  int simpleFd = -1;
  int verboseFd = -1;
  ASSERT_FALSE(
      sys::fs::createTemporaryFile("svfg-simple", "dot", simpleFd, simplePath));
  ASSERT_FALSE(sys::fs::createTemporaryFile("svfg-verbose", "dot", verboseFd,
                                            verbosePath));
  ::close(simpleFd);
  ::close(verboseFd);

  graph->dump(simplePath.str().str(), true);
  graph->dump(verbosePath.str().str(), false);

  std::ifstream simpleFile(simplePath.c_str());
  std::ifstream verboseFile(verbosePath.c_str());
  ASSERT_TRUE(simpleFile.good());
  ASSERT_TRUE(verboseFile.good());

  std::stringstream simpleBuf;
  std::stringstream verboseBuf;
  simpleBuf << simpleFile.rdbuf();
  verboseBuf << verboseFile.rdbuf();

  sys::fs::remove(simplePath);
  sys::fs::remove(verbosePath);

  EXPECT_NE(simpleBuf.str(), verboseBuf.str());
  EXPECT_EQ(simpleBuf.str().find("Kind: "), std::string::npos);
  EXPECT_NE(verboseBuf.str().find("Kind: "), std::string::npos);
}

TEST_F(SVFGSerializerTest, SVFGOPTBuildAndWriteSerializesFullGraph) {
  const char *source = R"(
    define i8* @id(i8* %p) {
    entry:
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      %r = call i8* @id(i8* %x)
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
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  bool sawFormalParmBefore = false;
  bool sawActualRetBefore = false;
  for (const auto &pair : *graph) {
    sawFormalParmBefore = sawFormalParmBefore || isa<FormalParmSVFGNode>(pair.second);
    sawActualRetBefore = sawActualRetBefore || isa<ActualRetSVFGNode>(pair.second);
    ASSERT_FALSE(isa<InterPhiSVFGNode>(pair.second));
  }
  ASSERT_TRUE(sawFormalParmBefore);
  ASSERT_TRUE(sawActualRetBefore);

  SVFGOPT optimized;
  optimized.swapWith(*graph);

  SmallString<256> path;
  int fd = -1;
  ASSERT_FALSE(
      sys::fs::createTemporaryFile("svfgopt-full-write", "txt", fd, path));
  ::close(fd);

  optimized.buildAndWrite(path.str().str());

  bool optimizedHasInterPhi = false;
  for (const auto &pair : optimized)
    optimizedHasInterPhi = optimizedHasInterPhi || isa<InterPhiSVFGNode>(pair.second);
  EXPECT_TRUE(optimizedHasInterPhi);

  SVFG reloaded;
  reloaded.setICFG(&icfg);
  ASSERT_TRUE(reloaded.readFromFile(path.str().str()));
  sys::fs::remove(path);

  bool reloadedHasFormalParm = false;
  bool reloadedHasActualRet = false;
  bool reloadedHasInterPhi = false;
  for (const auto &pair : reloaded) {
    reloadedHasFormalParm = reloadedHasFormalParm || isa<FormalParmSVFGNode>(pair.second);
    reloadedHasActualRet = reloadedHasActualRet || isa<ActualRetSVFGNode>(pair.second);
    reloadedHasInterPhi = reloadedHasInterPhi || isa<InterPhiSVFGNode>(pair.second);
  }

  EXPECT_TRUE(reloadedHasFormalParm);
  EXPECT_TRUE(reloadedHasActualRet);
  EXPECT_FALSE(reloadedHasInterPhi);
}

TEST_F(SVFGSerializerTest, SVFGOPTKeepsSelfCyclesByDefault) {
  auto graph = std::make_unique<SVFG>();
  auto *phi = new IntraMSSAPhiSVFGNode(1, nullptr, 1, 1, SVFGNodeBS{1});
  graph->addNode(phi);
  ASSERT_NE(graph->addEdge(phi, phi, SVFGEdgeK::IntraIndirect, nullptr,
                           SVFGNodeBS{1}),
            nullptr);

  SVFGOPT optimized;
  ASSERT_TRUE(optimized.adoptAndOptimize(std::move(graph)));

  auto *optimizedPhi =
      dyn_cast<IntraMSSAPhiSVFGNode>(optimized.getNode(phi->getId()));
  ASSERT_NE(optimizedPhi, nullptr);

  bool foundSelfCycle = false;
  for (SVFGEdge *edge : optimizedPhi->getOutEdges()) {
    if (edge && edge->getSrcNode() == optimizedPhi &&
        edge->getDstNode() == optimizedPhi) {
      foundSelfCycle = true;
      break;
    }
  }
  EXPECT_TRUE(foundSelfCycle);
}

TEST_F(SVFGSerializerTest, PreservesExplicitNullInterPhiOperand) {
  const char *source = R"(
    define i32 @main() {
    entry:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  SVFG graph;
  graph.setICFG(&icfg);

  auto *phi = new InterPhiSVFGNode(
      graph.getNextNodeId(), icfg.getIntraBlockNode(&mainFn->getEntryBlock()),
      mainFn);
  phi->setOpVer(0, nullptr);
  graph.addNode(phi);

  SmallString<256> path;
  int fd = -1;
  ASSERT_FALSE(
      sys::fs::createTemporaryFile("svfg-null-interphi", "txt", fd, path));
  ::close(fd);

  ASSERT_TRUE(graph.writeToFile(path.str().str()));

  SVFG reloaded;
  reloaded.setICFG(&icfg);
  ASSERT_TRUE(reloaded.readFromFile(path.str().str()));
  sys::fs::remove(path);

  ASSERT_EQ(reloaded.getNumNodes(), 1u);
  auto *reloadedPhi = dyn_cast<InterPhiSVFGNode>(reloaded.begin()->second);
  ASSERT_NE(reloadedPhi, nullptr);
  EXPECT_TRUE(reloadedPhi->isFormalParmPHI());
  EXPECT_EQ(reloadedPhi->getFunction(), mainFn);
  EXPECT_EQ(reloadedPhi->getOpVerNum(), 1u);
  EXPECT_EQ(reloadedPhi->getOpVer(0), nullptr);
}

TEST_F(SVFGSerializerTest, CanonicalizesLegacyCallMuNodeOnRead) {
  const char *source = R"(
    declare void @callee(i8*)

    define void @main(i8* %p) {
    entry:
      call void @callee(i8* %p)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SmallString<256> path;
  int fd = -1;
  ASSERT_FALSE(
      sys::fs::createTemporaryFile("svfg-legacy-callmu", "txt", fd, path));
  ::close(fd);

  std::ofstream out(path.str().str());
  ASSERT_TRUE(out.is_open());
  out << "SVFG-TEXT-V6\n";
  out << "D 1 4 \"\" \"main\" 0 0 0\n";
  out << "N 1 " << static_cast<uint32_t>(SVFGK::CallMu) << " 7 3 0 0 1 42\n";
  out.close();

  SVFG reloaded;
  reloaded.setICFG(&icfg);
  ASSERT_TRUE(reloaded.readFromFile(path.str().str()));
  sys::fs::remove(path);

  SVFGNode *node = reloaded.getNode(1);
  ASSERT_NE(node, nullptr);
  EXPECT_TRUE(isa<ActualInSVFGNode>(node));
  EXPECT_NE(node->getNodeKind(), SVFGK::CallMu);
  auto *actualIn = dyn_cast<ActualInSVFGNode>(node);
  ASSERT_NE(actualIn, nullptr);
  EXPECT_EQ(actualIn->getMemReg(), 7u);
  EXPECT_EQ(actualIn->getSSAVersion(), 3u);
}

TEST_F(SVFGSerializerTest, CanonicalizesLegacyEdgeKindsOnRead) {
  const char *source = R"(
    define i8* @touch(i8* %p) {
    entry:
      %v = load i8, i8* %p
      store i8 %v, i8* %p
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      %r = call i8* @touch(i8* %x)
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
  std::unique_ptr<SVFG> original(builder.build(&icfg));
  ASSERT_NE(original, nullptr);

  SmallString<256> path;
  int fd = -1;
  ASSERT_FALSE(
      sys::fs::createTemporaryFile("svfg-legacy-edges", "txt", fd, path));
  ::close(fd);

  ASSERT_TRUE(original->writeToFile(path.str().str()));

  std::ifstream in(path.str().str());
  ASSERT_TRUE(in.is_open());
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line[0] == 'E') {
      std::istringstream iss(line);
      char tag = 0;
      uint32_t src = 0;
      uint32_t dst = 0;
      uint32_t kind = 0;
      iss >> tag >> src >> dst >> kind;
      std::string rest;
      std::getline(iss, rest);

      if (kind == static_cast<uint32_t>(SVFGEdgeK::CallDir))
        kind = static_cast<uint32_t>(SVFGEdgeK::ParamCall);
      else if (kind == static_cast<uint32_t>(SVFGEdgeK::RetDir))
        kind = static_cast<uint32_t>(SVFGEdgeK::ParamRet);
      else if (kind == static_cast<uint32_t>(SVFGEdgeK::CallAIn))
        kind = static_cast<uint32_t>(SVFGEdgeK::CallFIn);
      else if (kind == static_cast<uint32_t>(SVFGEdgeK::RetAOut))
        kind = static_cast<uint32_t>(SVFGEdgeK::RetFOut);

      std::ostringstream oss;
      oss << tag << " " << src << " " << dst << " " << kind << rest;
      line = oss.str();
    }
    lines.push_back(line);
  }
  in.close();

  std::ofstream out(path.str().str(), std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  for (const std::string &updated : lines)
    out << updated << "\n";
  out.close();

  SVFG reloaded;
  reloaded.setICFG(&icfg);
  ASSERT_TRUE(reloaded.readFromFile(path.str().str()));
  sys::fs::remove(path);

  bool sawCallDir = false;
  bool sawRetDir = false;
  bool sawCallAIn = false;
  bool sawRetAOut = false;

  for (const auto &pair : reloaded) {
    for (SVFGEdge *edge : pair.second->getOutEdges()) {
      ASSERT_NE(edge, nullptr);
      EXPECT_NE(edge->getEdgeKind(), SVFGEdgeK::ParamCall);
      EXPECT_NE(edge->getEdgeKind(), SVFGEdgeK::ParamRet);
      EXPECT_NE(edge->getEdgeKind(), SVFGEdgeK::CallFIn);
      EXPECT_NE(edge->getEdgeKind(), SVFGEdgeK::RetFOut);
      sawCallDir = sawCallDir || edge->getEdgeKind() == SVFGEdgeK::CallDir;
      sawRetDir = sawRetDir || edge->getEdgeKind() == SVFGEdgeK::RetDir;
      sawCallAIn = sawCallAIn || edge->getEdgeKind() == SVFGEdgeK::CallAIn;
      sawRetAOut = sawRetAOut || edge->getEdgeKind() == SVFGEdgeK::RetAOut;
    }
  }

  EXPECT_TRUE(sawCallDir);
  EXPECT_TRUE(sawRetDir);
  EXPECT_TRUE(sawCallAIn);
  EXPECT_TRUE(sawRetAOut);
}

TEST_F(SVFGSerializerTest, RoundTripsDeferredIndirectCallState) {
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
  std::unique_ptr<SVFG> original(builder.build(&icfg));
  ASSERT_NE(original, nullptr);

  SmallString<256> path;
  int fd = -1;
  ASSERT_FALSE(
      sys::fs::createTemporaryFile("svfg-indcall-state", "txt", fd, path));
  ::close(fd);

  ASSERT_TRUE(original->writeToFile(path.str().str()));

  SVFG reloaded;
  reloaded.setICFG(&icfg);
  ASSERT_TRUE(reloaded.readFromFile(path.str().str()));
  sys::fs::remove(path);

  Function *applyFn = module->getFunction("apply");
  Function *targetFn = module->getFunction("target");
  ASSERT_NE(applyFn, nullptr);
  ASSERT_NE(targetFn, nullptr);

  const CallBase *indCall = nullptr;
  for (const BasicBlock &bb : *applyFn) {
    for (const Instruction &inst : bb) {
      const auto *cb = dyn_cast<CallBase>(&inst);
      if (cb && !cb->getCalledFunction()) {
        indCall = cb;
        break;
      }
    }
    if (indCall)
      break;
  }
  ASSERT_NE(indCall, nullptr);

  const Argument *fpArg = &*applyFn->arg_begin();
  SVFGNode *fpNode = reloaded.getValueNode(fpArg);
  ASSERT_NE(fpNode, nullptr);
  const uint32_t funPtrKey = fpNode->hasValueId() ? fpNode->getValueId()
                                                  : fpNode->getId();
  EXPECT_EQ(reloaded.getIndCallSites(funPtrKey).count(indCall), 1u);

  std::vector<SVFGEdge *> newEdges;
  EXPECT_TRUE(builder.connectCallSiteToCalleeOnTheFly(&reloaded, indCall,
                                                      targetFn, newEdges));
  EXPECT_FALSE(newEdges.empty());
  EXPECT_NE(reloaded.getCallSiteId(indCall, targetFn), 0u);

  ICFGNode *callerNode = icfg.getIntraBlockNode(indCall->getParent());
  ICFGNode *calleeEntryNode = icfg.getFunEntryICFGNode(targetFn);
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(calleeEntryNode, nullptr);

  bool foundCallEdge = false;
  for (const auto *edge : callerNode->getOutEdges()) {
    const auto *callEdge = dyn_cast<CallCFGEdge>(edge);
    if (!callEdge)
      continue;
    if (callEdge->getDstNode() == calleeEntryNode &&
        callEdge->getCallSite() == indCall) {
      foundCallEdge = true;
      break;
    }
  }
  EXPECT_TRUE(foundCallEdge);
}

} // namespace
