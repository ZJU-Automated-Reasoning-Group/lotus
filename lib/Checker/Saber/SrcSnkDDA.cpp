//===- SrcSnkDDA.cpp -- Source-sink analyzer --------------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
// Context-sensitive forward/backward traversal and isInAWrapper aligned with
// SVF.
//
//===----------------------------------------------------------------------===//

#include "Checker/Saber/SrcSnkDDA.h"

#include "Checker/Saber/ProgSlice.h"
#include "Checker/Saber/SaberOptions.h"
#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"
#include "IR/SVFG/SVFGStats.h"
#include "Utils/LLVM/RecursiveTimer.h"

#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::analysis;

static std::string getSourceName(const SVFGNode *node) {
  if (!node)
    return "<unknown>";
  if (const llvm::Instruction *inst = node->getInstruction()) {
    if (const llvm::Function *F = inst->getFunction())
      return F->getName().str();
    return "<no-function>";
  }
  return "<no-instruction>";
}

SrcSnkDDA::~SrcSnkDDA() {
  delete _curSlice;
  _curSlice = nullptr;
}

void SrcSnkDDA::setModule(llvm::Module *M) {
  if (module_ && module_ != M) {
    // Shared SVFG/ICFG and imported source/sink state are module-specific.
    // Reusing them across modules would make initialize() skip a required
    // rebuild and silently analyze the wrong IR.
    resetAnalysisState(false, false);
  }
  module_ = M;
}

void SrcSnkDDA::resetAnalysisState(bool preserveSharedGraph,
                                   bool preservePrecomputedSrcSnk) {
  delete _curSlice;
  _curSlice = nullptr;
  clearVisitedMap();
  clearWorklist();
  sliceStats_.reset();

  if (!preservePrecomputedSrcSnk) {
    sources.clear();
    sinks.clear();
    srcToCSMap.clear();
    hasPrecomputedSrcSnk_ = false;
  }

  if (!preserveSharedGraph) {
    svfg_.reset();
    icfg_.reset();
    icfgBuilder_.reset();
    svfg = nullptr;
    setGraph(nullptr);
  }

  if (saberCondAllocator)
    saberCondAllocator->reset(preserveSharedGraph);
  memSSA.reset();
}

void SrcSnkDDA::initialize() {
  if (!module_)
    return;

  resetAnalysisState(hasSVFGAndICFG(), hasPrecomputedSrcSnk_);

  RecursiveTimer timer("Saber initialization");
  const bool needsBuild = !hasSVFGAndICFG();

  // Build ICFG and SVFG only if not already set (for shared usage)
  if (needsBuild) {
    {
      RecursiveTimer timer("ICFG build");
      icfg_ = std::make_unique<::ICFG>();
      icfgBuilder_ = std::make_unique<::ICFGBuilder>(icfg_.get());
      icfgBuilder_->build(module_);
    }

    {
      RecursiveTimer timer("SVFG build");
      memSSA.setModule(module_);
      memSSA.setSaberCondAllocator(getSaberCondAllocator());
      svfg_ = memSSA.buildForSaber(icfg_.get(), SaberOptions::fullSVFG());
      if (!svfg_) {
        icfg_.reset();
        icfgBuilder_.reset();
        return;
      }
      this->svfg = svfg_.get();
      setGraph(this->svfg);
      memSSA.setCurrentSVFG(this->svfg);
      memSSA.collectGlobals();
      memSSA.rmDerefDirSVFGEdges();
      memSSA.rmIncomingEdgeForSUStore();
      memSSA.AddExtActualParmSVFGNodes();
      memSSA.recomputeGlobalSVFGNodes();
      if (SaberOptions::verbose()) {
        outs() << "SVFG nodes: " << this->svfg->getNumNodes() << "\n";
      }
    }
  } else {
    // SVFG/ICFG already set, just ensure svfg pointer is set
    this->svfg = svfg_.get();
    setGraph(this->svfg);
    memSSA.setModule(module_);
    memSSA.setSaberCondAllocator(getSaberCondAllocator());
    memSSA.setCurrentSVFG(this->svfg);
    memSSA.collectGlobals();
    memSSA.recomputeGlobalSVFGNodes();
    if (SaberOptions::verbose()) {
      outs() << "Using shared SVFG (nodes: " << this->svfg->getNumNodes()
             << ")\n";
    }
  }

  if (SaberOptions::dumpSlice())
    sliceStats_ = std::make_unique<SVFGStats>(svfg);

  {
    RecursiveTimer timer("Dominator/Loop analysis");
    getSaberCondAllocator()->setModule(module_);
    for (auto &func : *module_) {
      if (!func.isDeclaration()) {
        getSaberCondAllocator()->initDominatorsForFunction(&func);
        getSaberCondAllocator()->initPostDominatorsForFunction(&func);
        getSaberCondAllocator()->initLoopInfoForFunction(&func);
      }
    }
    getSaberCondAllocator()->allocate();
  }

  if (!hasPrecomputedSrcSnk_) {
    {
      RecursiveTimer timer("Source initialization");
      initSrcs();
      if (SaberOptions::verbose()) {
        outs() << "Found " << sources.size() << " source(s)\n";
      }
    }

    {
      RecursiveTimer timer("Sink initialization");
      initSnks();
      if (SaberOptions::verbose()) {
        outs() << "Found " << sinks.size() << " sink(s)\n";
      }
    }
  } else if (SaberOptions::verbose()) {
    outs() << "Using precomputed sources/sinks (" << sources.size()
           << " source(s), " << sinks.size() << " sink(s))\n";
  }
}

