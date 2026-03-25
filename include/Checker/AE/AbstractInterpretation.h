//===- AbstractInterpretation.h -- Abstract Execution---------------//
//
// Migrated from SVF's AE engine to Lotus.
//
// This file defines the AbstractInterpretation class, which implements
// sparse abstract execution for bug detection. The analysis performs
// abstract interpretation over LLVM IR using interval and address domains.
//
// Key features:
// - Sparse analysis: Only tracks values that affect bug detection
// - Cross-domain interaction: Uses Z3 solver to refine intervals via
// constraints
// - WTO-based iteration: Handles loops and recursion with widening/narrowing
// - Multiple bug detectors: Buffer overflow, null deref, use-after-free, etc.
//
// Based on the paper:
// "Precise Sparse Abstract Execution via Cross-Domain Interaction"
// Xiao Cheng, Jiawei Wang, Yulei Sui. ICSE 2024.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Checker/AE/AEDetector.h"
#include "Checker/AE/ICFGWTO.h"
#include "Checker/AE/RelationSolver.h"
#include "Checker/AE/SVFIRWrapper.h"

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

// Forward declarations for AserPTA
namespace aser {
template <typename ctx> class CallGraph;
template <typename ctx> class CallGraphNode;
} // namespace aser

// Forward declaration for internal PTA wrapper
class AbstractInterpretationPTAPtr;

namespace lotus {
namespace analysis {

class AEExtAPI;
class AEStat;
class AbstractInterpretation;
class ICFGWTO;
class ICFGCycleWTO;

/// AEStat collects statistics during abstract execution analysis.
///
/// Tracks metrics such as:
/// - Number of functions, blocks, and instructions analyzed
/// - Number of external vs internal call sites
/// - Abstract state sizes at program points
/// - Analysis time and memory usage
class AEStat {
public:
  AbstractInterpretation *_ae;
  int count{0};
  std::string memUsage;
  std::map<std::string, double> generalNumMap; // Use double for averages
  std::map<std::string, double> timeStatMap;
  uint64_t startTime{0};
  uint64_t endTime{0};

  // Additional statistics (matching SVF)
  uint32_t stmtCount{0};         // Total SVF statements processed
  uint32_t icfgNodeNum{0};       // Number of ICFG nodes
  uint32_t funcNum{0};           // Number of functions
  uint32_t extCallSiteNum{0};    // Number of external call sites
  uint32_t nonExtCallSiteNum{0}; // Number of non-external call sites

  AEStat(AbstractInterpretation *ae);
  ~AEStat() {}

  void countStateSize();
  void finializeStat();
  void performStat();

  double &getFunctionTrace() {
    if (generalNumMap.count("Function_Trace") == 0) {
      generalNumMap["Function_Trace"] = 0;
    }
    return generalNumMap["Function_Trace"];
  }

  double &getBlockTrace() {
    if (generalNumMap.count("Block_Trace") == 0) {
      generalNumMap["Block_Trace"] = 0;
    }
    return generalNumMap["Block_Trace"];
  }

  double &getICFGNodeTrace() {
    if (generalNumMap.count("ICFG_Node_Trace") == 0) {
      generalNumMap["ICFG_Node_Trace"] = 0;
    }
    return generalNumMap["ICFG_Node_Trace"];
  }

  void startClk();
  void endClk();
};

/// AbstractInterpretation is the main abstract execution engine.
///
/// This class performs sparse abstract interpretation over LLVM IR to detect
/// bugs such as buffer overflows, null pointer dereferences, use-after-free,
/// and invalid free operations.
///
/// Analysis workflow:
/// 1. Run pointer analysis (AserPTA) to get points-to information
/// 2. Build Weak Topological Order (WTO) for each function
/// 3. Perform abstract interpretation with widening/narrowing
/// 4. Run bug detectors on abstract states at each program point
/// 5. Report detected bugs
///
/// Recursion handling modes:
/// - TOP: Set all recursive function results to ⊤ (fastest, least precise)
/// - WIDEN_ONLY: Apply widening only (moderate precision)
/// - WIDEN_NARROW: Apply widening then narrowing (most precise, slowest)
class AbstractInterpretation {
  friend class AEStat;
  friend class BufOverflowDetector;
  friend class NullptrDerefDetector;

public:
  /// Recursion handling strategy
  enum HandleRecur {
    TOP,         ///< Set recursive results to ⊤ (top) immediately
    WIDEN_ONLY,  ///< Apply widening only for recursive functions
    WIDEN_NARROW ///< Apply widening then narrowing (default, most precise)
  };

