#pragma once

#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"

#include <set>
#include <unordered_map>
#include <vector>

namespace lotus {
namespace analysis {

class SVFGOPT : public SVFG {
public:
  using SVFGNodeSet = std::set<SVFGNode *>;
  using NodeIDMap = std::unordered_map<uint32_t, uint32_t>;
  using FIFOWorkList = std::vector<const MSSAPhiSVFGNode *>;

  struct SelfCycleInfo {
    const MSSAPhiSVFGNode *phiNode;
    SVFGEdge *callEdge;
    SVFGEdge *retEdge;

    SelfCycleInfo(const MSSAPhiSVFGNode *p, SVFGEdge *c, SVFGEdge *r)
        : phiNode(p), callEdge(c), retEdge(r) {}
  };

  SVFGOPT()
      : SVFG(), keepActualOutFormalIn(false), keepAllSelfCycle(false),
        keepContextSelfCycle(false) {}

  ~SVFGOPT() override = default;

  inline void setKeepActualOutFormalIn(bool keep = true) {
    keepActualOutFormalIn = keep;
  }

  inline void setKeepAllSelfCycle(bool keep = true) { keepAllSelfCycle = keep; }

  inline void setKeepContextSelfCycle(bool keep = true) {
    keepContextSelfCycle = keep;
  }

  SVFG *buildAndOptimize(const ICFG *icfg, const SVFGBuilderConfig &config);
  bool adoptAndOptimize(std::unique_ptr<SVFG> graph);
  void readAndOptimize(const std::string &filename);
  void buildAndWrite(const std::string &filename);

protected:
  void optimize();

  void connectAParamAndFParam(const llvm::CallBase *csArg,
                              const llvm::Argument *funArg,
                              const llvm::CallBase *callSite, uint32_t csId);

  void connectFRetAndARet(const llvm::Value *funRet,
                          const llvm::CallBase *csRet, uint32_t csId);

  void connectAInAndFIn(const ActualInSVFGNode *actualIn,
                        const FormalInSVFGNode *formalIn, uint32_t csId);

  void connectFOutAndAOut(const FormalOutSVFGNode *formalOut,
                          const ActualOutSVFGNode *actualOut, uint32_t csId);

  void handleInterValueFlow();
  void replaceFParamWithPHI(PhiSVFGNode *phi, SVFGNode *svfgNode);
  void replaceARetWithPHI(PhiSVFGNode *phi, SVFGNode *svfgNode);
  void retargetEdgesOfAInFOut(SVFGNode *node);
  void retargetEdgesOfAOutFIn(SVFGNode *node);

  void handleIntraValueFlow();
  void bypassMSSAPHINode(const MSSAPhiSVFGNode *node);
  bool handleSelfCycleEdges(const MSSAPhiSVFGNode *node);

  void initialWorkList();
  bool addToWorkList(const SVFGNode *node);
  bool canRemoveNode(const SVFGNode *node);
  void removeAllEdges(const SVFGNode *node);
  void removeIncomingEdges(const SVFGNode *node);
  void removeOutgoingEdges(const SVFGNode *node);
  bool addNewEdge(uint32_t srcId, uint32_t dstId, const SVFGEdge *predEdge,
                  const SVFGEdge *succEdge);

  bool bothInterEdges(const SVFGEdge *edge1, const SVFGEdge *edge2) const;

  void addPHIOperand(PhiSVFGNode *phi, uint32_t operandPos,
                     const llvm::Value *operand);

  InterPhiSVFGNode *
  addInterPHIForFormalParm(const FormalParmSVFGNode *formalParm);
  InterPhiSVFGNode *addInterPHIForActualRet(const ActualRetSVFGNode *actualRet);

  void resetDef(const llvm::Value *value, SVFGNode *node);
  void setActualInDef(uint32_t aiId, uint32_t defId);
  void setFormalOutDef(uint32_t foId, uint32_t defId);
  bool isDefOfAInFOut(const SVFGNode *node) const;

  bool actualInOfIndCS(const ActualInSVFGNode *ai) const;
  bool actualOutOfIndCS(const ActualOutSVFGNode *ao) const;
  bool formalInOfAddressTakenFunc(const FormalInSVFGNode *fi) const;
  bool formalOutOfAddressTakenFunc(const FormalOutSVFGNode *fo) const;
  bool isConnectingTwoCallSites(const SVFGNode *node) const;

private:
  NodeIDMap actualInToDefMap;
  NodeIDMap formalOutToDefMap;
  std::set<uint32_t> defNodes;
  FIFOWorkList workList;

  bool keepActualOutFormalIn;
  bool keepAllSelfCycle;
  bool keepContextSelfCycle;

  std::vector<SelfCycleInfo> selfCycles;
  std::vector<SVFGNode *> nodesToRemove;
};

} // namespace analysis
} // namespace lotus
