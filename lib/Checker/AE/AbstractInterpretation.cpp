//===- AbstractInterpretation.cpp -- Abstract Execution-------------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#include "Checker/AE/AbstractInterpretation.h"

#include "Checker/AE/AbsExtAPI.h"
#include "Checker/AE/RelationSolver.h"
#include "Checker/AE/SVFIRWrapper.h"
#include "Checker/Report/BugReportMgr.h"
#include "Solvers/SMT/LIBSMT/Z3Expr.h"

// AserPTA includes
#include "Alias/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/AserPTA/PointerAnalysis/Graph/CallGraph.h"
#include "Alias/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/AserPTA/PointerAnalysis/PointerAnalysisPass.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/PointsTo/BitVectorPTS.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/WavePropagation.h"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <queue>
#include <set>
#include <stack>
#include <unordered_map>
#include <unordered_set>

#include <llvm/ADT/GraphTraits.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace analysis {

namespace {

bool resolveGlobalAddress(const llvm::Constant *constant,
                          const llvm::DataLayout &dl,
                          const llvm::Value *&baseValue, int64_t &byteOffset) {
  if (!constant)
    return false;

  if (const auto *alias = llvm::dyn_cast<llvm::GlobalAlias>(constant)) {
    if (const llvm::Constant *aliasee = alias->getAliasee()) {
      return resolveGlobalAddress(aliasee, dl, baseValue, byteOffset);
    }
    return false;
  }

  if (const auto *gv = llvm::dyn_cast<llvm::GlobalValue>(constant)) {
    baseValue = gv;
    byteOffset = 0;
    return true;
  }

  const auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(constant);
  if (!ce)
    return false;

  switch (ce->getOpcode()) {
  case llvm::Instruction::BitCast:
  case llvm::Instruction::AddrSpaceCast:
    return resolveGlobalAddress(llvm::cast<llvm::Constant>(ce->getOperand(0)),
                                dl, baseValue, byteOffset);
  case llvm::Instruction::GetElementPtr: {
    const llvm::Constant *baseConst =
        llvm::cast<llvm::Constant>(ce->getOperand(0));
    if (!resolveGlobalAddress(baseConst, dl, baseValue, byteOffset)) {
      return false;
    }

    llvm::APInt offset(64, 0, /*isSigned=*/true);
    llvm::Instruction *gepInst = ce->getAsInstruction();
    const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(gepInst);
    const bool ok = gep && gep->accumulateConstantOffset(dl, offset);
    if (gepInst) {
      gepInst->deleteValue();
    }
    if (!ok) {
      return false;
    }
    byteOffset += offset.getSExtValue();
    return true;
  }
  default:
    return false;
  }
}

AbstractValue evaluateConstantInitializer(AbstractState &state,
                                          const llvm::Constant *init,
                                          const llvm::DataLayout &dl) {
  if (!init) {
    return AbstractValue(IntervalValue::top());
  }

  if (llvm::isa<llvm::ConstantPointerNull>(init)) {
    return AbstractValue(AddressValue(NullMemAddr));
  }

  if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(init)) {
    return AbstractValue(IntervalValue(ci->getSExtValue()));
  }

  if (const auto *cfp = llvm::dyn_cast<llvm::ConstantFP>(init)) {
    double val = cfp->getValueAPF().convertToDouble();
    return AbstractValue(IntervalValue(val, val));
  }

  const llvm::Value *baseValue = nullptr;
  int64_t byteOffset = 0;
  if (resolveGlobalAddress(init, dl, baseValue, byteOffset)) {
    uint32_t baseId = AbstractInterpretation::getValueIdStatic(baseValue);
    if (byteOffset <= 0) {
      return AbstractValue(
          AddressValue(AddressValue::getVirtualMemAddress(baseId)));
    }

    AddressValue fieldAddrs = state.getGepObjAddrs(baseId, IntervalValue(byteOffset));
    if (!fieldAddrs.isBottom()) {
      uint32_t baseSize = state.getObjSize(baseId);
      if (baseSize > static_cast<uint32_t>(byteOffset)) {
        uint32_t remaining = baseSize - static_cast<uint32_t>(byteOffset);
        for (uint32_t addr : fieldAddrs) {
          state.setObjSize(state.getIDFromAddr(addr), remaining);
        }
      }
      return AbstractValue(fieldAddrs);
    }
    return AbstractValue(
        AddressValue(AddressValue::getVirtualMemAddress(baseId)));
  }

  return AbstractValue(IntervalValue::top());
}

void storeInitializerValue(AbstractState &state, uint32_t baseValueId,
                           uint32_t byteOffset, bool useFieldObject,
                           const AbstractValue &value) {
  AddressValue targetAddrs;
  if (useFieldObject || byteOffset != 0) {
    targetAddrs = state.getGepObjAddrs(baseValueId, IntervalValue(byteOffset));
  } else if (state.inVarToAddrsTable(baseValueId)) {
    targetAddrs = state[baseValueId].getAddrs();
  }

  for (uint32_t addr : targetAddrs) {
    state.store(addr, value);
  }
}

void materializeGlobalInitializer(AbstractState &state, uint32_t baseValueId,
                                  uint32_t byteOffset, bool useFieldObject,
                                  const llvm::Constant *init,
                                  const llvm::DataLayout &dl) {
  if (!init)
    return;

  if (llvm::isa<llvm::ConstantAggregateZero>(init)) {
    if (init->getType()->isAggregateType()) {
      if (const auto *structTyConst =
              llvm::dyn_cast<llvm::StructType>(init->getType())) {
        auto *structTy = const_cast<llvm::StructType *>(structTyConst);
        const llvm::StructLayout *layout =
            dl.getStructLayout(structTy);
        for (unsigned i = 0; i < structTy->getNumElements(); ++i) {
          llvm::Constant *fieldInit =
              llvm::Constant::getNullValue(structTy->getElementType(i));
          materializeGlobalInitializer(state, baseValueId,
                                       byteOffset + layout->getElementOffset(i),
                                       true, fieldInit, dl);
        }
        return;
      }
      if (const auto *arrayTy =
              llvm::dyn_cast<llvm::ArrayType>(init->getType())) {
        llvm::Type *elemTy = arrayTy->getElementType();
        uint64_t elemSize = elemTy->isSized() ? dl.getTypeAllocSize(elemTy) : 1;
        for (uint64_t i = 0; i < arrayTy->getNumElements(); ++i) {
          llvm::Constant *elemInit = llvm::Constant::getNullValue(elemTy);
          materializeGlobalInitializer(
              state, baseValueId,
              byteOffset + static_cast<uint32_t>(i * elemSize), true, elemInit,
              dl);
        }
        return;
      }
      if (const auto *vecTy =
              llvm::dyn_cast<llvm::FixedVectorType>(init->getType())) {
        llvm::Type *elemTy = vecTy->getElementType();
        uint64_t elemSize = elemTy->isSized() ? dl.getTypeAllocSize(elemTy) : 1;
        for (unsigned i = 0; i < vecTy->getNumElements(); ++i) {
          llvm::Constant *elemInit = llvm::Constant::getNullValue(elemTy);
          materializeGlobalInitializer(
              state, baseValueId,
              byteOffset + static_cast<uint32_t>(i * elemSize), true, elemInit,
              dl);
        }
        return;
      }
    }
    storeInitializerValue(state, baseValueId, byteOffset, useFieldObject,
                          evaluateConstantInitializer(state, init, dl));
    return;
  }

  if (const auto *structInit = llvm::dyn_cast<llvm::ConstantStruct>(init)) {
    auto *structTy = const_cast<llvm::StructType *>(structInit->getType());
    const llvm::StructLayout *layout = dl.getStructLayout(structTy);
    for (unsigned i = 0; i < structInit->getNumOperands(); ++i) {
      const llvm::Constant *fieldInit =
          llvm::cast<llvm::Constant>(structInit->getOperand(i));
      materializeGlobalInitializer(state, baseValueId,
                                   byteOffset + layout->getElementOffset(i),
                                   true, fieldInit, dl);
    }
    return;
  }

  if (const auto *arrayInit = llvm::dyn_cast<llvm::ConstantArray>(init)) {
    llvm::Type *elemTy = arrayInit->getType()->getElementType();
    uint64_t elemSize = elemTy->isSized() ? dl.getTypeAllocSize(elemTy) : 1;
    for (unsigned i = 0; i < arrayInit->getNumOperands(); ++i) {
      const llvm::Constant *elemInit =
          llvm::cast<llvm::Constant>(arrayInit->getOperand(i));
      materializeGlobalInitializer(state, baseValueId,
                                   byteOffset +
                                       static_cast<uint32_t>(i * elemSize),
                                   true, elemInit, dl);
    }
    return;
  }

  if (const auto *vecInit = llvm::dyn_cast<llvm::ConstantVector>(init)) {
    llvm::Type *elemTy = vecInit->getType()->getElementType();
    uint64_t elemSize = elemTy->isSized() ? dl.getTypeAllocSize(elemTy) : 1;
    for (unsigned i = 0; i < vecInit->getNumOperands(); ++i) {
      const llvm::Constant *elemInit =
          llvm::cast<llvm::Constant>(vecInit->getOperand(i));
      materializeGlobalInitializer(state, baseValueId,
                                   byteOffset +
                                       static_cast<uint32_t>(i * elemSize),
                                   true, elemInit, dl);
    }
    return;
  }

  storeInitializerValue(state, baseValueId, byteOffset, useFieldObject,
                        evaluateConstantInitializer(state, init, dl));
}

void materializeConstantValue(AbstractState &as, const llvm::Value *val,
                              uint32_t valueId) {
  if (!val)
    return;

  if (as.inVarToValTable(valueId) || as.inVarToAddrsTable(valueId))
    return;

  if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(val)) {
    as[valueId] = AbstractValue(IntervalValue(ci->getSExtValue()));
    return;
  }

  if (const auto *cfp = llvm::dyn_cast<llvm::ConstantFP>(val)) {
    double num = cfp->getValueAPF().convertToDouble();
    as[valueId] = AbstractValue(IntervalValue(num, num));
    return;
  }

  if (llvm::isa<llvm::ConstantPointerNull>(val)) {
    as[valueId] = AbstractValue(AddressValue(NullMemAddr));
  }
}

void clearAEBugReports() {
  static constexpr const char *AEBugTypeNames[] = {
      "AE Buffer Overflow", "AE Null Dereference", "AE Divide By Zero",
      "AE Integer Overflow", "AE Use After Free", "AE Invalid Free",
      "AE Memory Leak"};

  BugReportMgr &mgr = BugReportMgr::get_instance();
  std::vector<int> ids;
  for (const char *name : AEBugTypeNames) {
    int id = mgr.find_bug_type(name);
    if (id >= 0) {
      ids.push_back(id);
    }
  }
  mgr.clear_reports_for_types(ids);
}

bool isAEQuietMode() {
  const char *env = std::getenv("LOTUS_AE_QUIET");
  return env != nullptr && env[0] != '\0' && env[0] != '0';
}

} // namespace

// Opaque pointer implementation for AserPTA
using PTASolver = aser::WavePropagation<aser::DefaultLangModel<
    aser::NoCtx, aser::FSMemModel<aser::NoCtx>, aser::BitVectorPTS>>;
using PTAPass = PointerAnalysisPass<PTASolver>;

// Forward declaration wrapper
class AbstractInterpretation::PTAPtr {
public:
  std::unique_ptr<PTAPass> pass;
  PTAPtr() : pass(std::make_unique<PTAPass>()) {}
  ~PTAPtr() = default;
};

AbstractInterpretation::AbstractInterpretation()
    : module_(nullptr), pta_(std::make_unique<PTAPtr>()),
      recursionMode_(WIDEN_NARROW), nextValueId_(3) {
  stat = new AEStat(this);
  utils = nullptr;
  callGraph_ = nullptr;
}

AbstractInterpretation::~AbstractInterpretation() {
  delete stat;
  delete utils;
  delete svfir_;
  svfir_ = nullptr;
  // Clean up WTO objects
  for (auto &pair : funcToWTO_) {
    delete pair.second;
  }
  funcToWTO_.clear();
}

void AbstractInterpretation::resetAnalysisState() {
  // Clear all state to prevent leakage between module analyses while
  // preserving the configured detector set and user-selected options.
  abstractTrace.clear();
  globalState = AbstractState();
  checkpoints.clear();
  callSiteStack.clear();
  checkedCheckpoints_.clear();
  delete svfir_;
  svfir_ = nullptr;

  // Clear value ID mappings
  valueToIdMap_.clear();
  idToValueMap_.clear();
  // Reserve low IDs used by special memory sentinels:
  //   0 -> null pointer
  //   2 -> InvalidMemAddr internal ID
  // Start from 3 so regular values never collide with sentinels.
  nextValueId_ = 3;

  // Clear call graph and recursion tracking
  recursiveFuns_.clear();
  callSiteRecursionDepth_.clear();
  nonRecursiveCallSites_.clear();
  recursiveSccIdMap_.clear();
  recursiveSccMembers_.clear();
  cycleHeadToCycle_.clear();
  analyzedFunctions_.clear();

  // Clean up WTO objects
  for (auto &pair : funcToWTO_) {
    delete pair.second;
  }
  funcToWTO_.clear();

  // Reset statistics
  if (stat) {
    stat->count = 0;
    stat->generalNumMap.clear();
    stat->timeStatMap.clear();
    stat->startTime = 0;
    stat->endTime = 0;
    stat->stmtCount = 0;
    stat->icfgNodeNum = 0;
    stat->funcNum = 0;
    stat->extCallSiteNum = 0;
    stat->nonExtCallSiteNum = 0;
  }

  for (auto &detector : detectors) {
    if (detector) {
      detector->reset();
    }
  }
  // Note: pta_ is kept (can be reused)
  ptaReady_ = false;
}

void AbstractInterpretation::reset() {
  resetAnalysisState();

  recursionMode_ = WIDEN_NARROW;
  widenDelay_ = 3;
  strictCheckpoint_ = true;
  enableBufOverflowCheck_ = false;
  enableNullDerefCheck_ = false;
  enableDivZeroCheck_ = false;
  enableOverflowCheck_ = false;
  enableMemLeakCheck_ = false;
  analyzeAllFunctions_ = false;
  detectors.clear();
}

void *AbstractInterpretation::getPTAPass() const {
  if (ptaReady_ && pta_) {
    return pta_->pass.get();
  }
  return nullptr;
}