void SrcSnkDDA::analyze() {
  RecursiveTimer timer("Saber analysis");

  initialize();

  if (sources.empty()) {
    if (SaberOptions::verbose()) {
      outs() << "No sources found, skipping analysis\n";
    }
    finalize();
    return;
  }

  ContextCond::setMaxCxtLen(SaberOptions::cxtLimit());

  if (SaberOptions::verbose()) {
    outs() << "Analyzing " << sources.size() << " source(s) with context limit "
           << SaberOptions::cxtLimit() << "\n";
  }

  unsigned sourceIdx = 0;
  for (auto *srcNode : sources) {
    sourceIdx++;
    std::string srcName = getSourceName(srcNode);
    std::string timerName = "Source " + std::to_string(sourceIdx) + "/" +
                            std::to_string(sources.size()) + ": " + srcName;
    RecursiveTimer sourceTimer(timerName);

    setCurSlice(srcNode);

    {
      RecursiveTimer timer("Forward traverse");
      ContextCond cxt;
      CxtVar var(cxt, srcNode->getId());
      DPIm item(var, srcNode);
      forwardTraverse(item);
      if (SaberOptions::verbose()) {
        outs() << "Forward slice size: " << getCurSlice()->getForwardSliceSize()
               << "\n";
      }
    }

    if (getCurSlice()->isReachGlobal()) {
      if (SaberOptions::verbose()) {
        outs() << "Source reaches global, skipping path analysis\n";
      }
    } else {
      unsigned sinkCount = 0;
      for (auto it = getCurSlice()->sinksBegin(),
                et = getCurSlice()->sinksEnd();
           it != et; ++it) {
        sinkCount++;
        auto *snkNode = *it;
        ContextCond cxt;
        CxtVar var(cxt, snkNode->getId());
        DPIm item(var, snkNode);
        backwardTraverse(item);
      }

      if (SaberOptions::verbose()) {
        outs() << "Backward slice size: "
               << getCurSlice()->getBackwardSliceSize() << " (" << sinkCount
               << " sinks)\n";
      }

      if (SaberOptions::dumpSlice())
        annotateSlice(getCurSlice());

      {
        RecursiveTimer timer("Path condition solve");
        if (getCurSlice()->AllPathReachableSolve())
          getCurSlice()->setAllReachable();
      }
    }

    reportBug(getCurSlice());
  }

  finalize();
}