  AbstractInterpretation();
  virtual ~AbstractInterpretation();

  /// Run abstract execution on an LLVM module
  /// @param module The LLVM module to analyze
  void runOnModule(llvm::Module *module);

  /// Main analysis entry point (called by runOnModule)
  void analyse();

  /// Reset all analysis state for analyzing a new module
  /// Prevents state leakage between module analyses and clears configured
  /// detectors.
  void reset();

  /// Get the singleton instance of AbstractInterpretation
  static AbstractInterpretation &getAEInstance() {
    static AbstractInterpretation instance;
    return instance;
  }

  /// Add a bug detector to the analysis
  /// If a detector of the same kind exists, it will be replaced
  /// @param detector Unique pointer to the detector to add
  void addDetector(std::unique_ptr<AEDetector> detector) {
    if (!detector)
      return;
    const AEDetector::DetectorKind newKind = detector->getKind();
    for (auto &existing : detectors) {
      if (existing && existing->getKind() == newKind) {
        existing = std::move(detector);
        return;
      }
    }
    detectors.push_back(std::move(detector));
  }

  /// Set the recursion handling mode
  void setRecursionMode(HandleRecur mode) { recursionMode_ = mode; }

  /// Set the widening delay (number of iterations before applying widening)
  void setWidenDelay(uint32_t delay) { widenDelay_ = delay; }

  /// Set whether to use strict checkpoint checking
  void setStrictCheckpoint(bool strict) { strictCheckpoint_ = strict; }

  // Checkpoint control options (matching SVF's Options)
  void setEnableBufOverflowCheck(bool enable) {
    enableBufOverflowCheck_ = enable;
  }
  void setEnableNullDerefCheck(bool enable) { enableNullDerefCheck_ = enable; }
  void setEnableDivZeroCheck(bool enable) { enableDivZeroCheck_ = enable; }
  void setEnableOverflowCheck(bool enable) { enableOverflowCheck_ = enable; }
  void setEnableMemLeakCheck(bool enable) { enableMemLeakCheck_ = enable; }
  void setAnalyzeAllFunctions(bool enable) { analyzeAllFunctions_ = enable; }

  std::set<const llvm::CallBase *> checkpoints;

  // Checkpoint statistics
  uint32_t getCheckpointCount() const { return checkpoints.size(); }
  uint32_t getCheckedCount() const { return checkedCheckpoints_.size(); }
  void markCheckpointChecked(const llvm::CallBase *call) {
    if (call) {
      checkedCheckpoints_.insert(call);
    }
  }

  AbstractState &getAbsStateFromTrace(const llvm::Instruction *val) {
    auto it = abstractTrace.find(val);
    if (it == abstractTrace.end()) {
      llvm::report_fatal_error(
          "No abstract state in trace for requested instruction");
    }
    return it->second;
  }

  bool hasAbsStateFromTrace(const llvm::Instruction *val) {
    return abstractTrace.count(val) != 0;
  }

  AEExtAPI *getUtils() const { return utils; }

  // Static wrapper for convenience (uses singleton instance)
  static uint32_t getValueIdStatic(const llvm::Value *val) {
    return getAEInstance().getValueId(val);
  }

  // Helper to get LLVM Value from ID (for AbstractState::getPointeeElement)
  const llvm::Value *getValueFromIdStatic(uint32_t id) const {
    return getValueFromId(id);
  }

private:
  llvm::Module *module_;
  AEStat *stat;
  AEExtAPI *utils;