void AbstractInterpretation::runOnModule(llvm::Module *module) {
  resetAnalysisState();
  clearAEBugReports();

  module_ = module;

  auto pruneDetector = [&](AEDetector::DetectorKind kind, bool enabled) {
    if (enabled)
      return;
    detectors.erase(
        std::remove_if(detectors.begin(), detectors.end(),
                       [&](const std::unique_ptr<AEDetector> &detector) {
                         return detector && detector->getKind() == kind;
                       }),
        detectors.end());
  };
  pruneDetector(AEDetector::DIV_ZERO, enableDivZeroCheck_);
  pruneDetector(AEDetector::INT_OVERFLOW, enableOverflowCheck_);
  pruneDetector(AEDetector::MEMORY_LEAK, enableMemLeakCheck_);

  // Always run pointer analysis (required for AE)
  pta_->pass->analyze(module);
  ptaReady_ = true;

  // Create SVFIRWrapper for PTA-based queries (like SVF's SVFIR)
  svfir_ = new SVFIRWrapper(getPTAPass(), module_);

  // Initialize global state with SVFIRWrapper
  globalState.svfir_ = svfir_;

  // Delete old utils if exists
  if (utils) {
    delete utils;
  }
  utils = new AEExtAPI(abstractTrace);

  auto hasDetectorKind = [&](AEDetector::DetectorKind kind) {
    return std::any_of(
        detectors.begin(), detectors.end(),
        [&](const std::unique_ptr<AEDetector> &detector) {
          return detector && detector->getKind() == kind;
        });
  };

  if (enableDivZeroCheck_ && !hasDetectorKind(AEDetector::DIV_ZERO)) {
    addDetector(std::make_unique<DivZeroDetector>());
  }
  if (enableOverflowCheck_ && !hasDetectorKind(AEDetector::INT_OVERFLOW)) {
    addDetector(std::make_unique<OverflowDetector>());
  }
  if (enableBufOverflowCheck_ && !hasDetectorKind(AEDetector::BUF_OVERFLOW)) {
    addDetector(std::make_unique<BufOverflowDetector>());
  }
  if (enableNullDerefCheck_ && !hasDetectorKind(AEDetector::NULL_DEREF)) {
    addDetector(std::make_unique<NullptrDerefDetector>());
  }
  if (enableMemLeakCheck_ && !hasDetectorKind(AEDetector::MEMORY_LEAK)) {
    addDetector(std::make_unique<MemLeakDetector>());
  }

  stat->startClk();
  collectCheckPoint();
  analyse();
  checkPointAllSet();
  stat->endClk();

  stat->finializeStat();
  if (!isAEQuietMode()) {
    stat->performStat();

    for (auto &detector : detectors)
      detector->reportBug();
    llvm::outs().flush();
    llvm::errs().flush();
  }
}

/// Program entry
void AbstractInterpretation::analyse() {
  // Initialize call graph SCC detection
  initCallGraphSCC();

  // Initialize WTO for all functions
  initWTO();

  handleGlobalNode();

  // Collect module-level statistics once (static counts).
  std::vector<const llvm::Function *> functions;
  functions.reserve(module_->size());
  for (const llvm::Function &func : *module_) {
    if (func.isDeclaration()) {
      continue;
    }
    functions.push_back(&func);
    stat->funcNum++;
    for (const llvm::BasicBlock &bb : func) {
      for (const llvm::Instruction &inst : bb) {
        stat->icfgNodeNum++;
        const auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (!call) {
          continue;
        }
        if (isExtCall(call)) {
          stat->extCallSiteNum++;
        } else {
          stat->nonExtCallSiteNum++;
        }
      }
    }
  }

  // Match SVF by default: start from main when present.
  // Whole-module analysis remains available for testing/debugging.
  const llvm::Function *mainFunc = module_->getFunction("main");
  if (mainFunc && !mainFunc->isDeclaration()) {
    handleFunction(mainFunc);
  }

  if (mainFunc && !analyzeAllFunctions_) {
    return;
  }

  for (const llvm::Function *func : functions) {
    if (!func || func == mainFunc)
      continue;
    if (analyzedFunctions_.count(func))
      continue;
    handleFunction(func);
  }
}

/// Handle global node
void AbstractInterpretation::handleGlobalNode() {
  const llvm::DataLayout &dl = module_->getDataLayout();

  for (auto &global : module_->globals()) {
    uint32_t globalId = getValueId(&global);
    uint32_t globalAddr = AddressValue::getVirtualMemAddress(globalId);

    // In LLVM IR a global variable is used as its address.
    globalState[globalId] = AbstractValue(AddressValue(globalAddr));
    if (global.getValueType()->isSized()) {
      globalState.setObjSize(globalId, dl.getTypeAllocSize(global.getValueType()));
    }

    if (global.hasInitializer() && !global.isDeclaration()) {
      materializeGlobalInitializer(globalState, globalId, 0,
                                   global.getValueType()->isAggregateType(),
                                   global.getInitializer(), dl);
    }
  }

  // Initialize null pointer constant (ID 0) in global state
  globalState[0] = AbstractValue(AddressValue(NullMemAddr));
  globalState[AbstractState::BlkPtr] = AbstractValue(IntervalValue::top());
}