bool SrcSnkDDA::isInAWrapper(const SVFGNode *src, CallSiteSet &csIdSet) {
  bool reachFunExit = false;
  WorkList worklist;
  worklist.push_back(src);
  SVFGNodeBS visited;
  unsigned step = 0;

  while (!worklist.empty()) {
    const SVFGNode *node = worklist.front();
    worklist.pop_front();

    if (visited.count(node->getId()))
      continue;
    visited.insert(node->getId());

    if (step++ > SaberOptions::maxStepInWrapper())
      return false;

    for (SVFGEdge *e : node->getOutEdges()) {
      SVFGEdgeK k = e->getEdgeKind();
      const SVFGNode *succ = e->getDstNode();

      if (k == SVFGEdgeK::CallDir || k == SVFGEdgeK::CallInd)
        return false;
      if (k == SVFGEdgeK::RetDir || k == SVFGEdgeK::RetInd) {
        reachFunExit = true;
        if (const llvm::CallBase *cs = e->getCallSite())
          csIdSet.insert(cs);
      } else if (isIntraVFGEdge(k) && isDirectVFGEdge(k)) {
        SVFGK sk = succ->getNodeKind();
        if (sk == SVFGK::Copy || sk == SVFGK::Gep || sk == SVFGK::IntraPhi ||
            sk == SVFGK::FormalRet || sk == SVFGK::ActualRet ||
            sk == SVFGK::Store || sk == SVFGK::InterPhi)
          worklist.push_back(succ);
      } else if (k == SVFGEdgeK::IntraLoad || k == SVFGEdgeK::IntraMu ||
                 k == SVFGEdgeK::IntraIndirect ||
                 (k == SVFGEdgeK::IntraDirect &&
                  succ->getNodeKind() == SVFGK::Load)) {
        SVFGK sk = succ->getNodeKind();
        if (sk == SVFGK::Load || sk == SVFGK::MIntraPhi ||
            sk == SVFGK::MInterPhi)
          worklist.push_back(succ);
      } else {
        // Match SVF: unsupported interprocedural/variant edges mean this is
        // not a pure wrapper propagation path.
        return false;
      }
    }
  }

  return reachFunExit;
}

void SrcSnkDDA::setCurSlice(const SVFGNode *src) {
  if (_curSlice != nullptr) {
    delete _curSlice;
    _curSlice = nullptr;
    clearVisitedMap();
  }
  _curSlice = new ProgSlice(src, getSaberCondAllocator(), getSVFG());
}

void SrcSnkDDA::forwardTraverse(DPIm &it) {
  // Seed visited with the initial state so cycles don't re-enqueue the source.
  const SVFGNode *srcNode = getNode(getNodeIDFromItem(it));
  if (srcNode && !forwardVisited(srcNode, it))
    addForwardVisited(srcNode, it);

  // Override to add progress reporting and traversal cap.
  pushIntoWorklist(it);
  unsigned long long processed = 0;
  unsigned long long lastReport = 0;
  bool hitForwardItemLimit = false;
  const unsigned maxForwardItems = SaberOptions::maxForwardItems();
  const unsigned long long verboseReportInterval =
      10000; // Report every 10k nodes in verbose mode
  const unsigned long long normalReportInterval =
      100000; // Report every 100k nodes normally

  while (!isWorklistEmpty()) {
    if (maxForwardItems != 0 && processed >= maxForwardItems) {
      hitForwardItemLimit = true;
      break;
    }

    DPIm item = popFromWorklist();
    FWProcessCurNode(item);
    SVFGNode *v = getNode(getNodeIDFromItem(item));
    if (!v)
      continue;

    processed++;

    // Periodic progress reporting
    unsigned long long reportInterval =
        SaberOptions::verbose() ? verboseReportInterval : normalReportInterval;
    if (SaberOptions::verbose() && processed - lastReport >= reportInterval) {
      outs() << "  Forward traverse progress: processed " << processed
             << " items, forward slice size: "
             << getCurSlice()->getForwardSliceSize();
      if (SaberOptions::verbose()) {
        outs() << ", unique nodes with contexts: " << nodeToDPItemsMap.size();
      }
      outs() << "\n";
      lastReport = processed;
    }

    for (auto &edge : v->getOutEdges()) {
      FWProcessOutgoingEdge(item, edge);
    }
  }

  // Keep the shared solver queue clean for subsequent traversals.
  if (hitForwardItemLimit) {
    while (!isWorklistEmpty())
      (void)popFromWorklist();
    if (SaberOptions::verbose()) {
      outs() << "  Forward traverse stopped after " << processed
             << " items (hit --saber-max-forward-items=" << maxForwardItems
             << ")\n";
    }
  }

  if (SaberOptions::verbose() && processed > 0) {
    outs() << "  Forward traverse completed: processed " << processed
           << " items total, forward slice size: "
           << getCurSlice()->getForwardSliceSize();
    outs() << ", unique nodes with contexts: " << nodeToDPItemsMap.size();
    outs() << "\n";
  }
}

