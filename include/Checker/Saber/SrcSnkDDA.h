//===- SrcSnkDDA.h -- Source-sink analyzer-----------------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
//
//===----------------------------------------------------------------------===//

#ifndef SRCSNKANALYSIS_H_
#define SRCSNKANALYSIS_H_

#include "Alias/DDA/CxtDPItem.h"
#include "Checker/Saber/ProgSlice.h"
#include "Checker/Saber/SaberSVFGBuilder.h"
#include "Checker/Saber/SrcSnkSolver.h"
#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGStats.h"

#include <map>
#include <memory>
#include <set>
#include <string>

namespace lotus {
namespace analysis {

class SrcSnkDDA : public SrcSnkSolver<CxtLocDPItem> {

public:
  using SVFGNodeSet = ProgSlice::SVFGNodeSet;
  using SVFGNodeToSliceMap = std::map<const SVFGNode *, ProgSlice *>;
  using SVFGNodeSetIter = SVFGNodeSet::const_iterator;
  using DPIm = CxtLocDPItem;
  using DPImSet = std::set<DPIm>;
  using SVFGNodeToDPItemsMap = std::map<const SVFGNode *, DPImSet>;
  using SrcToCSMap = std::map<const SVFGNode *, const llvm::CallBase *>;
  using CallSiteSet = std::set<const llvm::CallBase *>;
  using SVFGNodeBS = std::set<uint32_t>;
  using WorkList = ProgSlice::VFWorkList;
  using CSWorkList = std::deque<const llvm::CallBase *>;
  using RemovedSUVFEdges = SaberCondAllocator::SVFGNodeToSVFGNodeSetMap;

private:
  ProgSlice *_curSlice = nullptr;
  SVFGNodeSet sources;
  SVFGNodeSet sinks;
  std::unique_ptr<SaberCondAllocator> saberCondAllocator;
  SVFGNodeToDPItemsMap nodeToDPItemsMap;
  SVFGNodeSet visitedSet;
  SrcToCSMap srcToCSMap;
  bool hasPrecomputedSrcSnk_ = false;

protected:
  SaberSVFGBuilder memSSA;
  SVFG *svfg = nullptr;
  std::unique_ptr<SVFG> svfg_;
  std::unique_ptr<::ICFG> icfg_;
  std::unique_ptr<::ICFGBuilder> icfgBuilder_;
  llvm::Module *module_ = nullptr;
  std::unique_ptr<SVFGStats> sliceStats_;

public:
  SrcSnkDDA() { saberCondAllocator = std::make_unique<SaberCondAllocator>(); }
  ~SrcSnkDDA() override;

  void analyze();
  void initialize();
  void finalize() { dumpSlices(); }

  const SVFG *getSVFG() const { return graph(); }

  bool isGlobalSVFGNode(const SVFGNode *node) const {
    return memSSA.isGlobalSVFGNode(node);
  }

  void setCurSlice(const SVFGNode *src);

  ProgSlice *getCurSlice() const { return _curSlice; }
  void addSinkToCurSlice(const SVFGNode *node) {
    _curSlice->addToSinks(node);
    addToCurForwardSlice(node);
  }
  bool isInCurForwardSlice(const SVFGNode *node) {
    return _curSlice->inForwardSlice(node);
  }
  bool isInCurBackwardSlice(const SVFGNode *node) {
    return _curSlice->inBackwardSlice(node);
  }
  void addToCurForwardSlice(const SVFGNode *node) {
    _curSlice->addToForwardSlice(node);
  }
  void addToCurBackwardSlice(const SVFGNode *node) {
    _curSlice->addToBackwardSlice(node);
  }

  virtual void initSrcs() = 0;
  virtual void initSnks() = 0;
  virtual bool isSourceLikeFun(const std::string &funName) = 0;
  virtual bool isSinkLikeFun(const std::string &funName) = 0;

  bool isSource(const SVFGNode *node) const {
    return getSources().find(node) != getSources().end();
  }
  bool isSink(const SVFGNode *node) const {
    return getSinks().find(node) != getSinks().end();
  }

  bool isInAWrapper(const SVFGNode *src, CallSiteSet &csIdSet);

  virtual void reportBug(ProgSlice *slice) = 0;

  const SVFGNodeSet &getSources() const { return sources; }
  SVFGNodeSetIter sourcesBegin() const { return sources.begin(); }
  SVFGNodeSetIter sourcesEnd() const { return sources.end(); }
  void addToSources(const SVFGNode *node) { sources.insert(node); }
  void addSrcToCSID(const SVFGNode *node, const llvm::CallBase *cs) {
    srcToCSMap[node] = cs;
  }
  const llvm::CallBase *getSrcCSID(const SVFGNode *node) const {
    auto it = srcToCSMap.find(node);
    return (it != srcToCSMap.end()) ? it->second : nullptr;
  }