void AbstractInterpretation::initCallGraphSCC() {
  // Build call graph using AserPTA (includes indirect calls)
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::Function *>>
      callGraph;

  // Get call graph from AserPTA
  const aser::CallGraph<aser::NoCtx> *cg = nullptr;
  if (ptaReady_ && pta_ && pta_->pass) {
    cg = pta_->pass->getPTA()->getCallGraph();
  }

  // Build a map: LLVM Function -> set of callee Functions
  std::unordered_map<const llvm::Function *, std::set<const llvm::Function *>>
      funcToCallees;

  // First, collect all direct calls from LLVM IR (for completeness)
  for (const llvm::Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    for (const llvm::BasicBlock &BB : F) {
      for (const llvm::Instruction &I : BB) {
        const llvm::CallBase *cb = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!cb)
          continue;
        if (const llvm::Function *direct = cb->getCalledFunction()) {
          if (!direct->isDeclaration()) {
            funcToCallees[&F].insert(direct);
          }
        }
      }
    }
  }

  // Now enrich with AserPTA's call graph (includes indirect calls)
  // Iterate over all call graph nodes to find indirect calls and their targets
  if (cg) {
    for (auto nodeIt = cg->begin(); nodeIt != cg->end(); ++nodeIt) {
      const auto *cgNode = *nodeIt;
      if (!cgNode)
        continue;

      // Process indirect call nodes to find resolved targets
      if (cgNode->isIndirectCall()) {
        if (auto *indCall = cgNode->getTargetFunPtr()) {
          const llvm::Instruction *callInst = indCall->getCallSite();
          if (callInst && callInst->getFunction()) {
            const llvm::Function *callerFunc = callInst->getFunction();
            if (!callerFunc->isDeclaration()) {
              // Get all resolved targets for this indirect call
              for (auto *resolved : indCall->getResolvedNode()) {
                if (resolved && !resolved->isIndirectCall()) {
                  if (auto *targetFun = resolved->getTargetFun()) {
                    const llvm::Function *calleeFunc = targetFun->getFunction();
                    if (calleeFunc && !calleeFunc->isDeclaration()) {
                      funcToCallees[callerFunc].insert(calleeFunc);
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  // Convert set-based map to vector-based map for SCC algorithm
  for (const auto &pair : funcToCallees) {
    callGraph[pair.first] = std::vector<const llvm::Function *>(
        pair.second.begin(), pair.second.end());
  }

  // Tarjan's SCC algorithm (iterative version to avoid stack overflow)
  std::unordered_map<const llvm::Function *, uint32_t> index, lowlink;
  std::stack<const llvm::Function *> stk;
  std::unordered_set<const llvm::Function *> onStack;
  uint32_t nextIndex = 0;
  std::vector<std::set<const llvm::Function *>> sccs;

  struct Frame {
    const llvm::Function *f;
    std::vector<const llvm::Function *>::const_iterator it;
    std::vector<const llvm::Function *>::const_iterator end;
  };
  std::stack<Frame> worklist;

  auto visit = [&](const llvm::Function *root) {
    if (index.count(root))
      return;
    worklist.push({root, {}, {}});
    while (!worklist.empty()) {
      Frame &frame = worklist.top();
      const llvm::Function *f = frame.f;
      if (!index.count(f)) {
        index[f] = lowlink[f] = nextIndex++;
        stk.push(f);
        onStack.insert(f);
        auto cgIt = callGraph.find(f);
        if (cgIt != callGraph.end()) {
          frame.it = cgIt->second.begin();
          frame.end = cgIt->second.end();
        } else {
          frame.it = frame.end = {};
        }
      }
      bool pushed = false;
      while (frame.it != frame.end) {
        const llvm::Function *callee = *frame.it;
        ++frame.it;
        if (!index.count(callee)) {
          worklist.push({callee, {}, {}});
          pushed = true;
          break;
        } else if (onStack.count(callee)) {
          lowlink[f] = std::min(lowlink[f], index[callee]);
        }
      }
      if (pushed)
        continue;
      worklist.pop();
      if (!worklist.empty()) {
        const llvm::Function *parent = worklist.top().f;
        lowlink[parent] = std::min(lowlink[parent], lowlink[f]);
      }
      if (lowlink[f] == index[f]) {
        std::set<const llvm::Function *> scc;
        const llvm::Function *w;
        do {
          w = stk.top();
          stk.pop();
          onStack.erase(w);
          scc.insert(w);
        } while (w != f);
        sccs.push_back(std::move(scc));
      }
    }
  };

  // Visit all functions
  for (const llvm::Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    visit(&F);
  }

  // Identify recursive SCCs: multi-function SCCs + self-recursive functions
  std::unordered_set<size_t> recursiveSccIds;
  for (size_t i = 0; i < sccs.size(); ++i) {
    if (sccs[i].size() > 1) {
      // Multi-function SCC (mutual recursion)
      recursiveSccIds.insert(i);
    } else if (sccs[i].size() == 1) {
      // Check for self-recursion (self-edge in call graph)
      const llvm::Function *f = *sccs[i].begin();
      auto it = callGraph.find(f);
      if (it != callGraph.end()) {
        for (const llvm::Function *callee : it->second) {
          if (callee == f) {
            recursiveSccIds.insert(i);
            break;
          }
        }
      }
    }
  }

  // Populate recursiveFuns_ set
  for (size_t i = 0; i < sccs.size(); ++i) {
    if (recursiveSccIds.count(i)) {
      uint32_t rid = static_cast<uint32_t>(i);
      auto &members = recursiveSccMembers_[rid];
      for (const llvm::Function *f : sccs[i]) {
        recursiveFuns_.insert(f);
        recursiveSccIdMap_[f] = rid;
        members.push_back(f);
      }
    }
  }

  // Track entry calls (calls from outside SCC to inside SCC) - these are NOT
  // recursive callsites. This matches SVF's nonRecursiveCallSites tracking.
  // We identify entry calls by finding call sites where:
  // 1. The caller is outside the SCC
  // 2. The callee is inside the SCC
  for (size_t i = 0; i < sccs.size(); ++i) {
    if (!recursiveSccIds.count(i))
      continue; // Only process recursive SCCs

    const std::set<const llvm::Function *> &scc = sccs[i];

    // Find all call sites that call into this SCC from outside
    for (const llvm::Function &F : *module_) {
      if (F.isDeclaration())
        continue;

      // Check if this function is outside the SCC
      bool callerInSCC = scc.count(&F) > 0;
      if (callerInSCC)
        continue; // Skip if caller is already in the SCC

      for (const llvm::BasicBlock &BB : F) {
        for (const llvm::Instruction &I : BB) {
          const llvm::CallBase *cb = llvm::dyn_cast<llvm::CallBase>(&I);
          if (!cb)
            continue;

          // For direct calls, check if callee is in the SCC
          if (const llvm::Function *direct = cb->getCalledFunction()) {
            if (!direct->isDeclaration() && scc.count(direct) > 0) {
              // This is an entry call from outside SCC to inside SCC
              // Mark it as non-recursive call site (entry point)
              nonRecursiveCallSites_.insert({cb, direct});
            }
          }
          // For indirect calls, resolve via AserPTA and check if targets are in
          // SCC
          else {
            for (const llvm::Function *indirectCallee : getCallees(cb)) {
              if (indirectCallee && !indirectCallee->isDeclaration() &&
                  scc.count(indirectCallee) > 0) {
                nonRecursiveCallSites_.insert({cb, indirectCallee});
              }
            }
          }
        }
      }
    }
  }

  // Track recursive call sites (for statistics/debugging)
  for (const llvm::Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    for (const llvm::BasicBlock &BB : F) {
      for (const llvm::Instruction &I : BB) {
        const llvm::CallBase *cb = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!cb)
          continue;
        if (const llvm::Function *callee = cb->getCalledFunction()) {
          if (recursiveFuns_.count(&F) && recursiveFuns_.count(callee) &&
              &F != callee) {
            // This is a recursive call site (within same SCC)
            callSiteRecursionDepth_[cb] = 0; // Initialize depth
          } else if (recursiveFuns_.count(callee) && &F == callee) {
            // Self-recursive call
            callSiteRecursionDepth_[cb] = 0;
          }
        }
      }
    }
  }
}

void AbstractInterpretation::initWTO() {
  // Build WTO for each function in the module
  for (auto &func : *module_) {
    if (func.isDeclaration())
      continue;

    // Build WTO for this function
    ICFGWTO *wto = new ICFGWTO(&func);
    funcToWTO_[&func] = wto;

    // Collect cycle heads for efficient lookup
    collectCycleHeads(wto->getComponents(), &func);
  }
}

void AbstractInterpretation::collectCycleHeads(
    const std::list<const ICFGWTOComp *> &comps, const llvm::Function *func) {
  for (const ICFGWTOComp *comp : comps) {
    if (const ICFGCycleWTO *cycle = llvm::dyn_cast<ICFGCycleWTO>(comp)) {
      // Map cycle head (entry) to the cycle
      cycleHeadToCycle_[cycle->getEntry()] = cycle;
      // Recursively collect nested cycle heads
      collectCycleHeads(cycle->getComponents(), func);
    }
  }
}

void AbstractInterpretation::handleFunction(const llvm::Function *func) {
  if (!func || func->isDeclaration())
    return;
  analyzedFunctions_.insert(func);

  // Get WTO for this function
  auto it = funcToWTO_.find(func);
  if (it == funcToWTO_.end()) {
    // No WTO available - fall back to simple worklist
    std::set<const llvm::BasicBlock *> visited;
    std::set<const llvm::BasicBlock *> inProgress;
    std::vector<const llvm::BasicBlock *> worklist;
    worklist.push_back(&func->getEntryBlock());

    while (!worklist.empty()) {
      const llvm::BasicBlock *bb = worklist.back();
      worklist.pop_back();

      if (visited.count(bb))
        continue;
      if (inProgress.count(bb)) {
        continue;
      }

      inProgress.insert(bb);
      visited.insert(bb);

      if (!mergeStatesFromPredecessors(bb))
        continue;

      for (auto &inst : *bb) {
        if (!handleInstruction(&inst)) {
          return;
        }
      }

      const llvm::Instruction *term = bb->getTerminator();
      for (unsigned i = 0; i < term->getNumSuccessors(); ++i) {
        worklist.push_back(term->getSuccessor(i));
      }

      inProgress.erase(bb);
    }
    return;
  }

  // Use WTO-based traversal
  ICFGWTO *wto = it->second;
  const std::list<const ICFGWTOComp *> &components = wto->getComponents();

  // Process WTO components in order
  for (const ICFGWTOComp *comp : components) {
    if (const ICFGSingletonWTO *singleton =
            llvm::dyn_cast<ICFGSingletonWTO>(comp)) {
      handleSingletonWTO(singleton->getBlock());
    } else if (const ICFGCycleWTO *cycle = llvm::dyn_cast<ICFGCycleWTO>(comp)) {
      handleCycleWTO(cycle);
    }
  }
}

void AbstractInterpretation::handleSingletonWTO(const llvm::BasicBlock *bb) {
  stat->getBlockTrace()++;

  // Merge states from predecessors
  if (!mergeStatesFromPredecessors(bb))
    return;

  // Process all instructions in the block
  for (auto &inst : *bb) {
    if (!handleInstruction(&inst)) {
      return;
    }
  }

  stat->countStateSize();
}

/// Handle WTO cycle (loop or recursive function) using widening/narrowing
/// iteration.
///
/// Widening is applied at the cycle head to ensure termination of the analysis.
/// The cycle head's abstract state is iteratively updated until a fixpoint is
/// reached.
///
/// == What is being widened ==
/// The abstract state at the cycle head node, which includes:
/// - Variable values (intervals) that may change across loop iterations
/// - For example, a loop counter `i` starting at 0 and incrementing each
/// iteration
///
/// == Regular loops (non-recursive functions) ==
/// All modes (TOP/WIDEN_ONLY/WIDEN_NARROW) behave the same for regular loops:
/// 1. Widening phase: Iterate until the cycle head state stabilizes
///    Example: for(i=0; i<100; i++) -> i widens to [0, +inf]
/// 2. Narrowing phase: Refine the over-approximation from widening
///    Example: [0, +inf] narrows to [0, 100] using loop condition
///
/// == Recursive function cycles ==
/// Behavior depends on recursionMode_:
///
/// - TOP mode:
///     Does not iterate. Calls recursiveCallPass() to set all stores and
///     return value to TOP immediately. This is the most conservative but
///     fastest. Example:
///       int factorial(int n) { return n <= 1 ? 1 : n * factorial(n-1); }
///       factorial(5) -> returns [-inf, +inf]
///
/// - WIDEN_ONLY mode:
///     Widening phase only, no narrowing for recursive functions.
///     The recursive function body is analyzed with widening until fixpoint.
///     Example:
///       int factorial(int n) { return n <= 1 ? 1 : n * factorial(n-1); }
///       factorial(5) -> returns [10000, +inf] (widened upper bound)
///
/// - WIDEN_NARROW mode:
///     Both widening and narrowing phases for recursive functions.
///     After widening reaches fixpoint, narrowing refines the result.
///     Example:
///       int factorial(int n) { return n <= 1 ? 1 : n * factorial(n-1); }
///       factorial(5) -> returns [10000, 10000] (precise after narrowing)

void AbstractInterpretation::handleCycleWTO(const ICFGCycleWTO *cycle) {
  const llvm::BasicBlock *cycleHead = cycle->getEntry();
  const llvm::Function *func = cycleHead->getParent();

  // TOP mode for recursive function cycles: use recursiveCallPass to set
  // all stores and return value to TOP, maintaining original semantics
  if (recursionMode_ == TOP && isRecursiveFun(func)) {
    // Find the call site that entered this function
    if (!callSiteStack.empty()) {
      const llvm::CallBase *callNode = callSiteStack.back();
      bool entersThisFunction = false;
      for (const llvm::Function *resolved : getCallees(callNode)) {
        if (resolved == func) {
          entersThisFunction = true;
          break;
        }
      }
      if (entersThisFunction) {
        if (isRecursiveCallSite(callNode, func)) {
          recursiveCallPass(callNode, func);
          return;
        }

        // Entry calls into a recursive SCC should still execute the function
        // body once. Only the inner recursive callsites are summarized to TOP.
        handleSingletonWTO(cycleHead);
        for (const ICFGWTOComp *comp : cycle->getComponents()) {
          if (const ICFGSingletonWTO *singleton =
                  llvm::dyn_cast<ICFGSingletonWTO>(comp)) {
            handleSingletonWTO(singleton->getBlock());
          } else if (const ICFGCycleWTO *subCycle =
                         llvm::dyn_cast<ICFGCycleWTO>(comp)) {
            handleCycleWTO(subCycle);
          }
        }
        return;
      }
    }
  }

  // WIDEN_ONLY / WIDEN_NARROW modes: iterate until fixpoint
  bool increasing = true;
  const uint32_t widenDelay = widenDelay_;

  for (uint32_t curIter = 0;; curIter++) {
    // Get the abstract state before processing the cycle head
    AbstractState prevHeadState;
    bool hadPrevState = false;

    // Try to get state from first instruction of cycle head
    if (!cycleHead->empty()) {
      const llvm::Instruction *firstInst = &cycleHead->front();
      if (abstractTrace.find(firstInst) != abstractTrace.end()) {
        prevHeadState = abstractTrace[firstInst];
        hadPrevState = true;
      }
    }

    // Process the cycle head node
    handleSingletonWTO(cycleHead);

    // Get current state after processing
    AbstractState curHeadState;
    if (!cycleHead->empty()) {
      const llvm::Instruction *firstInst = &cycleHead->front();
      if (abstractTrace.find(firstInst) != abstractTrace.end()) {
        curHeadState = abstractTrace[firstInst];
      }
    }

    // Match SVF: apply widening/narrowing at cycle head before processing body.
    if (curIter >= widenDelay) {
      if (increasing) {
        // Apply widening operator
        AbstractState widenedState = prevHeadState.widening(curHeadState);

        // Update state at cycle head
        if (!cycleHead->empty()) {
          const llvm::Instruction *firstInst = &cycleHead->front();
          abstractTrace[firstInst] = widenedState;
          curHeadState = widenedState; // Update for comparison
        }

        // Check if widening fixpoint reached
        if (widenedState == prevHeadState) {
          // Widening fixpoint reached, switch to narrowing phase
          increasing = false;
        }
      } else {
        // Narrowing phase - check if narrowing should be applied
        const llvm::Function *func = cycleHead->getParent();
        if (!shouldApplyNarrowing(func)) {
          break;
        }

        // Apply narrowing
        AbstractState narrowedState = prevHeadState.narrowing(curHeadState);

        // Update state at cycle head
        if (!cycleHead->empty()) {
          const llvm::Instruction *firstInst = &cycleHead->front();
          abstractTrace[firstInst] = narrowedState;
          curHeadState = narrowedState; // Update for comparison
        }

        // Check if narrowing fixpoint reached
        if (narrowedState == prevHeadState) {
          // Narrowing fixpoint reached, exit loop
          break;
        }
      }
    }

    // Process cycle body components
    for (const ICFGWTOComp *comp : cycle->getComponents()) {
      if (const ICFGSingletonWTO *singleton =
              llvm::dyn_cast<ICFGSingletonWTO>(comp)) {
        handleSingletonWTO(singleton->getBlock());
      } else if (const ICFGCycleWTO *subCycle =
                     llvm::dyn_cast<ICFGCycleWTO>(comp)) {
        // Handle nested cycle recursively
        handleCycleWTO(subCycle);
      }
    }
  }
}

bool AbstractInterpretation::mergeStatesFromPredecessors(
    const llvm::BasicBlock *bb) {
  std::vector<AbstractState> workList;
  AbstractState preAs;

  for (auto predIt = llvm::pred_begin(bb), predEnd = llvm::pred_end(bb);
       predIt != predEnd; ++predIt) {
    const llvm::BasicBlock *pred = *predIt;

    // Get state from predecessor block (stored at terminator or block)
    AbstractState tmpEs;
    bool foundState = false;

    // Try to get state from terminator instruction
    const llvm::Instruction *term = pred->getTerminator();
    auto it = abstractTrace.find(term);
    if (it != abstractTrace.end()) {
      tmpEs = it->second;
      foundState = true;
    } else {
      // Try to get state from first instruction of block
      if (!pred->empty()) {
        auto it2 = abstractTrace.find(&pred->front());
        if (it2 != abstractTrace.end()) {
          tmpEs = it2->second;
          foundState = true;
        }
      }
    }

    if (foundState) {
      // Check if this is a conditional branch and if the edge is feasible
      if (const llvm::BranchInst *branch =
              llvm::dyn_cast<llvm::BranchInst>(term)) {
        if (branch->isConditional()) {
          // Determine which successor this edge leads to
          bool isTrueEdge = branch->getSuccessor(0) == bb;
          if (isBranchFeasible(branch, tmpEs, isTrueEdge)) {
            workList.push_back(tmpEs);
          }
        } else {
          workList.push_back(tmpEs);
        }
      } else if (const llvm::SwitchInst *switchInst =
                     llvm::dyn_cast<llvm::SwitchInst>(term)) {
        // Handle switch statement - check if this successor corresponds to a
        // feasible case
        bool isFeasible = false;

        // Check if this is the default case
        if (bb == switchInst->getDefaultDest()) {
          // Default edge is feasible unless we can prove the condition equals a
          // specific case value (singleton interval exactly on a case).
          uint32_t condId = getValueId(switchInst->getCondition());
          if (!tmpEs.inVarToValTable(condId)) {
            isFeasible = true;
          } else {
            const IntervalValue &condVal = tmpEs[condId].getInterval();
            if (condVal.isBottom()) {
              isFeasible = false;
            } else if (!condVal.is_numeral()) {
              isFeasible = true;
            } else {
              const int64_t exact = condVal.getIntNumeralOrZero();
              bool matchesCase = false;
              for (auto caseIt = switchInst->case_begin();
                   caseIt != switchInst->case_end(); ++caseIt) {
                const llvm::ConstantInt *caseVal = caseIt->getCaseValue();
                if (caseVal->getSExtValue() == exact) {
                  matchesCase = true;
                  break;
                }
              }
              isFeasible = !matchesCase;
            }
          }
        } else {
          // Check if this block corresponds to a case value
          for (auto caseIt = switchInst->case_begin();
               caseIt != switchInst->case_end(); ++caseIt) {
            if (caseIt->getCaseSuccessor() == bb) {
              const llvm::ConstantInt *caseVal = caseIt->getCaseValue();
              int64_t caseValue = caseVal->getSExtValue();
              if (isSwitchBranchFeasible(switchInst, caseValue, tmpEs)) {
                isFeasible = true;
                break;
              }
            }
          }
        }

        if (isFeasible) {
          workList.push_back(tmpEs);
        }
      } else {
        // Other terminator types (unconditional branches, returns, etc.)
        workList.push_back(tmpEs);
      }
    }
  }

  if (workList.empty()) {
    // Entry blocks have no predecessors; seed them with global state.
    if (bb == &bb->getParent()->getEntryBlock()) {
      if (!bb->empty()) {
        const llvm::Instruction *firstInst = &bb->front();
        auto existing = abstractTrace.find(firstInst);
        if (existing != abstractTrace.end()) {
          // Preserve callsite-seeded entry state (args/params) and merge
          // globals.
          AbstractState merged = globalState;
          merged.joinWith(existing->second);
          abstractTrace[firstInst] = merged;
        } else {
          abstractTrace[firstInst] = globalState;
        }
      }
      return true;
    }

    // No predecessors with state - initialize empty state
    if (!bb->empty()) {
      abstractTrace[&bb->front()] = AbstractState();
    }
    return false;
  }

  // Join all feasible predecessor states
  preAs = workList[0];
  for (size_t i = 1; i < workList.size(); ++i) {
    preAs.joinWith(workList[i]);
  }

  // Store state at first instruction of block
  if (!bb->empty()) {
    abstractTrace[&bb->front()] = preAs;
  }
  return true;
}

bool AbstractInterpretation::handleInstruction(const llvm::Instruction *inst) {
  stat->stmtCount++;
  this->currentInstruction_ = inst;

  if (abstractTrace.find(inst) == abstractTrace.end()) {
    const llvm::BasicBlock *bb = inst->getParent();
    // First try: inherit from previous instruction in same block (intra-block
    // flow). This is critical for entry blocks and blocks with no CFG preds.
    const llvm::Instruction *prevInst = nullptr;
    for (const llvm::Instruction &I : *bb) {
      if (&I == inst)
        break;
      prevInst = &I;
    }
    if (prevInst) {
      auto prevIt = abstractTrace.find(prevInst);
      if (prevIt != abstractTrace.end()) {
        abstractTrace[inst] = prevIt->second;
      }
    }
    // Fallback: inherit from CFG predecessors
    if (abstractTrace.find(inst) == abstractTrace.end()) {
      auto predIt = llvm::pred_begin(bb);
      if (predIt != llvm::pred_end(bb)) {
        const llvm::Instruction *predTerm = (*predIt)->getTerminator();
        if (abstractTrace.find(predTerm) != abstractTrace.end()) {
          abstractTrace[inst] = abstractTrace[predTerm];
        } else {
          abstractTrace[inst] = AbstractState();
        }
      } else {
        abstractTrace[inst] = AbstractState();
      }
    }
  }

  AbstractState &as = abstractTrace[inst];

  // Initialize null pointer constant in abstract state if not already present
  // NullPtr is 0, which is the ID assigned to ConstantPointerNull values
  if (as._varToAbsVal.find(0) == as._varToAbsVal.end()) {
    // Null pointer constant (ID 0) should be represented as NullMemAddr
    // (0x7f000000)
    as[0] = AbstractValue(AddressValue(NullMemAddr));
  }

  switch (inst->getOpcode()) {
  case llvm::Instruction::Alloca:
    updateStateOnAddr(llvm::cast<llvm::AllocaInst>(inst));
    break;
  case llvm::Instruction::Add:
  case llvm::Instruction::Sub:
  case llvm::Instruction::Mul:
  case llvm::Instruction::SDiv:
  case llvm::Instruction::UDiv:
  case llvm::Instruction::SRem:
  case llvm::Instruction::URem:
  case llvm::Instruction::Shl:
  case llvm::Instruction::LShr:
  case llvm::Instruction::AShr:
  case llvm::Instruction::And:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
    updateStateOnBinary(llvm::cast<llvm::BinaryOperator>(inst));
    break;
  case llvm::Instruction::ICmp:
  case llvm::Instruction::FCmp:
    updateStateOnCmp(llvm::cast<llvm::CmpInst>(inst));
    break;
  case llvm::Instruction::Load:
    updateStateOnLoad(llvm::cast<llvm::LoadInst>(inst));
    break;
  case llvm::Instruction::Store:
    updateStateOnStore(llvm::cast<llvm::StoreInst>(inst));
    break;
  case llvm::Instruction::GetElementPtr:
    updateStateOnGep(llvm::cast<llvm::GetElementPtrInst>(inst));
    break;
  case llvm::Instruction::Call:
  case llvm::Instruction::Invoke:
    updateStateOnCall(llvm::cast<llvm::CallBase>(inst));
    break;
  case llvm::Instruction::Select:
    updateStateOnSelect(llvm::cast<llvm::SelectInst>(inst));
    break;
  case llvm::Instruction::PHI:
    updateStateOnPhi(llvm::cast<llvm::PHINode>(inst));
    break;
  // Aggregate operations (equivalent to SVF's CopyStmt)
  case llvm::Instruction::ExtractValue:
    updateStateOnExtractValue(llvm::cast<llvm::ExtractValueInst>(inst));
    break;
  case llvm::Instruction::InsertValue:
    updateStateOnInsertValue(llvm::cast<llvm::InsertValueInst>(inst));
    break;
  case llvm::Instruction::ExtractElement:
    updateStateOnExtractElement(llvm::cast<llvm::ExtractElementInst>(inst));
    break;
  case llvm::Instruction::InsertElement:
    updateStateOnInsertElement(llvm::cast<llvm::InsertElementInst>(inst));
    break;
  case llvm::Instruction::ShuffleVector:
    updateStateOnShuffleVector(llvm::cast<llvm::ShuffleVectorInst>(inst));
    break;
  case llvm::Instruction::Ret:
    updateStateOnRet(llvm::cast<llvm::ReturnInst>(inst));
    break;
  case llvm::Instruction::Trunc:
  case llvm::Instruction::ZExt:
  case llvm::Instruction::SExt:
  case llvm::Instruction::FPToUI:
  case llvm::Instruction::FPToSI:
  case llvm::Instruction::UIToFP:
  case llvm::Instruction::SIToFP:
  case llvm::Instruction::FPTrunc:
  case llvm::Instruction::FPExt:
  case llvm::Instruction::PtrToInt:
  case llvm::Instruction::IntToPtr:
  case llvm::Instruction::BitCast:
    updateStateOnCast(llvm::cast<llvm::CastInst>(inst));
    break;
  default:
    break;
  }

  for (auto &detector : detectors)
    detector->detect(as, inst);

  as.commitPendingFrees();

  this->currentInstruction_ = nullptr;

  return true;
}

void AbstractInterpretation::updateStateOnAddr(const llvm::AllocaInst *addr) {
  uint32_t addrId = getValueId(addr);
  // Use improved version that tracks VLA sizes from abstract state
  // Note: We use the state at the alloca instruction itself
  uint32_t objSize =
      abstractTrace[addr].getAllocaInstByteSize(addr, abstractTrace[addr]);

  AbstractValue av(AddressValue(AddressValue::getVirtualMemAddress(addrId)));
  abstractTrace[addr][addrId] = av;
  abstractTrace[addr].setObjSize(addrId, objSize);
}

void AbstractInterpretation::updateStateOnBinary(
    const llvm::BinaryOperator *binary) {
  uint32_t lhsId = getValueId(binary);
  uint32_t op0Id = getValueId(binary->getOperand(0));
  uint32_t op1Id = getValueId(binary->getOperand(1));

  AbstractState &as = abstractTrace[binary];

  materializeConstantValue(as, binary->getOperand(0), op0Id);
  materializeConstantValue(as, binary->getOperand(1), op1Id);

  // Initialize operands if not present
  if (!as.inVarToValTable(op0Id))
    as[op0Id] = AbstractValue(IntervalValue::top());
  if (!as.inVarToValTable(op1Id))
    as[op1Id] = AbstractValue(IntervalValue::top());

  IntervalValue val0 = as[op0Id].getInterval();
  IntervalValue val1 = as[op1Id].getInterval();
  IntervalValue result;

  switch (binary->getOpcode()) {
  case llvm::Instruction::Add:
  case llvm::Instruction::FAdd:
    result = val0 + val1;
    break;
  case llvm::Instruction::Sub:
  case llvm::Instruction::FSub:
    result = val0 - val1;
    break;
  case llvm::Instruction::Mul:
  case llvm::Instruction::FMul:
    result = val0 * val1;
    break;
  case llvm::Instruction::SDiv:
  case llvm::Instruction::UDiv:
  case llvm::Instruction::FDiv:
    if (val1.contains(0)) {
      // Division by zero - result is top
      result = IntervalValue::top();
    } else {
      result = val0 / val1;
    }
    break;
  case llvm::Instruction::SRem:
  case llvm::Instruction::URem:
  case llvm::Instruction::FRem:
    if (val1.contains(0)) {
      // Modulo by zero - result is top
      result = IntervalValue::top();
    } else {
      result = val0 % val1;
    }
    break;
  case llvm::Instruction::Shl:
    result = val0 << val1;
    break;
  case llvm::Instruction::LShr:
  case llvm::Instruction::AShr:
    result = val0 >> val1;
    break;
  case llvm::Instruction::And:
    result = val0 & val1;
    break;
  case llvm::Instruction::Or:
    result = val0 | val1;
    break;
  case llvm::Instruction::Xor:
    result = val0 ^ val1;
    break;
  default:
    result = IntervalValue::top();
    break;
  }

  as[lhsId] = AbstractValue(result);
}

void AbstractInterpretation::updateStateOnCmp(const llvm::CmpInst *cmp) {
  uint32_t resId = getValueId(cmp);
  uint32_t op0Id = getValueId(cmp->getOperand(0));
  uint32_t op1Id = getValueId(cmp->getOperand(1));

  AbstractState &as = abstractTrace[cmp];

  materializeConstantValue(as, cmp->getOperand(0), op0Id);
  materializeConstantValue(as, cmp->getOperand(1), op1Id);

  // Handle address comparisons
  if (as.inVarToAddrsTable(op0Id) && as.inVarToAddrsTable(op1Id)) {
    AddressValue addrOp0 = as[op0Id].getAddrs();
    AddressValue addrOp1 = as[op1Id].getAddrs();
    IntervalValue resVal;

    switch (cmp->getPredicate()) {
    case llvm::CmpInst::ICMP_EQ:
      if (addrOp0.equals(addrOp1)) {
        resVal =
            IntervalValue(static_cast<int64_t>(1), static_cast<int64_t>(1));
      } else if (addrOp0.hasIntersect(addrOp1)) {
        resVal =
            IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(1));
      } else {
        resVal =
            IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(0));
      }
      break;
    case llvm::CmpInst::ICMP_NE:
      if (addrOp0.equals(addrOp1)) {
        resVal =
            IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(0));
      } else if (addrOp0.hasIntersect(addrOp1)) {
        resVal =
            IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(1));
      } else {
        resVal =
            IntervalValue(static_cast<int64_t>(1), static_cast<int64_t>(1));
      }
      break;
    default:
      resVal = IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(1));
      break;
    }
    as[resId] = AbstractValue(resVal);
    return;
  }

  // Handle null pointer comparisons
  if (op0Id == AbstractState::NullPtr || op1Id == AbstractState::NullPtr) {
    IntervalValue resVal = (as[op0Id].equals(as[op1Id])) ? IntervalValue(1, 1)
                                                         : IntervalValue(0, 0);
    as[resId] = AbstractValue(resVal);
    return;
  }

  // Handle interval comparisons
  if (!as.inVarToValTable(op0Id))
    as[op0Id] = AbstractValue(IntervalValue::top());
  if (!as.inVarToValTable(op1Id))
    as[op1Id] = AbstractValue(IntervalValue::top());

  IntervalValue val0 = as[op0Id].getInterval();
  IntervalValue val1 = as[op1Id].getInterval();
  IntervalValue resVal;

  // getIntNumeral() asserts on infinity; use conservative [0,1] when bounds
  // are unbounded
  if (val0.is_infinite() || val1.is_infinite()) {
    as[resId] = AbstractValue(IntervalValue(0, 1));
    return;
  }

  switch (cmp->getPredicate()) {
  case llvm::CmpInst::ICMP_EQ:
  case llvm::CmpInst::FCMP_OEQ:
  case llvm::CmpInst::FCMP_UEQ:
    if (val0.is_numeral() && val1.is_numeral() && val0.equals(val1)) {
      resVal = IntervalValue(1, 1);
    } else if (!val0.hasIntersect(val1)) {
      resVal = IntervalValue(0, 0);
    } else {
      resVal = IntervalValue(0, 1);
    }
    break;
  case llvm::CmpInst::ICMP_NE:
  case llvm::CmpInst::FCMP_ONE:
  case llvm::CmpInst::FCMP_UNE:
    if (val0.is_numeral() && val1.is_numeral() && !val0.equals(val1)) {
      resVal = IntervalValue(1, 1);
    } else if (!val0.hasIntersect(val1)) {
      resVal = IntervalValue(1, 1);
    } else {
      resVal = IntervalValue(0, 1);
    }
    break;
  case llvm::CmpInst::ICMP_SLT:
  case llvm::CmpInst::ICMP_ULT:
  case llvm::CmpInst::FCMP_OLT:
  case llvm::CmpInst::FCMP_ULT:
    if (val0.ub().getIntNumeral() < val1.lb().getIntNumeral()) {
      resVal = IntervalValue(1, 1);
    } else if (val0.lb().getIntNumeral() >= val1.ub().getIntNumeral()) {
      resVal = IntervalValue(0, 0);
    } else {
      resVal = IntervalValue(0, 1);
    }
    break;
  case llvm::CmpInst::ICMP_SLE:
  case llvm::CmpInst::ICMP_ULE:
  case llvm::CmpInst::FCMP_OLE:
  case llvm::CmpInst::FCMP_ULE:
    if (val0.ub().getIntNumeral() <= val1.lb().getIntNumeral()) {
      resVal = IntervalValue(1, 1);
    } else if (val0.lb().getIntNumeral() > val1.ub().getIntNumeral()) {
      resVal = IntervalValue(0, 0);
    } else {
      resVal = IntervalValue(0, 1);
    }
    break;
  case llvm::CmpInst::ICMP_SGT:
  case llvm::CmpInst::ICMP_UGT:
  case llvm::CmpInst::FCMP_OGT:
  case llvm::CmpInst::FCMP_UGT:
    if (val0.lb().getIntNumeral() > val1.ub().getIntNumeral()) {
      resVal = IntervalValue(1, 1);
    } else if (val0.ub().getIntNumeral() <= val1.lb().getIntNumeral()) {
      resVal = IntervalValue(0, 0);
    } else {
      resVal = IntervalValue(0, 1);
    }
    break;
  case llvm::CmpInst::ICMP_SGE:
  case llvm::CmpInst::ICMP_UGE:
  case llvm::CmpInst::FCMP_OGE:
  case llvm::CmpInst::FCMP_UGE:
    if (val0.lb().getIntNumeral() >= val1.ub().getIntNumeral()) {
      resVal = IntervalValue(1, 1);
    } else if (val0.ub().getIntNumeral() < val1.lb().getIntNumeral()) {
      resVal = IntervalValue(0, 0);
    } else {
      resVal = IntervalValue(0, 1);
    }
    break;
  case llvm::CmpInst::FCMP_FALSE:
    resVal = IntervalValue(0, 0);
    break;
  case llvm::CmpInst::FCMP_TRUE:
    resVal = IntervalValue(1, 1);
    break;
  default:
    resVal = IntervalValue(0, 1);
    break;
  }

  as[resId] = AbstractValue(resVal);
}

void AbstractInterpretation::updateStateOnLoad(const llvm::LoadInst *load) {
  uint32_t lhsId = getValueId(load);
  uint32_t ptrId = getValueId(load->getPointerOperand());

  AbstractState &as = abstractTrace[load];
  AbstractValue loadedVal = as.loadValue(ptrId);

  as[lhsId] = loadedVal;
}

void AbstractInterpretation::updateStateOnStore(const llvm::StoreInst *store) {
  uint32_t ptrId = getValueId(store->getPointerOperand());
  uint32_t valId = getValueId(store->getValueOperand());

  AbstractState &as = abstractTrace[store];
  materializeConstantValue(as, store->getValueOperand(), valId);
  as.storeValue(ptrId, valId);
}

void AbstractInterpretation::updateStateOnCast(const llvm::CastInst *cast) {
  uint32_t lhsId = getValueId(cast);
  uint32_t opId = getValueId(cast->getOperand(0));
  AbstractState &as = abstractTrace[cast];

  if (as.inVarToAddrsTable(opId)) {
    // Pointer cast - preserve address
    as[lhsId] = as[opId];
    return;
  }

  if (!as.inVarToValTable(opId)) {
    as[lhsId] = AbstractValue(IntervalValue::top());
    return;
  }

  IntervalValue srcVal = as[opId].getInterval();
  if (srcVal.isBottom()) {
    as[lhsId] = AbstractValue(IntervalValue::bottom());
    return;
  }

  llvm::Type *srcType = cast->getOperand(0)->getType();
  llvm::Type *dstType = cast->getDestTy();

  // Helper function for zero extension (matches SVF's getZExtValue)
  auto getZExtValue = [&](const IntervalValue &val,
                          llvm::Type *srcType) -> IntervalValue {
    if (!srcType->isIntegerTy())
      return IntervalValue::top();

    if (val.is_infinite())
      return IntervalValue::top();

    unsigned bits = srcType->getIntegerBitWidth();
    if (val.is_numeral()) {
      int64_t numVal = val.getIntNumeral();
      if (bits == 8) {
        int8_t signed_i8_value = static_cast<int8_t>(numVal);
        uint32_t unsigned_value = static_cast<uint8_t>(signed_i8_value);
        return IntervalValue(unsigned_value, unsigned_value);
      } else if (bits == 16) {
        int16_t signed_i16_value = static_cast<int16_t>(numVal);
        uint32_t unsigned_value = static_cast<uint16_t>(signed_i16_value);
        return IntervalValue(unsigned_value, unsigned_value);
      } else if (bits == 32) {
        int32_t signed_i32_value = static_cast<int32_t>(numVal);
        uint32_t unsigned_value = static_cast<uint32_t>(signed_i32_value);
        return IntervalValue(unsigned_value, unsigned_value);
      } else if (bits == 64) {
        int64_t signed_i64_value = static_cast<int64_t>(numVal);
        return IntervalValue(signed_i64_value, signed_i64_value);
      }
    }
    return IntervalValue::top();
  };

  // Helper function for truncation (matches SVF's getTruncValue)
  auto getTruncValue = [&](const IntervalValue &val,
                           llvm::Type *dstType) -> IntervalValue {
    if (val.isBottom())
      return val;

    if (val.is_infinite() || !dstType->isIntegerTy())
      return IntervalValue::top();

    int64_t int_lb = val.lb().getIntNumeral();
    int64_t int_ub = val.ub().getIntNumeral();
    unsigned dst_bits = dstType->getIntegerBitWidth();

    if (dst_bits == 8) {
      int8_t s8_lb = static_cast<int8_t>(int_lb);
      int8_t s8_ub = static_cast<int8_t>(int_ub);
      if (s8_lb > s8_ub) {
        // Overflow detected - return range limit for type
        if (utils) {
          return utils->getRangeLimitFromType(dstType);
        }
        return IntervalValue(std::numeric_limits<int8_t>::min(),
                             std::numeric_limits<int8_t>::max());
      }
      return IntervalValue(s8_lb, s8_ub);
    } else if (dst_bits == 16) {
      int16_t s16_lb = static_cast<int16_t>(int_lb);
      int16_t s16_ub = static_cast<int16_t>(int_ub);
      if (s16_lb > s16_ub) {
        // Overflow detected - return range limit for type
        if (utils) {
          return utils->getRangeLimitFromType(dstType);
        }
        return IntervalValue(std::numeric_limits<int16_t>::min(),
                             std::numeric_limits<int16_t>::max());
      }
      return IntervalValue(s16_lb, s16_ub);
    } else if (dst_bits == 32) {
      int32_t s32_lb = static_cast<int32_t>(int_lb);
      int32_t s32_ub = static_cast<int32_t>(int_ub);
      if (s32_lb > s32_ub) {
        // Overflow detected - return range limit for type
        if (utils) {
          return utils->getRangeLimitFromType(dstType);
        }
        return IntervalValue(std::numeric_limits<int32_t>::min(),
                             std::numeric_limits<int32_t>::max());
      }
      return IntervalValue(s32_lb, s32_ub);
    }
    return IntervalValue::top();
  };

  // Handle integer casts
  if (srcType->isIntegerTy() && dstType->isIntegerTy()) {
    if (cast->getOpcode() == llvm::Instruction::Trunc) {
      // Truncate: reduce precision with overflow detection
      as[lhsId] = AbstractValue(getTruncValue(srcVal, dstType));
    } else if (cast->getOpcode() == llvm::Instruction::ZExt) {
      // Zero extend: convert signed to unsigned properly
      as[lhsId] = AbstractValue(getZExtValue(srcVal, srcType));
    } else if (cast->getOpcode() == llvm::Instruction::SExt) {
      // Sign extend: preserve value, extend sign bit
      as[lhsId] = AbstractValue(srcVal);
    } else {
      as[lhsId] = AbstractValue(srcVal);
    }
  } else if (cast->getOpcode() == llvm::Instruction::PtrToInt) {
    // Pointer to integer: lose precision
    as[lhsId] = AbstractValue(IntervalValue::top());
  } else if (cast->getOpcode() == llvm::Instruction::IntToPtr) {
    // Integer to pointer: create address (insert nullptr)
    // Similar to SVF: no explicit assignment, address will be set elsewhere
    as[lhsId] = AbstractValue(AddressValue());
  } else if (cast->getOpcode() == llvm::Instruction::BitCast) {
    // Bitcast: preserve value
    if (as.inVarToAddrsTable(opId)) {
      as[lhsId] = as[opId];
    } else {
      // Do nothing for non-address bitcasts (matches SVF behavior)
      as[lhsId] = AbstractValue(srcVal);
    }
  } else if (cast->getOpcode() == llvm::Instruction::FPToSI ||
             cast->getOpcode() == llvm::Instruction::FPToUI ||
             cast->getOpcode() == llvm::Instruction::SIToFP ||
             cast->getOpcode() == llvm::Instruction::UIToFP) {
    // Floating point conversions: preserve interval (matches SVF)
    as[lhsId] = AbstractValue(srcVal);
  } else if (cast->getOpcode() == llvm::Instruction::FPTrunc ||
             cast->getOpcode() == llvm::Instruction::FPExt) {
    // Floating point truncation/extension: preserve interval (matches SVF)
    as[lhsId] = AbstractValue(srcVal);
  } else {
    // Other casts: preserve value
    as[lhsId] = AbstractValue(srcVal);
  }
}

void AbstractInterpretation::updateStateOnCall(const llvm::CallBase *call) {
  std::vector<const llvm::Function *> callees = getCallees(call);
  if (callees.empty()) {
    if (const llvm::Function *direct = call->getCalledFunction()) {
      callees.push_back(direct);
    } else {
      return;
    }
  }

  std::vector<const llvm::Function *> extCallees;
  std::vector<const llvm::Function *> intCallees;
  for (const llvm::Function *callee : callees) {
    if (!callee)
      continue;
    if (callee->isDeclaration()) {
      extCallees.push_back(callee);
    } else {
      intCallees.push_back(callee);
    }
  }

  auto callIt = abstractTrace.find(call);
  if (callIt == abstractTrace.end())
    return;

  const AbstractState baseState = callIt->second;
  AbstractState joinedState = baseState;
  bool hasJoinedState = false;

  if (!extCallees.empty()) {
    callIt->second = baseState;
    handleExtCall(call, extCallees);
    joinedState = callIt->second;
    hasJoinedState = true;
  }

  if (!intCallees.empty()) {
    callIt->second = baseState;
    handleFunCall(call, intCallees);
    if (hasJoinedState) {
      joinedState.joinWith(callIt->second);
    } else {
      joinedState = callIt->second;
      hasJoinedState = true;
    }
  }

  if (hasJoinedState) {
    callIt->second = joinedState;
  } else {
    callIt->second = baseState;
  }
}

void AbstractInterpretation::updateStateOnRet(const llvm::ReturnInst *ret) {
  if (ret->getReturnValue()) {
    uint32_t retId = getValueId(ret->getReturnValue());
    AbstractState &as = abstractTrace[ret];
    abstractTrace[ret][AbstractInterpretation::ReturnValueId] = as[retId];
  }
}

void AbstractInterpretation::updateStateOnCallPE(const llvm::CallBase *call,
                                                 const llvm::Function *callee,
                                                 AbstractState &callState) {
  if (!callee || callee->isDeclaration())
    return;

  for (unsigned i = 0; i < call->arg_size() && i < callee->arg_size(); ++i) {
    const llvm::Value *argOp = call->getArgOperand(i);
    uint32_t argId = getValueId(argOp);
    uint32_t paramId = getValueId(callee->getArg(i));

    AbstractValue argVal;
    bool hasArgVal = false;

    auto argIt = callState._varToAbsVal.find(argId);
    if (argIt != callState._varToAbsVal.end()) {
      argVal = argIt->second;
      hasArgVal = true;
    }

    if (!hasArgVal) {
      if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(argOp)) {
        uint32_t basePtrId = getValueId(load->getPointerOperand());
        argVal = callState.loadValue(basePtrId);
        if (!argVal.getAddrs().isBottom() || !argVal.getInterval().isBottom()) {
          hasArgVal = true;
        }
      }
    }

    if (!hasArgVal && llvm::isa<llvm::ConstantPointerNull>(argOp)) {
      argVal = AbstractValue(AddressValue(NullMemAddr));
      hasArgVal = true;
    }

    if (hasArgVal) {
      callState[paramId] = argVal;
    }
  }
}

void AbstractInterpretation::updateStateOnRetPE(const llvm::ReturnInst *ret,
                                                const llvm::CallBase *call,
                                                AbstractState &callerState) {
  if (!ret->getReturnValue() || !call)
    return;

  uint32_t retId = getValueId(ret->getReturnValue());
  uint32_t callId = getValueId(call);

  AbstractState &calleeState = abstractTrace[ret];
  auto retIt = calleeState._varToAbsVal.find(retId);
  if (retIt != calleeState._varToAbsVal.end()) {
    if (callerState._varToAbsVal.count(AbstractInterpretation::ReturnValueId) ==
        0) {
      callerState[AbstractInterpretation::ReturnValueId] = retIt->second;
    } else {
      callerState[AbstractInterpretation::ReturnValueId].join_with(
          retIt->second);
    }
    if (callerState._varToAbsVal.count(callId) == 0) {
      callerState[callId] = retIt->second;
    } else {
      callerState[callId].join_with(retIt->second);
    }
  }
}

void AbstractInterpretation::updateStateOnCopy(const llvm::Value *dst,
                                               const llvm::Value *src) {
  const llvm::Instruction *curInst = getCurrentInstruction();
  if (!curInst)
    return;

  uint32_t dstId = getValueId(dst);
  uint32_t srcId = getValueId(src);

  AbstractState &as = abstractTrace[curInst];

  if (as.inVarToValTable(srcId)) {
    as[dstId] = as[srcId];
  } else if (as.inVarToAddrsTable(srcId)) {
    as[dstId] = as[srcId];
  }
}

const llvm::Instruction *AbstractInterpretation::getCurrentInstruction() const {
  return this->currentInstruction_;
}

void AbstractInterpretation::updateStateOnGep(
    const llvm::GetElementPtrInst *gep) {
  uint32_t lhsId = getValueId(gep);
  uint32_t ptrId = getValueId(gep->getPointerOperand());

  AbstractState &as = abstractTrace[gep];

  // Use getElementIndex (like SVF) instead of getByteOffset: getElementIndex
  // clamps with meet_with([0, MaxFieldLimit]) so never returns infinity,
  // avoiding getIntNumeral() assertion when offset has infinite bounds.
  IntervalValue offset = as.getElementIndex(gep);

  if (as.inVarToAddrsTable(ptrId)) {
    AddressValue gepAddrs = as.getGepObjAddrs(ptrId, offset, gep);
    as[lhsId] = AbstractValue(gepAddrs);
  } else {
    as[lhsId] = AbstractValue(AddressValue());
  }
}

void AbstractInterpretation::updateStateOnSelect(
    const llvm::SelectInst *select) {
  uint32_t lhsId = getValueId(select);
  uint32_t condId = getValueId(select->getCondition());
  uint32_t trueId = getValueId(select->getTrueValue());
  uint32_t falseId = getValueId(select->getFalseValue());

  AbstractState &as = abstractTrace[select];

  AbstractValue result;

  if (as.inVarToValTable(condId)) {
    IntervalValue cond = as[condId].getInterval();
    if (cond.is_numeral()) {
      if (cond.getIntNumeral() != 0 && as.inVarToValTable(trueId)) {
        result = as[trueId];
      } else if (cond.getIntNumeral() == 0 && as.inVarToValTable(falseId)) {
        result = as[falseId];
      }
    } else {
      // Join both branches if condition is unknown
      if (as.inVarToValTable(trueId) && as.inVarToValTable(falseId)) {
        result = as[trueId];
        result.join_with(as[falseId]);
      } else if (as.inVarToValTable(trueId)) {
        result = as[trueId];
      } else if (as.inVarToValTable(falseId)) {
        result = as[falseId];
      }
    }
  } else {
    // Condition unknown, join both
    if (as.inVarToValTable(trueId) && as.inVarToValTable(falseId)) {
      result = as[trueId];
      result.join_with(as[falseId]);
    } else if (as.inVarToValTable(trueId)) {
      result = as[trueId];
    } else if (as.inVarToValTable(falseId)) {
      result = as[falseId];
    }
  }

  as[lhsId] = result;
}

void AbstractInterpretation::updateStateOnPhi(const llvm::PHINode *phi) {
  uint32_t resId = getValueId(phi);
  AbstractState &as = abstractTrace[phi];
  AbstractValue rhs;
  bool first = true;

  for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
    llvm::Value *val = phi->getIncomingValue(i);
    uint32_t curId = getValueId(val);
    const llvm::BasicBlock *opBB = phi->getIncomingBlock(i);

    // Get state from the predecessor block (stored at terminator or first inst)
    AbstractState opAs;
    bool foundState = false;

    const llvm::Instruction *term = opBB->getTerminator();
    auto it = abstractTrace.find(term);
    if (it != abstractTrace.end()) {
      opAs = it->second;
      foundState = true;
    } else if (!opBB->empty()) {
      auto it2 = abstractTrace.find(&opBB->front());
      if (it2 != abstractTrace.end()) {
        opAs = it2->second;
        foundState = true;
      }
    }

    if (foundState) {
      // Check if this edge is feasible (for conditional branches and switches)
      bool edgeFeasible = true;

      if (const llvm::BranchInst *branch =
              llvm::dyn_cast<llvm::BranchInst>(term)) {
        if (branch->isConditional()) {
          // Determine which successor this edge leads to
          bool isTrueEdge = branch->getSuccessor(0) == phi->getParent();
          AbstractState testState = opAs;
          edgeFeasible = isBranchFeasible(branch, testState, isTrueEdge);
          if (edgeFeasible) {
            opAs = testState;
          }
        }
      } else if (const llvm::SwitchInst *switchInst =
                     llvm::dyn_cast<llvm::SwitchInst>(term)) {
        // Check if this PHI incoming edge corresponds to a feasible switch case
        // The switch is in opBB, and we're checking if the edge from opBB to
        // phi->getParent() is feasible
        edgeFeasible = false;

        // Check if phi->getParent() is the default destination
        if (phi->getParent() == switchInst->getDefaultDest()) {
          // Default edge is feasible unless we can prove the condition equals a
          // specific case value (singleton interval exactly on a case).
          uint32_t condId = getValueId(switchInst->getCondition());
          if (!opAs.inVarToValTable(condId)) {
            edgeFeasible = true;
          } else {
            const IntervalValue &condVal = opAs[condId].getInterval();
            if (condVal.isBottom()) {
              edgeFeasible = false;
            } else if (!condVal.is_numeral()) {
              edgeFeasible = true;
            } else {
              const int64_t exact = condVal.getIntNumeralOrZero();
              bool matchesCase = false;
              for (auto caseIt = switchInst->case_begin();
                   caseIt != switchInst->case_end(); ++caseIt) {
                const llvm::ConstantInt *caseVal = caseIt->getCaseValue();
                if (caseVal->getSExtValue() == exact) {
                  matchesCase = true;
                  break;
                }
              }
              edgeFeasible = !matchesCase;
            }
          }
        } else {
          // Check if phi->getParent() corresponds to a case value
          for (auto caseIt = switchInst->case_begin();
               caseIt != switchInst->case_end(); ++caseIt) {
            if (caseIt->getCaseSuccessor() == phi->getParent()) {
              const llvm::ConstantInt *caseVal = caseIt->getCaseValue();
              int64_t caseValue = caseVal->getSExtValue();
              AbstractState testState = opAs;
              if (isSwitchBranchFeasible(switchInst, caseValue, testState)) {
                opAs = testState;
                edgeFeasible = true;
                break;
              }
            }
          }
        }
      }

      if (!edgeFeasible) {
        continue; // Skip infeasible edge
      }

      materializeConstantValue(opAs, val, curId);

      if (opAs.inVarToValTable(curId) || opAs.inVarToAddrsTable(curId)) {
        if (first) {
          rhs = opAs[curId];
          first = false;
        } else {
          rhs.join_with(opAs[curId]);
        }
      }
    }
  }

  if (!first) {
    as[resId] = rhs;
  } else {
    // No feasible predecessors - initialize to top
    as[resId] = AbstractValue(IntervalValue::top());
  }
}

void AbstractInterpretation::updateStateOnExtractValue(
    const llvm::ExtractValueInst *extract) {
  uint32_t lhsId = getValueId(extract);
  uint32_t aggId = getValueId(extract->getAggregateOperand());

  AbstractState &as = abstractTrace[extract];

  // ExtractValue extracts a field from an aggregate value
  // For abstract interpretation, we conservatively propagate the value
  // In SVF, this would be a CopyStmt from aggregate field to result
  if (as.inVarToValTable(aggId)) {
    updateStateOnCopy(extract, extract->getAggregateOperand());
  } else if (as.inVarToAddrsTable(aggId)) {
    updateStateOnCopy(extract, extract->getAggregateOperand());
  } else {
    // Conservative: set to top
    as[lhsId] = AbstractValue(IntervalValue::top());
  }
}

void AbstractInterpretation::updateStateOnInsertValue(
    const llvm::InsertValueInst *insert) {
  uint32_t lhsId = getValueId(insert);
  uint32_t aggId = getValueId(insert->getAggregateOperand());
  uint32_t valId = getValueId(insert->getInsertedValueOperand());

  AbstractState &as = abstractTrace[insert];

  // InsertValue inserts a value into an aggregate and returns the new aggregate
  // For abstract interpretation, we join the aggregate and inserted value
  // In SVF, this would be a CopyStmt that combines values
  AbstractValue result;
  bool hasValue = false;

  if (as.inVarToValTable(aggId) || as.inVarToAddrsTable(aggId)) {
    updateStateOnCopy(insert, insert->getAggregateOperand());
    result = as[lhsId];
    hasValue = true;
  }

  if (as.inVarToValTable(valId) || as.inVarToAddrsTable(valId)) {
    if (hasValue) {
      result.join_with(as[valId]);
    } else {
      result = as[valId];
      hasValue = true;
    }
  }

  if (hasValue) {
    as[lhsId] = result;
  } else {
    as[lhsId] = AbstractValue(IntervalValue::top());
  }
}

void AbstractInterpretation::updateStateOnExtractElement(
    const llvm::ExtractElementInst *extract) {
  uint32_t lhsId = getValueId(extract);
  uint32_t vecId = getValueId(extract->getVectorOperand());

  AbstractState &as = abstractTrace[extract];

  // ExtractElement extracts an element from a vector
  // For abstract interpretation, we propagate the vector's abstract value
  // In SVF, this would be a CopyStmt from vector element to result
  if (as.inVarToValTable(vecId)) {
    updateStateOnCopy(extract, extract->getVectorOperand());
  } else if (as.inVarToAddrsTable(vecId)) {
    updateStateOnCopy(extract, extract->getVectorOperand());
  } else {
    as[lhsId] = AbstractValue(IntervalValue::top());
  }
}

void AbstractInterpretation::updateStateOnInsertElement(
    const llvm::InsertElementInst *insert) {
  uint32_t lhsId = getValueId(insert);
  uint32_t vecId = getValueId(insert->getOperand(0));
  uint32_t valId = getValueId(insert->getOperand(2));

  AbstractState &as = abstractTrace[insert];

  // InsertElement inserts a value into a vector and returns the new vector
  // For abstract interpretation, we join the vector and inserted value
  // In SVF, this would be a CopyStmt that combines values
  AbstractValue result;
  bool hasValue = false;

  if (as.inVarToValTable(vecId) || as.inVarToAddrsTable(vecId)) {
    updateStateOnCopy(insert, insert->getOperand(0));
    result = as[lhsId];
    hasValue = true;
  }

  if (as.inVarToValTable(valId) || as.inVarToAddrsTable(valId)) {
    if (hasValue) {
      result.join_with(as[valId]);
    } else {
      result = as[valId];
      hasValue = true;
    }
  }

  if (hasValue) {
    as[lhsId] = result;
  } else {
    as[lhsId] = AbstractValue(IntervalValue::top());
  }
}

void AbstractInterpretation::updateStateOnShuffleVector(
    const llvm::ShuffleVectorInst *shuffle) {
  uint32_t lhsId = getValueId(shuffle);
  uint32_t v1Id = getValueId(shuffle->getOperand(0));
  uint32_t v2Id = getValueId(shuffle->getOperand(1));

  AbstractState &as = abstractTrace[shuffle];

  // ShuffleVector shuffles elements from two vectors into a new vector
  // For abstract interpretation, we join both vectors' abstract values
  // In SVF, this would be a CopyStmt that combines values from both sources
  AbstractValue result;
  bool hasValue = false;

  if (as.inVarToValTable(v1Id) || as.inVarToAddrsTable(v1Id)) {
    updateStateOnCopy(shuffle, shuffle->getOperand(0));
    result = as[lhsId];
    hasValue = true;
  }

  if (as.inVarToValTable(v2Id) || as.inVarToAddrsTable(v2Id)) {
    if (hasValue) {
      result.join_with(as[v2Id]);
    } else {
      result = as[v2Id];
      hasValue = true;
    }
  }

  if (hasValue) {
    as[lhsId] = result;
  } else {
    as[lhsId] = AbstractValue(IntervalValue::top());
  }
}

uint32_t AbstractInterpretation::getValueId(const llvm::Value *val) {
  if (!val)
    return 0;

  if (llvm::isa<llvm::ConstantPointerNull>(val)) {
    idToValueMap_[0] = val;
    return 0;
  }

  // Check if we already have an ID for this value
  auto it = valueToIdMap_.find(val);
  if (it != valueToIdMap_.end()) {
    return it->second;
  }

  // Assign a new stable ID
  uint32_t id = nextValueId_++;
  valueToIdMap_[val] = id;
  idToValueMap_[id] = val;

  return id;
}

const llvm::Value *AbstractInterpretation::getValueFromId(uint32_t id) const {
  auto it = idToValueMap_.find(id);
  if (it != idToValueMap_.end()) {
    return it->second;
  }
  return nullptr;
}

bool AbstractInterpretation::isExtCall(const llvm::CallBase *callNode) {
  for (const llvm::Function *callee : getCallees(callNode)) {
    if (callee && callee->isDeclaration()) {
      return true;
    }
  }
  return false;
}

void AbstractInterpretation::handleExtCall(
    const llvm::CallBase *callNode,
    const std::vector<const llvm::Function *> &callees) {
  auto traceIt = abstractTrace.find(callNode);
  if (traceIt == abstractTrace.end())
    return;

  callSiteStack.push_back(callNode);
  const AbstractState baseState = traceIt->second;
  AbstractState joinedState = baseState;
  bool hasJoinedState = false;

  if (callees.empty()) {
    if (const llvm::Function *direct = callNode->getCalledFunction()) {
      traceIt->second = baseState;
      utils->handleExtAPI(callNode, direct);
      joinedState = traceIt->second;
      hasJoinedState = true;
    }
  } else {
    for (const llvm::Function *callee : callees) {
      if (!callee || !callee->isDeclaration())
        continue;
      traceIt->second = baseState;
      utils->handleExtAPI(callNode, callee);
      if (hasJoinedState) {
        joinedState.joinWith(traceIt->second);
      } else {
        joinedState = traceIt->second;
        hasJoinedState = true;
      }
    }
  }

  if (hasJoinedState) {
    traceIt->second = joinedState;
  } else {
    traceIt->second = baseState;
  }

  for (auto &detector : detectors) {
    detector->handleStubFunctions(callNode);
  }
  callSiteStack.pop_back();
}

bool AbstractInterpretation::isRecursiveFun(const llvm::Function *fun) const {
  // Use the recursiveFuns_ set populated by initCallGraphSCC()
  return recursiveFuns_.count(fun) > 0;
}

bool AbstractInterpretation::isRecursiveCall(const llvm::CallBase *callNode) {
  const llvm::Function *callfun = callNode->getCalledFunction();
  if (!callfun)
    return false;
  return isRecursiveFun(callfun);
}

void AbstractInterpretation::handleCallSite(const llvm::CallBase *call) {
  updateStateOnCall(call);
}

void AbstractInterpretation::handleFunCall(
    const llvm::CallBase *callNode,
    const std::vector<const llvm::Function *> &callees) {
  if (callees.empty())
    return;

  auto callIt = abstractTrace.find(callNode);
  if (callIt == abstractTrace.end())
    return;

  callSiteStack.push_back(callNode);

  bool gotReturn = false;
  AbstractValue joinedReturn;
  bool gotPostState = false;
  AbstractState joinedPostState;

  for (const llvm::Function *callee : callees) {
    if (!callee || callee->isDeclaration())
      continue;

    // Build call state and map actuals to formals.
    AbstractState callState = callIt->second;
    updateStateOnCallPE(callNode, callee, callState);

    const llvm::BasicBlock &entryBlock = callee->getEntryBlock();
    if (!entryBlock.empty()) {
      const llvm::Instruction *entryInst = &entryBlock.front();
      auto entryIt = abstractTrace.find(entryInst);
      if (entryIt == abstractTrace.end()) {
        abstractTrace[entryInst] = callState;
      } else {
        AbstractState merged = entryIt->second;
        merged.joinWith(callState);
        abstractTrace[entryInst] = merged;
      }
    }

    // Recursive callsite inside SCC: do not recurse.
    // Instead, feed argument state to the callee entry and consume the current
    // callee summary (if any), matching SVF's interprocedural cycle feedback.
    if (isRecursiveCallSite(callNode, callee)) {
      if (recursionMode_ == TOP && isRecursiveFun(callee)) {
        recursiveCallPass(callNode, callee);
        continue;
      }

      AbstractValue calleeReturn;
      bool calleeHasReturn = collectCalleeReturnValue(callee, calleeReturn);
      if (calleeHasReturn) {
        if (!gotReturn) {
          joinedReturn = calleeReturn;
          gotReturn = true;
        } else {
          joinedReturn.join_with(calleeReturn);
        }
      }
      continue;
    }

    // Analyze callee.
    if (isRecursiveFun(callee)) {
      handleRecursiveSCC(callee);
    } else {
      handleFunction(callee);
    }

    // Propagate all concrete return edges to caller state (RetPE equivalent).
    for (const llvm::BasicBlock &bb : *callee) {
      const auto *retInst =
          llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
      if (!retInst || !retInst->getReturnValue())
        continue;
      updateStateOnRetPE(retInst, callNode, callIt->second);
    }

    for (const llvm::BasicBlock &bb : *callee) {
      const auto *retInst =
          llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
      if (!retInst)
        continue;
      auto retIt = abstractTrace.find(retInst);
      if (retIt == abstractTrace.end())
        continue;

      AbstractState postState = retIt->second;
      if (!callNode->getType()->isVoidTy()) {
        auto rvIt =
            postState._varToAbsVal.find(AbstractInterpretation::ReturnValueId);
        if (rvIt != postState._varToAbsVal.end()) {
          postState[getValueId(callNode)] = rvIt->second;
        }
      }

      if (!gotPostState) {
        joinedPostState = postState;
        gotPostState = true;
      } else {
        joinedPostState.joinWith(postState);
      }
    }

    AbstractValue calleeReturn;
    bool calleeHasReturn = collectCalleeReturnValue(callee, calleeReturn);

    if (recursionMode_ == TOP && isRecursiveFun(callee)) {
      setTopToObjInRecursion(callNode, callee);
      calleeReturn = AbstractValue(IntervalValue::top());
      calleeHasReturn = true;
    }

    if (calleeHasReturn) {
      if (!gotReturn) {
        joinedReturn = calleeReturn;
        gotReturn = true;
      } else {
        joinedReturn.join_with(calleeReturn);
      }
    }
  }

  if (!callSiteStack.empty() && callSiteStack.back() == callNode) {
    auto it = abstractTrace.find(callNode);
    if (it != abstractTrace.end()) {
      if (gotPostState) {
        it->second = joinedPostState;
      }
      if (callNode->getType()->isVoidTy() == false && gotReturn) {
        uint32_t lhsId = getValueId(callNode);
        it->second[lhsId] = joinedReturn;
      }
    }
  }

  callSiteStack.pop_back();
}

bool AbstractInterpretation::collectCalleeReturnValue(
    const llvm::Function *callee, AbstractValue &joinedReturn) const {
  bool calleeHasReturn = false;
  for (const llvm::BasicBlock &bb : *callee) {
    const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
    if (!ret || !ret->getReturnValue())
      continue;
    auto retIt = abstractTrace.find(ret);
    if (retIt == abstractTrace.end())
      continue;
    auto rvIt =
        retIt->second._varToAbsVal.find(AbstractInterpretation::ReturnValueId);
    if (rvIt == retIt->second._varToAbsVal.end())
      continue;
    if (!calleeHasReturn) {
      joinedReturn = rvIt->second;
      calleeHasReturn = true;
    } else {
      joinedReturn.join_with(rvIt->second);
    }
  }
  return calleeHasReturn;
}

void AbstractInterpretation::handleRecursiveSCC(const llvm::Function *seed) {
  auto idIt = recursiveSccIdMap_.find(seed);
  if (idIt == recursiveSccIdMap_.end()) {
    handleFunction(seed);
    return;
  }
  auto membersIt = recursiveSccMembers_.find(idIt->second);
  if (membersIt == recursiveSccMembers_.end() || membersIt->second.empty()) {
    handleFunction(seed);
    return;
  }

  // Function-level WTOs do not capture interprocedural recursive cycles.
  // Iterate over SCC entry summaries here so recursive summary propagation
  // converges, while each function still uses its own WTO for intra-procedural
  // cycles.
  if (recursionMode_ == TOP) {
    for (const llvm::Function *func : membersIt->second) {
      if (!func || func->isDeclaration() || func->empty())
        continue;
      handleFunction(func);
    }
    return;
  }

  bool increasing = true;
  for (uint32_t iter = 0;; ++iter) {
    bool stable = true;

    for (const llvm::Function *func : membersIt->second) {
      if (!func || func->isDeclaration() || func->empty())
        continue;

      const llvm::Instruction *entryInst = &func->getEntryBlock().front();
      AbstractState prevEntry;
      bool hadPrevEntry = false;
      auto beforeIt = abstractTrace.find(entryInst);
      if (beforeIt != abstractTrace.end()) {
        prevEntry = beforeIt->second;
        hadPrevEntry = true;
      }

      handleFunction(func);

      auto afterIt = abstractTrace.find(entryInst);
      if (afterIt == abstractTrace.end())
        continue;

      if (iter >= widenDelay_) {
        if (increasing) {
          if (hadPrevEntry) {
            AbstractState widened = prevEntry.widening(afterIt->second);
            afterIt->second = widened;
            if (widened != prevEntry) {
              stable = false;
            }
          } else {
            stable = false;
          }
        } else {
          if (!shouldApplyNarrowing(func)) {
            continue;
          }
          if (hadPrevEntry) {
            AbstractState narrowed = prevEntry.narrowing(afterIt->second);
            afterIt->second = narrowed;
            if (narrowed != prevEntry) {
              stable = false;
            }
          } else {
            stable = false;
          }
        }
      } else if (!hadPrevEntry || afterIt->second != prevEntry) {
        stable = false;
      }
    }

    if (iter < widenDelay_) {
      if (stable) {
        break;
      }
      continue;
    }

    if (increasing) {
      if (!stable) {
        continue;
      }
      if (shouldApplyNarrowing(seed)) {
        increasing = false;
        continue;
      }
      break;
    }

    if (stable) {
      break;
    }
  }
}

/// Check if a call is a recursive callsite (within same SCC, not entry call
/// from outside) This matches SVF's isRecursiveCallSite logic
bool AbstractInterpretation::isRecursiveCallSite(
    const llvm::CallBase *callNode, const llvm::Function *callee) const {
  // Only callsites to recursive functions can be recursive callsites.
  if (!callee || !isRecursiveFun(callee))
    return false;
  // If this call site is marked as non-recursive (entry call), it's not a
  // recursive callsite.
  return nonRecursiveCallSites_.find({callNode, callee}) ==
         nonRecursiveCallSites_.end();
}

bool AbstractInterpretation::skipRecursiveCall(const llvm::CallBase *callNode) {
  for (const llvm::Function *callee : getCallees(callNode)) {
    if (!callee)
      continue;
    if (!isRecursiveFun(callee))
      return false;
    if (!isRecursiveCallSite(callNode, callee))
      return false;
  }
  return true;
}

std::vector<const llvm::Function *>
AbstractInterpretation::getCallees(const llvm::CallBase *callNode) const {
  std::vector<const llvm::Function *> result;
  if (!callNode)
    return result;

  auto collectFunctionsFromValue =
      [&](const llvm::Value *value, std::set<const llvm::Function *> &targets,
          auto &&collectRef, std::set<const llvm::Value *> &visited) -> void {
    if (!value || !visited.insert(value).second)
      return;

    value = value->stripPointerCasts();
    if (const auto *fun = llvm::dyn_cast<llvm::Function>(value)) {
      targets.insert(fun);
      return;
    }

    if (const auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(value)) {
      for (const llvm::Use &operand : ce->operands()) {
        collectRef(operand.get(), targets, collectRef, visited);
      }
      return;
    }

    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(value)) {
      const llvm::Value *ptr = load->getPointerOperand()->stripPointerCasts();
      if (const auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(ptr)) {
        if (gv->hasInitializer()) {
          collectRef(gv->getInitializer(), targets, collectRef, visited);
        }
      }
      for (const llvm::User *user : ptr->users()) {
        if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(user)) {
          if (store->getPointerOperand()->stripPointerCasts() == ptr) {
            collectRef(store->getValueOperand(), targets, collectRef, visited);
          }
        }
      }
      return;
    }

    if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
      for (const llvm::Value *incoming : phi->incoming_values()) {
        collectRef(incoming, targets, collectRef, visited);
      }
      return;
    }

    if (const auto *select = llvm::dyn_cast<llvm::SelectInst>(value)) {
      collectRef(select->getTrueValue(), targets, collectRef, visited);
      collectRef(select->getFalseValue(), targets, collectRef, visited);
    }
  };

  // Direct call.
  if (const llvm::Function *direct = callNode->getCalledFunction()) {
    result.push_back(direct);
    return result;
  }

  // Indirect call: collect all resolved targets from PTA call graph.
  if (!ptaReady_ || !pta_ || !pta_->pass)
    return result;

  const auto *cg = pta_->pass->getPTA()->getCallGraph();
  if (!cg)
    return result;

  std::set<const llvm::Function *> uniqueTargets;
  for (auto nodeIt = cg->begin(); nodeIt != cg->end(); ++nodeIt) {
    const auto *cgNode = *nodeIt;
    if (!cgNode || !cgNode->isIndirectCall())
      continue;
    auto *indCall = cgNode->getTargetFunPtr();
    if (!indCall)
      continue;
    if (indCall->getCallSite() != callNode)
      continue;

    for (const auto *resolvedNode : indCall->getResolvedNode()) {
      if (!resolvedNode || resolvedNode->isIndirectCall())
        continue;
      if (const auto *targetFun = resolvedNode->getTargetFun()) {
        const llvm::Function *calleeFunc = targetFun->getFunction();
        if (calleeFunc) {
          uniqueTargets.insert(calleeFunc);
        }
      }
    }

    for (auto succIt = cgNode->succ_begin(); succIt != cgNode->succ_end();
         ++succIt) {
      const auto *calleeNode = *succIt;
      if (!calleeNode || calleeNode->isIndirectCall())
        continue;
      if (const auto *targetFun = calleeNode->getTargetFun()) {
        const llvm::Function *calleeFunc = targetFun->getFunction();
        if (calleeFunc) {
          uniqueTargets.insert(calleeFunc);
        }
      }
    }
  }

  if (uniqueTargets.empty()) {
    std::set<const llvm::Value *> visited;
    collectFunctionsFromValue(callNode->getCalledOperand(), uniqueTargets,
                              collectFunctionsFromValue, visited);
  }

  result.assign(uniqueTargets.begin(), uniqueTargets.end());
  return result;
}

