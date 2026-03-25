/**
 * @file QualifierAnalysis.h
 * @brief Qualifier analysis for uninitialized/undefined data detection
 *
 * This opt-in analysis performs flow-sensitive qualifier inference to detect
 * uninitialized and undefined data usage in programs. It is implemented as a
 * specialized checker over a fixed qualifier domain rather than as a generic
 * qualifier framework.
 *
 * @author Lotus Analysis Framework
 */

#ifndef UBIANALYSIS_QUALIFIERANALYSIS_H
#define UBIANALYSIS_QUALIFIERANALYSIS_H

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/BasicBlock.h>

// #include "Alias/TypeQualifier/UBIAnalysis.h"
#include "Alias/TypeQualifier/Config.h"
#include "Alias/TypeQualifier/FunctionSummary.h"
#include "Alias/TypeQualifier/IntGlobal.h"
#include "Alias/TypeQualifier/NodeFactory.h"
#include "Alias/TypeQualifier/PtsSet.h"
#include "Alias/TypeQualifier/QualifierTypes.h"

#include <map>
#include <set>
#include <stack>
#include <unordered_map>
#include <unordered_set>

#define _CH 1

typedef std::map<NodeIndex, AndersPtsSet> PtsGraph;
typedef std::map<const llvm::Instruction *, PtsGraph> NodeToPtsGraph;
typedef std::map<const llvm::Instruction *, QualifierArray> NodeToQualifier;
typedef std::map<const llvm::BasicBlock *, QualifierArray> BBToQualifier;
typedef std::map<const llvm::Instruction *, RequirednessArray>
    NodeToRequiredness;
typedef std::map<const llvm::BasicBlock *, RequirednessArray> BBToRequiredness;

// alias set
typedef std::set<NodeIndex> NodeSet;
typedef std::unordered_map<NodeIndex, NodeSet> AAMap;
typedef std::unordered_map<const llvm::Instruction *, AAMap> NodeToAAMap;
enum WarnType { FUNCTION_PTR, NORMAL_PTR, DATA, OTHER };

/// @brief Main qualifier analysis pass
///
/// Performs qualifier analysis to detect uninitialized/undefined data usage.
/// Uses iterative refinement over a typed qualifier domain to infer states.
class QualifierAnalysis : public IterativeModulePass {
private:
  const llvm::DataLayout *DL;
  llvm::Module *module;
  unsigned FCounter;

  // if flag = true then we runn till check, else we run till inference to
  // converge
  bool runOnFunction(llvm::Function *, bool flag);

  void ptsJoin(PtsGraph &, PtsGraph &);

public:
  QualifierAnalysis(GlobalContext *Ctx_)
      : IterativeModulePass(Ctx_, "QualifierAnalysis"), FCounter(0) {}

  /// @brief Run the analysis on a module
  /// @param M The module to analyze
  /// @return true if analysis completed successfully
  virtual bool doModulePass(llvm::Module *);
  virtual bool doInitialization(llvm::Module *);
  virtual bool doFinalization(llvm::Module *);

  /// @brief Print statistics about the analysis
  void PrintStatistics();
  void collectRemaining();
  // used for recursive function:
  // void calSumForRec (std::set<llvm::Function*>&);
  void calSumForRec(std::vector<llvm::Function *> &);
  void printWarnForRec(std::vector<llvm::Function *> &);
  // void calSumForScc (std::vector<std::vector<llvm::Function*>>&);
  void calDepFuncs();
  void finalize();
  void getGlobals();
  /// @brief Run the full analysis
  void run();
};

class FuncAnalysis {
private:
  llvm::Function *F;
  const llvm::DataLayout *DL;
  llvm::Module *M;
  GlobalContext *Ctx;

  AndersNodeFactory nodeFactory;
  NodeToPtsGraph nPtsGraph;
  NodeToPtsGraph inPtsGraph;
  NodeToAAMap nAAMap;

  NodeToQualifier nQualiArray;
  NodeToQualifier nQualiUpdate;
  NodeToRequiredness nRequiredIn;
  NodeToRequiredness nRequiredOut;
  BBToRequiredness inRequiredArray;
  BBToRequiredness outRequiredArray;
  RequirednessArray requiredAtEntry;
  BBToQualifier inQualiArray;
  BBToQualifier outQualiArray;