  void importSourceSinkState(const SVFGNodeSet &precomputedSources,
                             const SVFGNodeSet &precomputedSinks,
                             const SrcToCSMap &precomputedSrcToCS) {
    sources = precomputedSources;
    sinks = precomputedSinks;
    srcToCSMap = precomputedSrcToCS;
    hasPrecomputedSrcSnk_ = true;
  }

  void exportSourceSinkState(SVFGNodeSet &outSources, SVFGNodeSet &outSinks,
                             SrcToCSMap &outSrcToCS) const {
    outSources = sources;
    outSinks = sinks;
    outSrcToCS = srcToCSMap;
  }

  const SVFGNodeSet &getSinks() const { return sinks; }
  SVFGNodeSetIter sinksBegin() const { return sinks.begin(); }
  SVFGNodeSetIter sinksEnd() const { return sinks.end(); }
  void addToSinks(const SVFGNode *node) { sinks.insert(node); }

  void importRemovedSUVFEdges(const RemovedSUVFEdges &removedEdges) {
    getSaberCondAllocator()->getRemovedSUVFEdges() = removedEdges;
  }

  void exportRemovedSUVFEdges(RemovedSUVFEdges &outRemovedEdges) const {
    outRemovedEdges = getSaberCondAllocator()->getRemovedSUVFEdges();
  }

  SaberCondAllocator *getSaberCondAllocator() const {
    return saberCondAllocator.get();
  }

  void setModule(llvm::Module *M);

  /// Set shared SVFG and ICFG (for optimization when running multiple checkers)
  /// Ownership is transferred - the checker will take ownership of these
  /// pointers
  void setSharedSVFGAndICFG(std::unique_ptr<SVFG> shared_svfg,
                            std::unique_ptr<::ICFG> shared_icfg) {
    svfg_ = std::move(shared_svfg);
    icfg_ = std::move(shared_icfg);
    svfg = svfg_.get();
    setGraph(svfg);
  }

  /// Extract SVFG and ICFG (for sharing with other checkers)
  /// Ownership is transferred to the caller
  std::pair<std::unique_ptr<SVFG>, std::unique_ptr<::ICFG>>
  extractSVFGAndICFG() {
    svfg = nullptr;
    setGraph(nullptr);
    return std::make_pair(std::move(svfg_), std::move(icfg_));
  }

  /// Check if SVFG/ICFG are already initialized (to skip rebuilding)
  bool hasSVFGAndICFG() const { return svfg_ != nullptr && icfg_ != nullptr; }

protected:
  void resetAnalysisState(bool preserveSharedGraph, bool preservePrecomputedSrcSnk);

  void FWProcessCurNode(const DPIm &item) override {
    const SVFGNode *node = getNode(item.getCurNodeID());
    if (isSink(node)) {
      addSinkToCurSlice(node);
      _curSlice->setPartialReachable();
    } else {
      addToCurForwardSlice(node);
    }
  }

  void BWProcessCurNode(const DPIm &item) override {
    const SVFGNode *node = getNode(item.getCurNodeID());
    if (isInCurForwardSlice(node)) {
      addToCurBackwardSlice(node);
    }
  }

  void FWProcessOutgoingEdge(const DPIm &item, SVFGEdge *edge) override;
  void BWProcessIncomingEdge(const DPIm &item, SVFGEdge *edge) override;

  void forwardTraverse(DPIm &it) override;

  bool forwardVisited(const SVFGNode *node, const DPIm &item) {
    auto it = nodeToDPItemsMap.find(node);
    if (it != nodeToDPItemsMap.end())
      return it->second.find(item) != it->second.end();
    return false;
  }

  void addForwardVisited(const SVFGNode *node, const DPIm &item) {
    nodeToDPItemsMap[node].insert(item);
  }

  bool backwardVisited(const SVFGNode *node) {
    return visitedSet.find(node) != visitedSet.end();
  }

  void addBackwardVisited(const SVFGNode *node) { visitedSet.insert(node); }

  void clearVisitedMap() {
    nodeToDPItemsMap.clear();
    visitedSet.clear();
  }

  bool isAllPathReachable() { return _curSlice->isAllReachable(); }
  bool isSomePathReachable() { return _curSlice->isPartialReachable(); }

  void dumpSlices();
  void annotateSlice(ProgSlice *slice);
  void printZ3Stat();
};

} // namespace analysis
} // namespace lotus

#endif
