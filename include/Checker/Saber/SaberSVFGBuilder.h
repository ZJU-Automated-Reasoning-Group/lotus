//===- SaberSVFGBuilder.h -- Building SVFG for Saber--------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
//
//===----------------------------------------------------------------------===//

#ifndef SABERSVFGBUILDER_H_
#define SABERSVFGBUILDER_H_

#include "IR/ICFG/CallGraph.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGNode.h"
#include "IR/SVFG/SVFGOPT.h"

#include <deque>
#include <map>
#include <set>
#include <unordered_set>

namespace lotus {
namespace analysis {

class SaberCondAllocator;

class SaberSVFGBuilder : public SVFGBuilder {

public:
  using SVFGNodeSet = std::set<const SVFGNode *>;
  using NodeToPTSSMap = std::map<uint32_t, std::set<uint32_t>>;
  using WorkList = std::deque<uint32_t>;

  SaberSVFGBuilder() : SVFGBuilder() {}

  virtual ~SaberSVFGBuilder() {}

  std::unique_ptr<SVFG> buildForSaber(const ICFG *icfg, bool fullSVFG);
  SVFG *buildSVFG(const ICFG *icfg);

  inline bool isGlobalSVFGNode(const SVFGNode *node) const {
    return globSVFGNodes.find(node) != globSVFGNodes.end();
  }

  void setSaberCondAllocator(SaberCondAllocator *allocator) {
    saberCondAllocator = allocator;
  }

  void setModule(llvm::Module *M) {
    module_ = M;
    recursiveFunctionsReady_ = false;
    recursiveFunctionsCache_.clear();
  }

  void setCurrentSVFG(SVFG *g) { currentSVFG_ = g; }

  void collectGlobals();
  void recomputeGlobalSVFGNodes();
  void reset();

  /// Called after building SVFG (e.g. from SrcSnkDDA::initialize).
  void rmDerefDirSVFGEdges();
  virtual void rmIncomingEdgeForSUStore();
  virtual void AddExtActualParmSVFGNodes();

protected:
  std::unique_ptr<SVFG>
  buildCompatSVFGForSaber(std::unique_ptr<SVFG> graph);

  /// Get points-to set of an object (objects that this object, as a pointer,
  /// points to). Uses AserPTA via getObjectValue + getObjectIdsForValue.
  /// Returns empty if objId has no value.
  SVFGNodeBS getPointsToOfObject(uint32_t objId);
  bool isStrongUpdate(const SVFGNode *node, uint32_t &singleton);
  bool accessGlobal(const SVFGNode *pagNode);
  bool accessGlobal(const llvm::Value *ptr);
  std::set<uint32_t> &CollectPtsChain(uint32_t id, NodeToPTSSMap &cachedPtsMap);

  std::set<uint32_t> globs;
  SVFGNodeSet globSVFGNodes;
  SVFG *currentSVFG_ = nullptr;
  std::unordered_set<const llvm::Function *> recursiveFunctionsCache_;
  bool recursiveFunctionsReady_ = false;

  SaberCondAllocator *saberCondAllocator = nullptr;
  llvm::Module *module_ = nullptr;
};

} // namespace analysis
} // namespace lotus

#endif