const llvm::Function *
AbstractInterpretation::getCallee(const llvm::CallBase *callNode) {
  std::vector<const llvm::Function *> callees = getCallees(callNode);
  if (callees.empty())
    return nullptr;
  return callees.front();
}

bool AbstractInterpretation::shouldApplyNarrowing(const llvm::Function *fun) {
  // Non-recursive functions (regular loops): always apply narrowing.
  if (!fun || !isRecursiveFun(fun))
    return true;

  // Recursive functions: mode-dependent.
  switch (recursionMode_) {
  case TOP:
    return false;
  case WIDEN_ONLY:
    return false;
  case WIDEN_NARROW:
    return true;
  default:
    return false;
  }
}

void AbstractInterpretation::collectCheckPoint() {
  // Matching SVF's checkpoint names
  std::set<std::string> ae_checkpoint_names = {"svf_assert", "svf_assert_eq"};

  // Buffer overflow checkpoint names (enabled by enableBufOverflowCheck_)
  std::set<std::string> buf_checkpoint_names = {"UNSAFE_BUFACCESS",
                                                "SAFE_BUFACCESS"};

  // Null dereference checkpoint names (enabled by enableNullDerefCheck_)
  std::set<std::string> nullptr_checkpoint_names = {"UNSAFE_LOAD", "SAFE_LOAD"};

  // Division by zero checkpoint names (enabled by enableDivZeroCheck_)
  std::set<std::string> divzero_checkpoint_names = {"UNSAFE_DIVZERO",
                                                    "SAFE_DIVZERO"};

  // Integer overflow checkpoint names (enabled by enableOverflowCheck_)
  std::set<std::string> overflow_checkpoint_names = {"UNSAFE_OVERFLOW",
                                                     "SAFE_OVERFLOW"};

  for (auto &func : *module_) {
    for (auto &bb : func) {
      for (auto &inst : bb) {
        if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
          if (const llvm::Function *callee = call->getCalledFunction()) {
            std::string funName = callee->getName().str();

            // Always collect svf_assert checkpoints
            if (ae_checkpoint_names.count(funName)) {
              checkpoints.insert(call);
              continue;
            }

            // Collect buffer overflow checkpoints if enabled
            if (enableBufOverflowCheck_ &&
                buf_checkpoint_names.count(funName)) {
              checkpoints.insert(call);
              continue;
            }

            // Collect null dereference checkpoints if enabled
            if (enableNullDerefCheck_ &&
                nullptr_checkpoint_names.count(funName)) {
              checkpoints.insert(call);
              continue;
            }

            // Collect division by zero checkpoints if enabled
            if (enableDivZeroCheck_ &&
                divzero_checkpoint_names.count(funName)) {
              checkpoints.insert(call);
              continue;
            }

            // Collect integer overflow checkpoints if enabled
            if (enableOverflowCheck_ &&
                overflow_checkpoint_names.count(funName)) {
              checkpoints.insert(call);
              continue;
            }
          }
        }
      }
    }
  }

  // Initialize checked checkpoints set
  checkedCheckpoints_.clear();
}

