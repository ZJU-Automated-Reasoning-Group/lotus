//===- SaberCondAllocator.h -- Path condition
// manipulation---------------------//
//
// Migrated from SVF's SABER engine to Lotus.
//
//===----------------------------------------------------------------------===//

#ifndef PATHALLOCATOR_H_
#define PATHALLOCATOR_H_

#include "Checker/Saber/Z3Expr.h"
#include "IR/SVFG/SVFG.h"

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>

namespace lotus {
namespace analysis {

class SaberCondAllocator {

public:
  using Condition = Z3Expr;
  using IndexToTermInstMap = std::map<uint32_t, const llvm::Instruction *>;
  using CondPosMap = std::map<uint32_t, Condition>;
  using BBCondMap = std::map<const llvm::BasicBlock *, CondPosMap>;
  using BasicBlockSet = std::set<const llvm::BasicBlock *>;
  using FunToExitBBsMap = std::map<std::string, BasicBlockSet>;
  using BBToCondMap = std::map<const llvm::BasicBlock *, Condition>;
  using CFWorkList = std::deque<const llvm::BasicBlock *>;
  using SVFGNodeToSVFGNodeSetMap =
      std::map<const SVFGNode *, std::set<const SVFGNode *>>;
  using NodeBS = std::set<uint32_t>;

  SaberCondAllocator();
  virtual ~SaberCondAllocator() = default;

  void allocate();
  void reset(bool preserve_removed_su_vfg_edges = false);

  Condition newCond(const llvm::Instruction *inst);
  void setCondInst(Condition cond, const llvm::Instruction *inst);
  void setNegCondInst(Condition cond, const llvm::Instruction *inst);
  const llvm::Instruction *getCondInst(uint32_t id) const;
  bool isNegCond(uint32_t id) const;

  Condition getBranchCond(const llvm::BasicBlock *bb,
                          const llvm::BasicBlock *succ) const;
  Condition getEvalBrCond(const llvm::BasicBlock *bb,
                          const llvm::BasicBlock *succ);
  void setBranchCond(const llvm::BasicBlock *bb, const llvm::BasicBlock *succ,
                     const Condition &cond);

  Condition evaluateBranchCond(const llvm::BasicBlock *bb,
                               const llvm::BasicBlock *succ);

  bool isBBCallsProgExit(const llvm::BasicBlock *bb);
  Condition evaluateProgExit(const llvm::BasicBlock *bb,
                             const llvm::BasicBlock *succ);
  Condition evaluateTestNullLikeExpr(const llvm::Instruction *brInst,
                                     const llvm::BasicBlock *succ);
  Condition evaluateLoopExitBranch(const llvm::BasicBlock *bb,
                                   const llvm::BasicBlock *dst);

  Condition ComputeIntraVFGGuard(const llvm::BasicBlock *srcBB,
                                 const llvm::BasicBlock *dstBB);
  Condition ComputeInterCallVFGGuard(const llvm::BasicBlock *srcBB,
                                     const llvm::BasicBlock *dstBB,
                                     const llvm::BasicBlock *callBB);
  Condition ComputeInterRetVFGGuard(const llvm::BasicBlock *srcBB,
                                    const llvm::BasicBlock *dstBB,
                                    const llvm::BasicBlock *retBB);

  Condition getPHIComplementCond(const llvm::BasicBlock *BB1,
                                 const llvm::BasicBlock *BB2,
                                 const llvm::BasicBlock *BB0);

  bool isSatisfiable(const Condition &condition);
  bool isAllPathReachable(const Condition &condition) const {
    return isEquivalentBranchCond(condition, getTrueCond());
  }
  bool isEquivalentBranchCond(const Condition &lhs, const Condition &rhs) const;
  void extractSubConds(const Condition &condition, NodeBS &support) const;
  std::string dumpCond(const Condition &cond) const;

  Condition getFalseCond() const { return Condition::getFalseCond(); }
  Condition getTrueCond() const { return Condition::getTrueCond(); }