  // Pointer analysis (AserPTA) - for call graph and indirect calls
  class PTAPtr; // Opaque pointer to avoid template dependencies in header
  std::unique_ptr<PTAPtr> pta_;
  bool ptaReady_{false};

  // SVFIRWrapper for PTA-based queries (like SVF's SVFIR)
  SVFIRWrapper *svfir_ = nullptr;

  // Call graph and recursion tracking
  llvm::CallGraph *callGraph_;
  std::set<const llvm::Function *> recursiveFuns_;
  std::map<const llvm::Function *, ICFGWTO *> funcToWTO_;
  std::map<const llvm::BasicBlock *, const ICFGCycleWTO *> cycleHeadToCycle_;
  std::map<const llvm::CallBase *, int> callSiteRecursionDepth_;
  // Track entry calls (calls from outside SCC to inside SCC) - these are NOT
  // recursive callsites
  std::set<std::pair<const llvm::CallBase *, const llvm::Function *>>
      nonRecursiveCallSites_;
  std::unordered_map<const llvm::Function *, uint32_t> recursiveSccIdMap_;
  std::unordered_map<uint32_t, std::vector<const llvm::Function *>>
      recursiveSccMembers_;
  HandleRecur recursionMode_; // Recursion handling mode (default: WIDEN_NARROW)
  uint32_t widenDelay_{3};
  bool strictCheckpoint_{true};

  // Checkpoint enable flags (matching SVF's Options)
  bool enableBufOverflowCheck_{false};
  bool enableNullDerefCheck_{false};
  bool enableDivZeroCheck_{false};
  bool enableOverflowCheck_{false};
  bool enableMemLeakCheck_{false}; // Disabled by default
  bool analyzeAllFunctions_{false};

  // Track which checkpoints have been checked
  std::set<const llvm::CallBase *> checkedCheckpoints_;

  std::vector<std::unique_ptr<AEDetector>> detectors;
  std::map<const llvm::Instruction *, AbstractState> abstractTrace;
  AbstractState globalState; // State for global variables
  std::map<std::string, std::function<void(const llvm::CallBase *)>> func_map;
  std::vector<const llvm::CallBase *> callSiteStack;

  // Stable value ID mapping
  std::unordered_map<const llvm::Value *, uint32_t> valueToIdMap_;
  std::unordered_map<uint32_t, const llvm::Value *> idToValueMap_;
  uint32_t nextValueId_;
  // Dedicated slot for function return summaries (must not collide with
  // NullPtr ID 0 or regular value IDs).
  enum : uint32_t { ReturnValueId = 0xFFFFFFFEu };
  // Current instruction being interpreted, used by transfer helpers that need
  // instruction-local state access.
  const llvm::Instruction *currentInstruction_{nullptr};
  // Functions reached during the current analysis traversal from entry.
  std::unordered_set<const llvm::Function *> analyzedFunctions_;

  void resetAnalysisState();

  void handleGlobalNode();
  void initWTO();
  void initCallGraphSCC();
  void collectCycleHeads(const std::list<const ICFGWTOComp *> &comps,
                         const llvm::Function *func);
  bool mergeStatesFromPredecessors(const llvm::BasicBlock *bb);
  void handleSingletonWTO(const llvm::BasicBlock *bb);
  void handleCycleWTO(const ICFGCycleWTO *cycle);
  void handleCallSite(const llvm::CallBase *call);
  void handleFunction(const llvm::Function *func);
  bool handleInstruction(const llvm::Instruction *inst);