void AbstractInterpretation::checkPointAllSet() {
  if (checkpoints.empty())
    return;

  // Count unchecked checkpoints
  std::vector<const llvm::CallBase *> unchecked;
  for (const auto *call : checkpoints) {
    if (!checkedCheckpoints_.count(call)) {
      unchecked.push_back(call);
    }
  }

  if (unchecked.empty())
    return;

  std::string msg = "At least one checkpoint has not been checked (" +
                    std::to_string(unchecked.size()) + " remaining):\n";
  for (const auto *call : unchecked) {
    std::string callStr;
    llvm::raw_string_ostream os(callStr);
    os << *call;
    os.flush();
    msg += "  " + callStr + "\n";
  }

  if (strictCheckpoint_) {
    llvm::report_fatal_error(llvm::StringRef(msg));
  } else {
    llvm::errs() << "Warning: " << msg;
  }
}

void AbstractInterpretation::recursiveCallPass(const llvm::CallBase *callNode,
                                               const llvm::Function *callee) {
  if (!callee || !isRecursiveFun(callee))
    return;

  // Get the abstract state at the call site
  AbstractState &callState = abstractTrace[callNode];

  // Set all stores in the recursive function to TOP
  setTopToObjInRecursion(callNode, callee);

  if (!callNode->getType()->isVoidTy() && !callNode->getType()->isPointerTy() &&
      !callNode->getType()->isAggregateType()) {
    uint32_t lhsId = getValueId(callNode);
    callState[lhsId] = AbstractValue(IntervalValue::top());
  }
}