  static Condition condAnd(const Condition &lhs, const Condition &rhs) {
    return Condition::AND(lhs, rhs);
  }
  static Condition condOr(const Condition &lhs, const Condition &rhs) {
    return Condition::OR(lhs, rhs);
  }
  static Condition condNeg(const Condition &cond) {
    return Condition::NEG(cond);
  }

  const llvm::Instruction *getCurEvalICFGNode() const { return curEvalInst; }
  void setCurEvalICFGNode(const llvm::Instruction *inst) { curEvalInst = inst; }

  const SVFGNode *getCurEvalSVFGNode() const { return curEvalSVFGNode; }
  void setCurEvalSVFGNode(const SVFGNode *node) { curEvalSVFGNode = node; }

  SVFGNodeToSVFGNodeSetMap &getRemovedSUVFEdges() { return removedSUVFEdges; }
  const SVFGNodeToSVFGNodeSetMap &getRemovedSUVFEdges() const {
    return removedSUVFEdges;
  }

  void setCFCond(const llvm::BasicBlock *bb, const Condition &cond);
  const Condition &getCFCond(const llvm::BasicBlock *bb) const;
  bool hasCFCond(const llvm::BasicBlock *bb) const;
  void clearCFCond();

  void clearCurEvalSVFGNode() { curEvalSVFGNode = nullptr; }

  bool isTestNullExpr(const llvm::Instruction *test) const;
  bool isTestNotNullExpr(const llvm::Instruction *test) const;
  bool isEQCmp(const llvm::CmpInst *cmp) const;
  bool isNECmp(const llvm::CmpInst *cmp) const;
  bool isTestContainsNullAndTheValue(const llvm::CmpInst *cmp) const;

  NodeBS exactCondElem(const Condition &cond) const {
    NodeBS support;
    extractSubConds(cond, support);
    return support;
  }
  void setFinalCond(const Condition &cond) { finalCond = cond; }
  const Condition &getFinalCond() const { return finalCond; }

  std::string getMemUsage() { return "N/A"; }
  uint32_t getCondNum() { return totalCondNum_; }

  void printPathCond();

  void setModule(llvm::Module *M);

  void initDominatorsForFunction(const llvm::Function *func);
  void initPostDominatorsForFunction(const llvm::Function *func);
  void initLoopInfoForFunction(const llvm::Function *func);

protected:
  void allocateForBB(const llvm::BasicBlock *bb);
  void collectBBCallingProgExit(const llvm::BasicBlock *bb);

  bool dominate(const llvm::BasicBlock *bb1, const llvm::BasicBlock *bb2) const;
  bool postDominate(const llvm::BasicBlock *bb1,
                    const llvm::BasicBlock *bb2) const;

  std::map<const llvm::Function *, std::unique_ptr<llvm::DominatorTree>>
      dtCache_;
  std::map<const llvm::Function *, std::unique_ptr<llvm::PostDominatorTree>>
      pdtCache_;
  std::map<const llvm::Function *, std::unique_ptr<llvm::LoopInfo>>
      loopInfoCache_;
  /// Cached return blocks per function (for ComputeInterRetVFGGuard).
  std::map<const llvm::Function *, std::vector<const llvm::BasicBlock *>>
      retBlocksCache_;

  uint32_t totalCondNum_ = 0;
  const llvm::Instruction *curEvalInst = nullptr;
  const SVFGNode *curEvalSVFGNode = nullptr;
  SVFGNodeToSVFGNodeSetMap removedSUVFEdges;

  BBCondMap bbConds;
  IndexToTermInstMap condToInst;
  std::set<uint32_t> negCondIds;
  FunToExitBBsMap funToExitBBsMap;
  BBToCondMap cfConds;
  std::vector<Condition> conditionVec;
  Condition finalCond;
  llvm::Module *module_ = nullptr;
};

} // namespace analysis
} // namespace lotus

#endif