  uint32_t getValueId(const llvm::Value *val);
  const llvm::Value *getValueFromId(uint32_t id) const;

private:
  void updateStateOnAddr(const llvm::AllocaInst *addr);
  void updateStateOnBinary(const llvm::BinaryOperator *binary);
  void updateStateOnCmp(const llvm::CmpInst *cmp);
  void updateStateOnLoad(const llvm::LoadInst *load);
  void updateStateOnStore(const llvm::StoreInst *store);
  void updateStateOnCast(const llvm::CastInst *cast);
  void updateStateOnCall(const llvm::CallBase *call);
  void updateStateOnRet(const llvm::ReturnInst *ret);
  void updateStateOnGep(const llvm::GetElementPtrInst *gep);
  void updateStateOnSelect(const llvm::SelectInst *select);
  void updateStateOnPhi(const llvm::PHINode *phi);

  // Call/Return parameter expansion (matching SVF's CallPE/RetPE handlers)
  void updateStateOnCallPE(const llvm::CallBase *call,
                           const llvm::Function *callee,
                           AbstractState &callState);
  void updateStateOnRetPE(const llvm::ReturnInst *ret,
                          const llvm::CallBase *call,
                          AbstractState &callerState);

  // Copy statement handler (matching SVF's CopyStmt)
  void updateStateOnCopy(const llvm::Value *dst, const llvm::Value *src);

  // Aggregate operation handlers (matching SVF)
  void updateStateOnExtractValue(const llvm::ExtractValueInst *ev);
  void updateStateOnInsertValue(const llvm::InsertValueInst *iv);
  void updateStateOnExtractElement(const llvm::ExtractElementInst *ee);
  void updateStateOnInsertElement(const llvm::InsertElementInst *ie);
  void updateStateOnShuffleVector(const llvm::ShuffleVectorInst *sv);

  // Get current instruction for state tracking
  const llvm::Instruction *getCurrentInstruction() const;

  bool isExtCall(const llvm::CallBase *callNode);
  void handleExtCall(const llvm::CallBase *callNode,
                     const std::vector<const llvm::Function *> &callees);
  bool isRecursiveFun(const llvm::Function *fun) const;
  bool isRecursiveCall(const llvm::CallBase *callNode);
  void handleFunCall(const llvm::CallBase *callNode,
                     const std::vector<const llvm::Function *> &callees);
  bool isRecursiveCallSite(const llvm::CallBase *callNode,
                           const llvm::Function *callee) const;
  bool skipRecursiveCall(const llvm::CallBase *callNode);
  bool collectCalleeReturnValue(const llvm::Function *callee,
                                AbstractValue &joinedReturn) const;
  void handleRecursiveSCC(const llvm::Function *seed);
  const llvm::Function *getCallee(const llvm::CallBase *callNode);
  bool shouldApplyNarrowing(const llvm::Function *fun);

  void collectCheckPoint();
  void checkPointAllSet();
  void recursiveCallPass(const llvm::CallBase *callNode,
                         const llvm::Function *callee);
  void setTopToObjInRecursion(const llvm::CallBase *callNode,
                              const llvm::Function *callee);

  bool isBranchFeasible(const llvm::BranchInst *branch, AbstractState &as,
                        bool isTrueEdge);
  bool isCmpBranchFeasible(const llvm::CmpInst *cmpInst, bool succ,
                           AbstractState &as);
  bool isSwitchBranchFeasible(const llvm::SwitchInst *switchInst,
                              int64_t caseValue, AbstractState &as);

  // Relational solver integration
  bool checkPathFeasibilityWithSolver(const AbstractState &as,
                                      const Z3Expr &pathConstraint);

  // Use RelationSolver for more precise branch feasibility checking
  // RSY (Rohn/Speelpenning/Yudell) abstract interpretation
  AbstractState computeRSY(const AbstractState &domain, const Z3Expr &phi);

  // Bilateral solver for refining abstract state with path constraints
  AbstractState computeBilateral(const AbstractState &domain,
                                 const Z3Expr &phi);

  RelationSolver relSolver_;