void AbstractInterpretation::setTopToObjInRecursion(
    const llvm::CallBase *callNode, const llvm::Function *callee) {
  if (!callee || !isRecursiveFun(callee))
    return;

  AbstractState &callState = abstractTrace[callNode];
  auto stripPointerProjections = [](const llvm::Value *value) {
    while (value) {
      if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value)) {
        value = gep->getPointerOperand();
        continue;
      }
      if (const auto *bitcast = llvm::dyn_cast<llvm::BitCastOperator>(value)) {
        value = bitcast->getOperand(0);
        continue;
      }
      if (const auto *addrspaceCast =
              llvm::dyn_cast<llvm::AddrSpaceCastOperator>(value)) {
        value = addrspaceCast->getOperand(0);
        continue;
      }
      break;
    }
    return value;
  };

  auto topifyReachableMemory = [&](const llvm::Value *actualPtr) {
    uint32_t ptrId = getValueId(actualPtr);
    if (!callState.inVarToAddrsTable(ptrId))
      return;
    for (uint32_t addr : callState[ptrId].getAddrs()) {
      if (AbstractState::isNullMem(addr) || AbstractState::isInvalidMem(addr))
        continue;
      uint32_t objId = callState.getIDFromAddr(addr);
      if (!callState.inAddrToValTable(objId))
        continue;
      AbstractValue &stored = callState.load(addr);
      if (stored.isInterval()) {
        stored.getInterval().set_to_top();
      }
    }
  };

  for (const llvm::BasicBlock &bb : *callee) {
    for (const llvm::Instruction &inst : bb) {
      const auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
      if (!store)
        continue;

      const llvm::Value *storedVal = store->getValueOperand();
      if (storedVal->getType()->isPointerTy() || storedVal->getType()->isVoidTy())
        continue;

      const llvm::Value *base = stripPointerProjections(store->getPointerOperand());
      if (const auto *arg = llvm::dyn_cast<llvm::Argument>(base)) {
        if (arg->getParent() == callee && arg->getArgNo() < callNode->arg_size()) {
          topifyReachableMemory(callNode->getArgOperand(arg->getArgNo()));
        }
        continue;
      }

      if (const auto *global = llvm::dyn_cast<llvm::GlobalValue>(base)) {
        topifyReachableMemory(global);
      }
    }
  }
}