void SrcSnkDDA::FWProcessOutgoingEdge(const DPIm &item, SVFGEdge *edge) {
  const SVFGNode *dstNode = edge->getDstNode();
  ContextCond cxt = item.getCond();
  CxtVar var(cxt, dstNode->getId());
  DPIm newItem(var, dstNode);

  if (isGlobalSVFGNode(dstNode) || getCurSlice()->isReachGlobal()) {
    getCurSlice()->setReachGlobal();
    return;
  }

  const SVFG *g = graph();
  if (edge->isCallEdge()) {
    const llvm::CallBase *cs = edge->getCallSite();
    const llvm::Function *callee = dstNode ? dstNode->getFunction() : nullptr;
    if (cs && g) {
      uint32_t csId = g->getCallSiteId(cs, callee);
      if (csId != 0)
        (void)newItem.pushContext(csId);
    }
  } else if (edge->isRetEdge()) {
    const llvm::CallBase *cs = edge->getCallSite();
    const llvm::Function *callee =
        edge->getSrcNode() ? edge->getSrcNode()->getFunction() : nullptr;
    if (!cs || !g)
      return;
    uint32_t csId = g->getCallSiteId(cs, callee);
    if (csId == 0)
      return;
    if (!newItem.matchContext(csId))
      return;
  }

  if (forwardVisited(dstNode, newItem))
    return;
  addForwardVisited(dstNode, newItem);
  pushIntoWorklist(newItem);
}

void SrcSnkDDA::BWProcessIncomingEdge(const DPIm &item, SVFGEdge *edge) {
  (void)item;
  const SVFGNode *srcNode = edge->getSrcNode();
  if (backwardVisited(srcNode))
    return;
  addBackwardVisited(srcNode);

  ContextCond cxt;
  CxtVar var(cxt, srcNode->getId());
  DPIm newItem(var, srcNode);
  pushIntoWorklist(newItem);
}

void SrcSnkDDA::annotateSlice(ProgSlice *slice) {
  if (!slice || !SaberOptions::dumpSlice() || !sliceStats_)
    return;
  sliceStats_->addSource(slice->getSource());
  for (auto it = slice->sinksBegin(), et = slice->sinksEnd(); it != et; ++it)
    sliceStats_->addSink(*it);
  for (auto it = slice->forwardSliceBegin(), et = slice->forwardSliceEnd();
       it != et; ++it)
    sliceStats_->addToForwardSlice(*it);
  for (auto it = slice->backwardSliceBegin(), et = slice->backwardSliceEnd();
       it != et; ++it)
    sliceStats_->addToBackwardSlice(*it);
}

void SrcSnkDDA::dumpSlices() {
  if (!SaberOptions::dumpSlice() || !svfg)
    return;
  svfg->dump("saber_slice.dot");
}

void SrcSnkDDA::printZ3Stat() {
  outs() << "Z3 Mem usage: " << getSaberCondAllocator()->getMemUsage() << "\n";
  outs() << "Z3 Number: " << getSaberCondAllocator()->getCondNum() << "\n";
}