  static std::map<int32_t, int32_t> getReversePredicate() {
    static std::map<int32_t, int32_t> _reverse_predicate = {
        {llvm::CmpInst::FCMP_OEQ, llvm::CmpInst::FCMP_ONE},
        {llvm::CmpInst::FCMP_UEQ, llvm::CmpInst::FCMP_UNE},
        {llvm::CmpInst::FCMP_OGT, llvm::CmpInst::FCMP_OLE},
        {llvm::CmpInst::FCMP_OGE, llvm::CmpInst::FCMP_OLT},
        {llvm::CmpInst::FCMP_OLT, llvm::CmpInst::FCMP_OGE},
        {llvm::CmpInst::FCMP_OLE, llvm::CmpInst::FCMP_OGT},
        {llvm::CmpInst::FCMP_ONE, llvm::CmpInst::FCMP_OEQ},
        {llvm::CmpInst::FCMP_UNE, llvm::CmpInst::FCMP_UEQ},
        {llvm::CmpInst::ICMP_EQ, llvm::CmpInst::ICMP_NE},
        {llvm::CmpInst::ICMP_NE, llvm::CmpInst::ICMP_EQ},
        {llvm::CmpInst::ICMP_UGT, llvm::CmpInst::ICMP_ULE},
        {llvm::CmpInst::ICMP_ULT, llvm::CmpInst::ICMP_UGE},
        {llvm::CmpInst::ICMP_UGE, llvm::CmpInst::ICMP_ULT},
        {llvm::CmpInst::ICMP_SGT, llvm::CmpInst::ICMP_SLE},
        {llvm::CmpInst::ICMP_SLT, llvm::CmpInst::ICMP_SGE},
        {llvm::CmpInst::ICMP_SGE, llvm::CmpInst::ICMP_SLT},
    };
    return _reverse_predicate;
  }

  static std::map<int32_t, int32_t> getSwitchLhsRhsPredicate() {
    static std::map<int32_t, int32_t> _switch_lhsrhs_predicate = {
        {llvm::CmpInst::FCMP_OEQ, llvm::CmpInst::FCMP_OEQ},
        {llvm::CmpInst::FCMP_UEQ, llvm::CmpInst::FCMP_UEQ},
        {llvm::CmpInst::FCMP_OGT, llvm::CmpInst::FCMP_OLT},
        {llvm::CmpInst::FCMP_OGE, llvm::CmpInst::FCMP_OLE},
        {llvm::CmpInst::FCMP_OLT, llvm::CmpInst::FCMP_OGT},
        {llvm::CmpInst::FCMP_OLE, llvm::CmpInst::FCMP_OGE},
        {llvm::CmpInst::FCMP_ONE, llvm::CmpInst::FCMP_ONE},
        {llvm::CmpInst::FCMP_UNE, llvm::CmpInst::FCMP_UNE},
        {llvm::CmpInst::ICMP_EQ, llvm::CmpInst::ICMP_EQ},
        {llvm::CmpInst::ICMP_NE, llvm::CmpInst::ICMP_NE},
        {llvm::CmpInst::ICMP_UGT, llvm::CmpInst::ICMP_ULT},
        {llvm::CmpInst::ICMP_ULT, llvm::CmpInst::ICMP_UGT},
        {llvm::CmpInst::ICMP_UGE, llvm::CmpInst::ICMP_ULE},
        {llvm::CmpInst::ICMP_SGT, llvm::CmpInst::ICMP_SLT},
        {llvm::CmpInst::ICMP_SLT, llvm::CmpInst::ICMP_SGT},
        {llvm::CmpInst::ICMP_SGE, llvm::CmpInst::ICMP_SLE},
    };
    return _switch_lhsrhs_predicate;
  }

public:
  AEExtAPI *getUtils() { return utils; }

  // Get pointer analysis for points-to information
  // Returns nullptr if PTA not ready
  void *getPTAPass() const;

  bool isPTAReady() const { return ptaReady_; }

  // Get the module being analyzed
  llvm::Module *getModule() const { return module_; }

  // Get all possible callees for a call (direct + indirect resolved via PTA)
  std::vector<const llvm::Function *>
  getCallees(const llvm::CallBase *callNode) const;
};

} // namespace analysis
} // namespace lotus