// AEStat implementation (matching SVF's AEStat)
AEStat::AEStat(AbstractInterpretation *ae) : _ae(ae) {}

void AEStat::countStateSize() {
  if (count == 0) {
    // Initialize accumulators for computing averages
    generalNumMap["ES_Var_AVG_Num"] = 0.0;
    generalNumMap["ES_Loc_AVG_Num"] = 0.0;
    generalNumMap["ES_Var_Addr_AVG_Num"] = 0.0;
    generalNumMap["ES_Loc_Addr_AVG_Num"] = 0.0;
  }
  ++count;

  // Collect state size information from the abstract interpretation
  // These values are accumulated during analysis
}

void AEStat::finializeStat() {
  if (count > 0) {
    // Compute averages
    generalNumMap["ES_Var_AVG_Num"] /= count;
    generalNumMap["ES_Loc_AVG_Num"] /= count;
    generalNumMap["ES_Var_Addr_AVG_Num"] /= count;
    generalNumMap["ES_Loc_Addr_AVG_Num"] /= count;
  }

  // Set total statement count (matching SVF's SVF_STMT_NUM)
  generalNumMap["Stmt_Num"] = stmtCount;

  // Set ICFG node count (matching SVF's ICFG_Node_Num)
  generalNumMap["ICFG_Node_Num"] = icfgNodeNum;

  // Set function count (matching SVF's Func_Num)
  generalNumMap["Func_Num"] = funcNum;

  // Set external call site count (matching SVF's EXT_CallSite_Num)
  generalNumMap["EXT_CallSite_Num"] = extCallSiteNum;

  // Set non-external call site count (matching SVF's NonEXT_CallSite_Num)
  generalNumMap["NonEXT_CallSite_Num"] = nonExtCallSiteNum;
}

void AEStat::performStat() {
  unsigned field_width = 30;

  llvm::outs() << "\n************************\n";
  llvm::outs()
      << "################ Abstract Execution Statistics ###############\n";

  // Print general statistics (matching SVF format)
  for (const auto &item : generalNumMap) {
    std::string name = item.first;
    // Pad name to field_width, left-aligned
    if (name.length() < field_width) {
      name.append(field_width - name.length(), ' ');
    }
    llvm::outs() << "  " << name << item.second << "\n";
  }

  llvm::outs() << "-------------------------------------------------------\n";

  // Print time statistics (matching SVF format)
  for (const auto &item : timeStatMap) {
    std::string name = item.first;
    // Pad name to field_width, left-aligned
    if (name.length() < field_width) {
      name.append(field_width - name.length(), ' ');
    }
    llvm::outs() << "  " << name << item.second << "\n";
  }

  llvm::outs() << "Memory usage: " << memUsage << "\n";
  llvm::outs() << "#######################################################\n";
  llvm::outs().flush();
}