  Summary fSummary;
  bool printWarning;
  std::map<llvm::Instruction *, int *> nUpdate;
  std::set<llvm::Instruction *> VisitIns;
  std::unordered_set<int> warningSet;
  std::unordered_set<BasicBlock *> terminationBB;
  std::set<std::string> warningVars;
  std::set<std::string> relatedBC;
  std::unordered_map<int, std::string> eToS = {
      {0, "FUNCTION_PTR"}, {1, "NORMAL_PTR"}, {2, "DATA"}, {3, "OTHER"}};
  // statistics:
  std::set<NodeIndex> uninitArg;
  std::set<NodeIndex> uninitOutArg;
  std::set<NodeIndex> ignoreOutArg;

  std::map<NodeIndex, std::set<NodeIndex>> relatedNode;

  void buildPtsGraph();
  void qualiInference();
  void runBackwardRequirednessAnalysis();
  void runForwardQualifierAnalysis();
  void buildFunctionSummary(llvm::ReturnInst *);
  void QualifierCheck();
  void computeAASet();
  void calStackVar();
  // utils
  void createNodeForPointerVal(const llvm::Value *, const llvm::Type *,
                               const NodeIndex valNode, PtsGraph &);
  void createInitNodes(PtsGraph &);
  PtsGraph processInstruction(llvm::Instruction *, PtsGraph &);
  NodeIndex processStruct(const llvm::Value *, const llvm::StructType *,
                          const NodeIndex valNode, PtsGraph &);
  void unionPtsGraph(PtsGraph &, const PtsGraph &);
  NodeIndex extendObjectSize(NodeIndex, const llvm::StructType *, PtsGraph &);
  void updateObjectNode(NodeIndex, NodeIndex, PtsGraph &);
  int handleContainerOf(const llvm::Instruction *, int64_t, NodeIndex,
                        PtsGraph &);
  void printRelatedBB(NodeIndex nodeIndex, const llvm::Value *,
                      std::set<const Instruction *> &visit, std::string,
                      int argNo = -1, int field = -1,
                      llvm::Function *Callee = NULL);
  void calculateRelatedBB(NodeIndex, const llvm::Instruction *I,
                          std::set<NodeIndex> &visit,
                          std::set<const BasicBlock *> &blacklist,
                          std::set<const BasicBlock *> &whitelist);
  void calculateBLForUse(const llvm::Instruction *I,
                         std::set<const BasicBlock *> &blacklist);
  void handleGEPConstant(const ConstantExpr *ce, PtsGraph &in);
  void addRelatedBC(llvm::Instruction *, NodeIndex,
                    llvm::Function *Callee = NULL);
  void addTerminationBB(llvm::BasicBlock *bb);
  bool isTerminationBB(llvm::BasicBlock *bb) { return terminationBB.count(bb); }
  // Used by qualifier inference
  void computeRequiredness(llvm::Instruction *, const RequirednessArray &,
                           RequirednessArray &);
  void computeQualifier(llvm::Instruction *, QualifierArray &,
                        QualifierArray &);
  void setGlobalQualies(QualifierArray &);
  void qualiJoin(QualifierArray &, QualifierArray &, unsigned);
  void updateJoin(QualifierArray &, QualifierArray &, unsigned);
  void requiredJoin(RequirednessArray &, const RequirednessArray &, unsigned);
  void markRequired(NodeIndex, RequirednessArray &);
  void markRequiredForValue(const llvm::Instruction *, const llvm::Value *,
                            RequirednessArray &);
  void materializeRequiredState(const llvm::Instruction *,
                                QualifierArray &) const;
  void insertUninit(const llvm::Instruction *, NodeIndex,
                    std::set<NodeIndex> &);
  // used for manually summaries functions
  void processInitFuncs(llvm::Instruction *, llvm::Function *, bool,
                        QualifierArray &, QualifierArray &);
  void processCopyFuncs(llvm::Instruction *, llvm::Function *, bool,
                        QualifierArray &, QualifierArray &);
  void processTransferFuncs(llvm::Instruction *, llvm::Function *, bool,
                            QualifierArray &, QualifierArray &);
  void processFuncs(llvm::Instruction *, llvm::Function *, bool,
                    QualifierArray &, QualifierArray &);

  // used for requirement propagation
  void backPropagateReq(llvm::Instruction *, llvm::Value *, QualifierArray &);
  void setReqFor(const llvm::Instruction *, const llvm::Value *,
                 QualifierArray &, std::set<const llvm::Value *> &);
  std::vector<llvm::Function *> resolveCallTargets(llvm::CallInst *CI) const;
  void applyResolvedCallTarget(llvm::Instruction *I, llvm::CallInst *CI,
                               llvm::Function *Func, QualifierArray &in,
                               QualifierArray &out, bool memsetUse);
  void DFS(llvm::Instruction *, NodeIndex);
  void summarizeFuncs(llvm::ReturnInst *);
  void propInitFuncs(llvm::Instruction *, RequirednessArray &);
  void propCopyFuncs(llvm::Instruction *, RequirednessArray &);
  void propTransferFuncs(llvm::Instruction *, RequirednessArray &);
  void propFuncs(llvm::Instruction *, llvm::Function *, RequirednessArray &);
  // used for function checking.
  void checkCopyFuncs(llvm::Instruction *, llvm::Function *);
  void checkTransferFuncs(llvm::Instruction *, llvm::Function *);
  void checkFuncs(llvm::Instruction *, llvm::Function *);
  bool warningReported(llvm::Instruction *, NodeIndex idx);
  enum WarnType getFieldTy(llvm::Type *, int);
  enum WarnType getWType(llvm::Type *);

  void initSummary();
  void getDef(llvm::Function *func) {
    if (func->empty()) {
      auto FIter = Ctx->Funcs.find(func->getName().str());
      if (FIter != Ctx->Funcs.end()) {
        func = FIter->second;
      }
    }
  }

public:
  FuncAnalysis(llvm::Function *F_, GlobalContext *Ctx_, bool flag)
      : F(F_), Ctx(Ctx_) {
    initializeFunctionModelSets(*Ctx_);
    M = F->getParent();
    if (M) {
      errs() << "FuncAnalysis for F :" << F->getName().str()
             << " module found.";
    }
    DL = &(M->getDataLayout());
    printWarning = flag;
    nodeFactory.setModule(M);
    nodeFactory.setDataLayout(DL);
    nodeFactory.setGlobalContext(Ctx_);
    nodeFactory.setStructAnalyzer(&(Ctx->structAnalyzer));
    nodeFactory.setGobjMap(&(Ctx->Gobjs));
    VisitIns.clear();
    warningSet.clear();
  }
  bool run();
  int getUninitArg() { return uninitArg.size(); }
  int getUninitOutArg() { return uninitOutArg.size(); }
  int getIgnoreOutArg() { return ignoreOutArg.size(); }
  const RequirednessArray &getRequiredAtEntry() const { return requiredAtEntry; }
  const Summary &getSummary() const { return fSummary; }
  bool isArgumentRequiredAtEntry(const llvm::Argument *arg) {
    NodeIndex idx = nodeFactory.getValueNodeFor(arg);
    return idx != AndersNodeFactory::InvalidIndex &&
           requiredAtEntry.at(idx) == RequirednessState::Required;
  }
};
class Tarjan {
private:
  std::unordered_map<llvm::Function *, std::set<llvm::Function *>> depCG;
  std::unordered_set<llvm::Function *> funcs;

  std::unordered_set<llvm::Function *> depVisit;
  std::stack<llvm::Function *> Stack;
  std::unordered_map<llvm::Function *, int> dfn;
  std::unordered_map<llvm::Function *, int> low;
  std::unordered_map<llvm::Function *, bool> inStack;
  std::vector<llvm::Function *> sc;
  std::vector<std::vector<llvm::Function *>> SCC;
  int ts = 1;
  int ind = 0;
  int sccSize = 0;
  int rec = 0;

  int biggest = 0;

public:
  Tarjan(
      std::unordered_map<llvm::Function *, std::set<llvm::Function *>> _depCG) {
    depCG = _depCG;
    for (auto item : _depCG) {
      funcs.insert(item.first);
      for (auto *callee : item.second) {
        funcs.insert(callee);
      }
    }
    depVisit.clear();
  };
  void getSCC(std::vector<std::vector<llvm::Function *>> &);
  void dfs(llvm::Function *F);
};
int64_t getGEPInstFieldNum(const llvm::GetElementPtrInst *gepInst,
                           const llvm::DataLayout *dataLayout,
                           const StructAnalyzer &structAnalyzer,
                           llvm::Module *module);

#endif // PROJECT_QUALIFIERANALYSIS_H