void AEStat::startClk() {
  startTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
}

void AEStat::endClk() {
  endTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
  if (startTime > 0) {
    double duration = (endTime - startTime) / 1000.0; // Convert to seconds
    timeStatMap["Total_Time"] = duration;
  }
}

bool AbstractInterpretation::isBranchFeasible(const llvm::BranchInst *branch,
                                              AbstractState &as,
                                              bool isTrueEdge) {
  if (!branch->isConditional()) {
    return true;
  }

  llvm::Value *cond = branch->getCondition();

  // Try to extract comparison from condition
  if (const llvm::CmpInst *cmp = llvm::dyn_cast<llvm::CmpInst>(cond)) {
    // Use the full predicate handling for cmp instructions
    return isCmpBranchFeasible(cmp, isTrueEdge, as);
  }

  // For non-cmp conditions (e.g., phi, select), do simple check
  uint32_t condId = getValueId(cond);

  if (!as.inVarToValTable(condId)) {
    // Unknown condition - assume feasible
    return true;
  }

  IntervalValue condVal = as[condId].getInterval();

  if (condVal.isBottom()) {
    return false;
  }

  // Check if condition can be true/false based on the edge
  if (isTrueEdge) {
    // True edge: condition must be able to be non-zero.
    // Any value except exactly 0 is true in LLVM branch semantics.
    return !condVal.is_zero();
  } else {
    // False edge: condition must be able to be zero
    return condVal.contains(0);
  }
}

bool AbstractInterpretation::isCmpBranchFeasible(const llvm::CmpInst *cmpInst,
                                                 bool succ, AbstractState &as) {
  uint32_t op0Id = getValueId(cmpInst->getOperand(0));
  uint32_t op1Id = getValueId(cmpInst->getOperand(1));
  const llvm::Value *op0Val = cmpInst->getOperand(0);
  const llvm::Value *op1Val = cmpInst->getOperand(1);

  materializeConstantValue(as, op0Val, op0Id);
  materializeConstantValue(as, op1Val, op1Id);

  int32_t predicate = cmpInst->getPredicate();
  if (!succ) {
    auto reversePredMap = getReversePredicate();
    if (reversePredMap.find(predicate) != reversePredMap.end()) {
      predicate = reversePredMap[predicate];
    }
  }

  // Refine pointer-vs-null comparisons to reduce false positives in guarded
  // dereferences (e.g., if (p) { *p = ... }).
  if (predicate == llvm::CmpInst::ICMP_EQ ||
      predicate == llvm::CmpInst::ICMP_NE) {
    if (op0Id == AbstractState::NullPtr && op1Id == AbstractState::NullPtr) {
      return predicate == llvm::CmpInst::ICMP_EQ;
    }

    uint32_t ptrId = 0;
    bool hasNullOperand = false;
    if (op0Id == AbstractState::NullPtr) {
      hasNullOperand = true;
      ptrId = op1Id;
    } else if (op1Id == AbstractState::NullPtr) {
      hasNullOperand = true;
      ptrId = op0Id;
    }

    if (hasNullOperand && as.inVarToAddrsTable(ptrId)) {
      auto refineAddrSet =
          [&](AddressValue::AddrSet vals) -> AddressValue::AddrSet {
        if (predicate == llvm::CmpInst::ICMP_NE) {
          vals.erase(NullMemAddr); // true edge of (p != null)
        } else {
          if (vals.count(NullMemAddr) != 0) {
            vals = {NullMemAddr}; // true edge of (p == null)
          } else {
            vals.clear();
          }
        }
        return vals;
      };

      AddressValue::AddrSet refined =
          refineAddrSet(as[ptrId].getAddrs().getVals());
      if (refined.empty()) {
        return false;
      }
      as[ptrId] = AbstractValue(AddressValue(refined));

      // If compared pointer value comes from a load, refine pointed memory too.
      // This propagates branch facts from `%x = load ...; icmp %x, null` back
      // to the backing variable, so subsequent loads in the guarded block are
      // narrowed.
      const llvm::Value *ptrVal = (ptrId == op0Id) ? op0Val : op1Val;
      if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(ptrVal)) {
        uint32_t basePtrId = getValueId(load->getPointerOperand());
        if (as.inVarToAddrsTable(basePtrId)) {
          bool anyFeasible = false;
          for (auto addr : as[basePtrId].getAddrs()) {
            uint32_t objId = as.getIDFromAddr(addr);
            if (!as.inAddrToAddrsTable(objId)) {
              continue;
            }
            AbstractValue &memVal = as.load(addr);
            AddressValue::AddrSet memRefined =
                refineAddrSet(memVal.getAddrs().getVals());
            if (memRefined.empty()) {
              continue;
            }
            memVal = AbstractValue(AddressValue(memRefined));
            anyFeasible = true;
          }
          if (!anyFeasible) {
            return false;
          }
        }
      }
      return true;
    }
  }

  // Check if operands come from load instructions and handle them
  const llvm::LoadInst *loadOp0 = nullptr;
  const llvm::LoadInst *loadOp1 = nullptr;

  // Try to find if op0 comes from a load instruction
  if (const llvm::LoadInst *load = llvm::dyn_cast<llvm::LoadInst>(op0Val)) {
    loadOp0 = load;
  } else if (const llvm::BitCastInst *bc =
                 llvm::dyn_cast<llvm::BitCastInst>(op0Val)) {
    if (const llvm::LoadInst *load =
            llvm::dyn_cast<llvm::LoadInst>(bc->getOperand(0))) {
      loadOp0 = load;
    }
  }

  // Try to find if op1 comes from a load instruction
  if (const llvm::LoadInst *load = llvm::dyn_cast<llvm::LoadInst>(op1Val)) {
    loadOp1 = load;
  } else if (const llvm::BitCastInst *bc =
                 llvm::dyn_cast<llvm::BitCastInst>(op1Val)) {
    if (const llvm::LoadInst *load =
            llvm::dyn_cast<llvm::LoadInst>(bc->getOperand(0))) {
      loadOp1 = load;
    }
  }

  // Get addresses from loaded pointers for memory refinement
  AddressValue addrOp0, addrOp1;
  if (loadOp0) {
    uint32_t ptrId = getValueId(loadOp0->getPointerOperand());
    if (as.inVarToAddrsTable(ptrId)) {
      addrOp0 = as[ptrId].getAddrs();
    }
  }
  if (loadOp1) {
    uint32_t ptrId = getValueId(loadOp1->getPointerOperand());
    if (as.inVarToAddrsTable(ptrId)) {
      addrOp1 = as[ptrId].getAddrs();
    }
  }

  if (as.inVarToAddrsTable(op0Id) || as.inVarToAddrsTable(op1Id)) {
    return true;
  }

  if (!as.inVarToValTable(op0Id) || !as.inVarToValTable(op1Id)) {
    return true;
  }

  IntervalValue val0 = as[op0Id].getInterval();
  IntervalValue val1 = as[op1Id].getInterval();

  if (val0.isBottom() || val1.isBottom()) {
    return false;
  }

  AbstractState newEs = as;
  uint32_t lhsId = op0Id;
  uint32_t rhsId = op1Id;
  const llvm::LoadInst *lhsLoadOp = loadOp0;
  const llvm::LoadInst *rhsLoadOp = loadOp1;
  AddressValue lhsAddrs = addrOp0;
  AddressValue rhsAddrs = addrOp1;

  bool b0 = val0.is_numeral();
  bool b1 = val1.is_numeral();

  if (b0 && !b1) {
    std::swap(lhsId, rhsId);
    std::swap(lhsLoadOp, rhsLoadOp);
    std::swap(lhsAddrs, rhsAddrs);
    predicate = getSwitchLhsRhsPredicate()[predicate];
  } else if (!b0 && !b1) {
    return true;
  } else if (b0 && b1) {
    return true;
  }

  IntervalValue &lhs = newEs[lhsId].getInterval();
  IntervalValue &rhs = newEs[rhsId].getInterval();

  switch (predicate) {
  case llvm::CmpInst::ICMP_EQ:
  case llvm::CmpInst::FCMP_OEQ:
  case llvm::CmpInst::FCMP_UEQ:
    lhs.meet_with(rhs);
    break;
  case llvm::CmpInst::ICMP_NE:
  case llvm::CmpInst::FCMP_ONE:
  case llvm::CmpInst::FCMP_UNE:
    break;
  case llvm::CmpInst::ICMP_UGT:
  case llvm::CmpInst::ICMP_SGT:
  case llvm::CmpInst::FCMP_OGT:
  case llvm::CmpInst::FCMP_UGT: {
    BoundedInt lb(rhs.lb().getIntNumeral() + 1);
    BoundedInt ub = BoundedInt::plus_infinity();
    lhs.meet_with(IntervalValue(lb, ub));
    break;
  }
  case llvm::CmpInst::ICMP_UGE:
  case llvm::CmpInst::ICMP_SGE:
  case llvm::CmpInst::FCMP_OGE:
  case llvm::CmpInst::FCMP_UGE: {
    BoundedInt lb(rhs.lb().getIntNumeral());
    BoundedInt ub = BoundedInt::plus_infinity();
    lhs.meet_with(IntervalValue(lb, ub));
    break;
  }
  case llvm::CmpInst::ICMP_ULT:
  case llvm::CmpInst::ICMP_SLT:
  case llvm::CmpInst::FCMP_OLT:
  case llvm::CmpInst::FCMP_ULT: {
    BoundedInt lb = BoundedInt::minus_infinity();
    BoundedInt ub(rhs.ub().getIntNumeral() - 1);
    lhs.meet_with(IntervalValue(lb, ub));
    break;
  }
  case llvm::CmpInst::ICMP_ULE:
  case llvm::CmpInst::ICMP_SLE:
  case llvm::CmpInst::FCMP_OLE:
  case llvm::CmpInst::FCMP_ULE: {
    BoundedInt lb = BoundedInt::minus_infinity();
    BoundedInt ub(rhs.ub().getIntNumeral());
    lhs.meet_with(IntervalValue(lb, ub));
    break;
  }
  case llvm::CmpInst::FCMP_FALSE:
    break;
  case llvm::CmpInst::FCMP_TRUE:
    break;
  default:
    return true;
  }

  if (lhs.isBottom()) {
    return false;
  }

  // Refine memory for loaded pointers (matching SVF's behavior)
  // If operand comes from a load, also refine the memory it points to
  if (lhsLoadOp && !lhsAddrs.getVals().empty()) {
    for (const auto &addr : lhsAddrs.getVals()) {
      uint32_t objId = newEs.getIDFromAddr(addr);
      if (newEs.inAddrToValTable(objId)) {
        AbstractValue memVal = newEs.load(addr);
        memVal.getInterval().meet_with(lhs);
        newEs.store(addr, memVal);
      }
    }
  }
  if (rhsLoadOp && !rhsAddrs.getVals().empty()) {
    for (const auto &addr : rhsAddrs.getVals()) {
      uint32_t objId = newEs.getIDFromAddr(addr);
      if (newEs.inAddrToValTable(objId)) {
        AbstractValue memVal = newEs.load(addr);
        memVal.getInterval().meet_with(rhs);
        newEs.store(addr, memVal);
      }
    }
  }

  as = newEs;
  return true;
}

bool AbstractInterpretation::isSwitchBranchFeasible(
    const llvm::SwitchInst *switchInst, int64_t caseValue, AbstractState &as) {
  // Get the switch condition value
  llvm::Value *cond = switchInst->getCondition();
  uint32_t condId = getValueId(cond);

  if (!as.inVarToValTable(condId)) {
    // Unknown condition - assume feasible (conservative)
    return true;
  }

  // Get the interval for the switch condition
  IntervalValue &switchCond = as[condId].getInterval();

  if (switchCond.isBottom()) {
    return false;
  }

  // Check if the case value is within the switch condition's interval
  IntervalValue caseVal(caseValue, caseValue);

  // If the switch condition can equal the case value, the branch is feasible
  if (switchCond.hasIntersect(caseVal)) {
    // Update the abstract state: meet the switch condition with the case value
    // This refines the state for this branch
    AbstractState newState = as;
    newState[condId].getInterval().meet_with(caseVal);

    // If the meet doesn't result in bottom, the branch is feasible
    if (!newState[condId].getInterval().isBottom()) {
      // Update the state for this branch
      as = newState;

      // Also update memory locations if the condition comes from a load
      // This matches SVF's behavior of propagating switch values through memory
      if (const llvm::LoadInst *load = llvm::dyn_cast<llvm::LoadInst>(cond)) {
        uint32_t ptrId = getValueId(load->getPointerOperand());
        if (as.inVarToAddrsTable(ptrId)) {
          AddressValue addrs = as[ptrId].getAddrs();
          for (auto addr : addrs) {
            uint32_t objId = as.getIDFromAddr(addr);
            if (as.inAddrToValTable(objId)) {
              as.load(addr).getInterval().meet_with(caseVal);
            }
          }
        }
      }

      return true;
    }
  }

  return false;
}

bool AbstractInterpretation::checkPathFeasibilityWithSolver(
    const AbstractState &as, const Z3Expr &pathConstraint) {
  // Use the relation solver to check if the path constraint is satisfiable
  // given the abstract state

  // Build gamma_hat expression from abstract state (constraints on variables)
  Z3Expr stateConstraints = relSolver_.gamma_hat(as);

  // Combine state constraints with path constraint
  Z3Expr combinedConstraint = stateConstraints && pathConstraint;

  // Check satisfiability using Z3 solver
  z3::solver &solver = Z3Expr::getSolver();
  z3::params params(Z3Expr::getContext());
  params.set(":timeout", static_cast<unsigned>(1000)); // 1 second timeout
  solver.set(params);

  solver.push();
  solver.add(combinedConstraint.getExpr());

  z3::check_result result = solver.check();
  solver.pop();

  // Path is feasible if satisfiable
  return result == z3::sat;
}

AbstractState AbstractInterpretation::computeRSY(const AbstractState &domain,
                                                 const Z3Expr &phi) {
  // Use the RelationSolver's RSY (Rohn/Speelpenning/Yudell) method
  // for more precise abstract interpretation with relational constraints
  // This helps refine the abstract state based on path constraints
  return relSolver_.RSY(domain, phi);
}

AbstractState
AbstractInterpretation::computeBilateral(const AbstractState &domain,
                                         const Z3Expr &phi) {
  // Use the RelationSolver's bilateral method for precise abstract
  // interpretation Bilateral narrowing combines abstract interpretation with
  // constraint solving to precisely refine the abstract state
  return relSolver_.bilateral(domain, phi);
}

} // namespace analysis
} // namespace lotus
