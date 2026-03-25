#include "Analysis/Spectre/CacheSpecuAnalysis.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>

namespace spectre {

using namespace llvm;

namespace {

struct PointerLocation {
  const Value *Base = nullptr;
  uint64_t Offset = 0;
  bool Precise = false;
};

struct AccessEffect {
  bool Hit = false;
  bool Observed = false;
  SpectreObservation Observation;
};

struct BlockSimulationResult {
  std::unique_ptr<CacheModel> ExitState;
  bool StopSpeculation = false;
};

struct SpeculationState {
  const BranchInst *Branch = nullptr;
  const BasicBlock *MergeBlock = nullptr;
  unsigned Budget = 0;
};

class MemoryResolution {
public:
  MemoryResolution(Function &function, AliasAnalysis *aliasAnalysis,
                   CacheModel &cacheModel)
      : F(function), AA(aliasAnalysis), Cache(cacheModel) {}

  void seedObjects() {
    unsigned argIndex = 0;
    for (Argument &arg : F.args()) {
      ++argIndex;
      registerObject(&arg, arg.getType(), F.getParamAlignment(argIndex),
                     false, false, false, true);
    }

    Module *module = F.getParent();
    for (GlobalVariable &global : module->globals()) {
      registerObject(&global, global.getValueType(), global.getAlignment(),
                     false, true, false, false);
    }

    for (Instruction &inst : instructions(F)) {
      if (auto *allocaInst = dyn_cast<AllocaInst>(&inst)) {
        registerObject(allocaInst, allocaInst->getAllocatedType(),
                       allocaInst->getAlignment(), false, false, true, false);
      } else if (auto *callInst = dyn_cast<CallInst>(&inst)) {
        if (isAllocationCall(callInst)) {
          Type *allocatedTy = callInst->getType()->getPointerElementType();
          if (allocatedTy == nullptr) {
            allocatedTy = Type::getInt8Ty(F.getContext());
          }
          registerObject(callInst, allocatedTy, ARCH_SIZE, true, false, false,
                         false);
        }
      }
    }
  }

  void recordBitCast(const Value *result, const Value *source) {
    PointerAliases[result] = resolvePointer(source);
  }

  void recordGEP(const Value *result, const Value *source, uint64_t beginOffset,
                 bool precise) {
    PointerLocation target = resolvePointer(source);
    target.Offset += beginOffset;
    target.Precise = target.Precise && precise;
    PointerAliases[result] = target;
  }

  void recordPhi(const PHINode &phi) {
    PointerLocation merged;
    bool first = true;
    for (const Value *incoming : phi.incoming_values()) {
      PointerLocation candidate = resolvePointer(incoming);
      if (candidate.Base == nullptr) {
        continue;
      }
      if (first) {
        merged = candidate;
        first = false;
      } else if (merged.Base != candidate.Base || merged.Offset != candidate.Offset) {
        merged.Precise = false;
      }
    }
    if (!first) {
      PointerAliases[&phi] = merged;
    }
  }

  void recordSelect(const SelectInst &select) {
    PointerLocation lhs = resolvePointer(select.getTrueValue());
    PointerLocation rhs = resolvePointer(select.getFalseValue());
    if (lhs.Base == nullptr && rhs.Base == nullptr) {
      return;
    }
    if (lhs.Base == nullptr) {
      lhs = rhs;
    }
    if (rhs.Base != nullptr &&
        (lhs.Base != rhs.Base || lhs.Offset != rhs.Offset)) {
      lhs.Precise = false;
    }
    PointerAliases[&select] = lhs;
  }

  void recordPointerStore(const StoreInst &store) {
    const Value *storedValue = store.getValueOperand();
    if (!storedValue->getType()->isPointerTy()) {
      return;
    }
    PointerAliases[store.getPointerOperand()] = resolvePointer(storedValue);
  }

  ResolvedAccess resolveMemoryAccess(const Instruction &inst, const Value *ptr,
                                     AccessKind kind, bool isRead,
                                     bool isWrite) {
    ResolvedAccess access;
    access.Inst = &inst;
    access.Kind = kind;
    access.IsRead = isRead;
    access.IsWrite = isWrite;

    PointerLocation loc = resolvePointer(ptr);
    if (loc.Base == nullptr && AA != nullptr) {
      loc = resolveViaAliasAnalysis(ptr);
    }
    if (loc.Base == nullptr) {
      return access;
    }

    access.Base = loc.Base;
    access.OffsetBegin = loc.Offset;
    access.OffsetEnd = loc.Offset;
    access.IsPrecise = loc.Precise;
    return access;
  }

  const AbstractMemoryObject *lookupObject(const Value *base) const {
    auto it = Objects.find(base);
    if (it == Objects.end()) {
      return nullptr;
    }
    return &it->second;
  }

  SmallVector<const AbstractMemoryObject *, 8> allObjects() const {
    SmallVector<const AbstractMemoryObject *, 8> result;
    for (const auto &entry : Objects) {
      result.push_back(&entry.second);
    }
    return result;
  }

private:
  Function &F;
  AliasAnalysis *AA;
  CacheModel &Cache;
  std::unordered_map<const Value *, PointerLocation> PointerAliases;
  std::unordered_map<const Value *, AbstractMemoryObject> Objects;

  static bool isAllocationCall(const CallInst *callInst) {
    const Function *callee = callInst->getCalledFunction();
    if (callee == nullptr) {
      return false;
    }
    StringRef name = callee->getName();
    return name.contains("malloc") || name.contains("calloc") ||
           name.contains("realloc") || name.contains("operator new");
  }

  std::string objectName(const Value *value) const {
    if (value == nullptr) {
      return "<unknown>";
    }
    if (value->hasName()) {
      return value->getName().str();
    }
    std::string text;
    raw_string_ostream os(text);
    value->printAsOperand(os, false);
    return os.str();
  }

  void registerObject(Value *base, Type *ty, unsigned alignment, bool isHeap,
                      bool isGlobal, bool isStack, bool isArgument) {
    Cache.AddVar(base, ty, alignment);
    AbstractMemoryObject object;
    object.Base = base;
    object.Name = objectName(base);
    object.Size = CacheModel::GetTySize(ty);
    object.Alignment = alignment;
    object.IsHeap = isHeap;
    object.IsGlobal = isGlobal;
    object.IsStack = isStack;
    object.IsArgument = isArgument;
    Objects[base] = object;
    PointerAliases[base] = PointerLocation{base, 0, true};
  }

  PointerLocation resolveViaAliasAnalysis(const Value *ptr) const {
    if (AA == nullptr) {
      return {};
    }

    PointerLocation resolved;
    for (const auto &entry : Objects) {
      const Value *base = entry.first;
      if (base == nullptr || !base->getType()->isPointerTy() ||
          !ptr->getType()->isPointerTy()) {
        continue;
      }
      AliasResult result = AA->alias(const_cast<Value *>(ptr),
                                     const_cast<Value *>(base));
      if (result != AliasResult::NoAlias) {
        if (resolved.Base == nullptr) {
          resolved = PointerLocation{base, 0, false};
        } else {
          resolved.Precise = false;
          return resolved;
        }
      }
    }
    return resolved;
  }

  PointerLocation resolvePointer(const Value *value) {
    if (value == nullptr) {
      return {};
    }

    auto aliasIt = PointerAliases.find(value);
    if (aliasIt != PointerAliases.end()) {
      return aliasIt->second;
    }

    if (auto *constantExpr = dyn_cast<ConstantExpr>(value)) {
      if (auto *op = dyn_cast<GEPOperator>(constantExpr)) {
        const Value *base = op->getPointerOperand();
        unsigned from = 0;
        unsigned to = 0;
        auto *tmp = cast<GetElementPtrInst>(constantExpr->getAsInstruction());
        int precise = CacheModel::GEPInstPos(*tmp, from, to);
        delete tmp;
        PointerLocation loc = resolvePointer(base);
        loc.Offset += from;
        loc.Precise = loc.Precise && precise == 1;
        return loc;
      }
      if (constantExpr->isCast()) {
        return resolvePointer(constantExpr->getOperand(0));
      }
    }

    if (auto *bitCastInst = dyn_cast<BitCastInst>(value)) {
      PointerLocation loc = resolvePointer(bitCastInst->getOperand(0));
      PointerAliases[value] = loc;
      return loc;
    }

    if (auto *gep = dyn_cast<GetElementPtrInst>(value)) {
      unsigned from = 0;
      unsigned to = 0;
      int precise =
          CacheModel::GEPInstPos(const_cast<GetElementPtrInst &>(*gep), from, to);
      PointerLocation loc = resolvePointer(gep->getPointerOperand());
      loc.Offset += from;
      loc.Precise = loc.Precise && precise == 1;
      PointerAliases[value] = loc;
      return loc;
    }

    if (auto *phi = dyn_cast<PHINode>(value)) {
      recordPhi(*phi);
      return PointerAliases[value];
    }

    if (auto *selectInst = dyn_cast<SelectInst>(value)) {
      recordSelect(*selectInst);
      return PointerAliases[value];
    }

    auto objectIt = Objects.find(value);
    if (objectIt != Objects.end()) {
      return PointerLocation{value, 0, true};
    }

    if (value->getType()->isPointerTy()) {
      return resolveViaAliasAnalysis(value);
    }
    return {};
  }
};

class BranchSideSimulator {
public:
  BranchSideSimulator(CacheSpecuAnalysis &analysis, MemoryResolution &memory,
                      const SpeculationState &state)
      : Analysis(analysis), Memory(memory), State(state) {}

  std::pair<std::unique_ptr<CacheModel>, SpectreFinding>
  run(const BasicBlock *entry, const CacheModel &startState, bool thenSide) {
    std::map<const BasicBlock *, std::unique_ptr<CacheModel>> inStates;
    std::map<const BasicBlock *, std::unique_ptr<CacheModel>> outStates;
    std::map<const BasicBlock *, unsigned> visitBudget;
    std::deque<const BasicBlock *> worklist;
    worklist.push_back(entry);
    inStates[entry] = std::unique_ptr<CacheModel>(startState.fork());

    SpectreFinding finding;
    finding.Branch = State.Branch;
    finding.MergeBlock = State.MergeBlock;

    while (!worklist.empty()) {
      const BasicBlock *bb = worklist.front();
      worklist.pop_front();
      if (bb == State.MergeBlock) {
        continue;
      }
      if (++visitBudget[bb] > State.Budget) {
        continue;
      }

      CacheModel *inState = inStates[bb].get();
      if (inState == nullptr) {
        continue;
      }

      BlockSimulationResult blockResult = simulateBlock(*bb, *inState);
      outStates[bb] = std::move(blockResult.ExitState);
      if (thenSide) {
        finding.ExploredThenBlocks.push_back(bb);
      } else {
        finding.ExploredElseBlocks.push_back(bb);
      }

      if (blockResult.StopSpeculation) {
        continue;
      }

      for (const BasicBlock *succ : successors(bb)) {
        if (succ == State.MergeBlock) {
          continue;
        }
        if (inStates[succ] == nullptr) {
          inStates[succ] = std::unique_ptr<CacheModel>(outStates[bb]->fork());
          worklist.push_back(succ);
          continue;
        }

        std::unique_ptr<CacheModel> before(inStates[succ]->fork());
        inStates[succ]->merge(outStates[bb].get());
        if (Analysis.wideningOp(before.get(), inStates[succ].get()) ||
            !inStates[succ]->equal(before.get())) {
          worklist.push_back(succ);
        }
      }
    }

    std::unique_ptr<CacheModel> exitState(startState.fork());
    for (const auto &entryState : outStates) {
      exitState->merge(entryState.second.get());
    }

    if (thenSide) {
      finding.ThenObservations = std::move(CollectedObservations);
    } else {
      finding.ElseObservations = std::move(CollectedObservations);
    }
    return {std::move(exitState), std::move(finding)};
  }

private:
  CacheSpecuAnalysis &Analysis;
  MemoryResolution &Memory;
  const SpeculationState &State;
  SmallVector<SpectreObservation, 8> CollectedObservations;

  BlockSimulationResult simulateBlock(const BasicBlock &bb,
                                      const CacheModel &inState) {
    BlockSimulationResult result;
    result.ExitState = std::unique_ptr<CacheModel>(inState.fork());

    for (const Instruction &inst : bb) {
      AccessEffect effect = simulateInstruction(inst, *result.ExitState);
      if (effect.Observed) {
        CollectedObservations.push_back(effect.Observation);
      }
      if (isSpeculationBarrier(inst)) {
        result.StopSpeculation = true;
        break;
      }
    }

    return result;
  }

  AccessEffect simulateInstruction(const Instruction &inst, CacheModel &state) {
    if (const auto *loadInst = dyn_cast<LoadInst>(&inst)) {
      return observeResolvedAccess(
          Memory.resolveMemoryAccess(inst, loadInst->getPointerOperand(),
                                     AccessKind::Load, true, false),
          state, false);
    }
    if (const auto *storeInst = dyn_cast<StoreInst>(&inst)) {
      return observeResolvedAccess(
          Memory.resolveMemoryAccess(inst, storeInst->getPointerOperand(),
                                     AccessKind::Store, false, true),
          state, false);
    }
    if (const auto *callInst = dyn_cast<CallInst>(&inst)) {
      return observeCall(*callInst, state);
    }
    if (const auto *intrinsicInst = dyn_cast<IntrinsicInst>(&inst)) {
      return observeIntrinsic(*intrinsicInst, state);
    }
    return {};
  }

  AccessEffect observeCall(const CallInst &callInst, CacheModel &state) {
    AccessEffect effect;
    effect.Observation.Inst = &callInst;
    effect.Observation.FromCall = true;

    if (isSpeculationBarrier(callInst)) {
      return effect;
    }

    const Function *callee = callInst.getCalledFunction();
    if (callee != nullptr && callee->onlyReadsMemory()) {
      if (callInst.arg_empty()) {
        return effect;
      }
      if (callInst.getArgOperand(0)->getType()->isPointerTy()) {
        return observeResolvedAccess(
            Memory.resolveMemoryAccess(callInst, callInst.getArgOperand(0),
                                       AccessKind::Call, true, false),
            state, true);
      }
      return effect;
    }

    state.invalidateAll();
    effect.Observed = true;
    effect.Observation.ObjectName = "<unknown-call>";
    for (unsigned line = 0; line < state.CacheLineNum; ++line) {
      effect.Observation.CacheLines.push_back(line);
    }
    return effect;
  }

  AccessEffect observeIntrinsic(const IntrinsicInst &intrinsicInst,
                                CacheModel &state) {
    if (intrinsicInst.getIntrinsicID() == Intrinsic::memcpy ||
        intrinsicInst.getIntrinsicID() == Intrinsic::memmove) {
      return observeResolvedAccess(
          Memory.resolveMemoryAccess(intrinsicInst, intrinsicInst.getArgOperand(0),
                                     AccessKind::Intrinsic, true, true),
          state, false);
    }
    return {};
  }

  AccessEffect observeResolvedAccess(const ResolvedAccess &access,
                                     CacheModel &state, bool fromCall) {
    AccessEffect effect;
    effect.Observation.Inst = access.Inst;
    effect.Observation.MemoryObject = access.Base;
    effect.Observation.FromCall = fromCall;

    if (access.Base == nullptr) {
      state.invalidateAll();
      effect.Observed = true;
      effect.Observation.ObjectName = "<unknown>";
      for (unsigned line = 0; line < state.CacheLineNum; ++line) {
        effect.Observation.CacheLines.push_back(line);
      }
      return effect;
    }

    const AbstractMemoryObject *object = Memory.lookupObject(access.Base);
    effect.Observation.ObjectName =
        object != nullptr ? object->Name : "<unregistered>";

    unsigned startLine =
        state.LocateVar(const_cast<Value *>(access.Base), access.OffsetBegin);
    unsigned endLine =
        state.LocateVar(const_cast<Value *>(access.Base), access.OffsetEnd);
    if (startLine == static_cast<unsigned>(-1)) {
      state.invalidateAll();
      effect.Observed = true;
      return effect;
    }

    for (unsigned line = startLine; line <= endLine; ++line) {
      unsigned before = state.Ages[line];
      unsigned hit = state.Access(const_cast<Value *>(access.Base),
                                  static_cast<unsigned>(access.OffsetBegin));
      effect.Hit = hit != 0;
      if (before != state.Ages[line] || before >= state.CacheLinesPerSet) {
        effect.Observed = true;
      }
      effect.Observation.CacheLines.push_back(line % state.CacheLineNum);
    }
    return effect;
  }

  static bool isSpeculationBarrier(const Instruction &inst) {
    if (const auto *callInst = dyn_cast<CallInst>(&inst)) {
      const Function *callee = callInst->getCalledFunction();
      if (callee == nullptr) {
        return false;
      }
      StringRef name = callee->getName();
      return name.contains("lfence") || name.contains("speculation_safe_value") ||
             name.contains("spec.barrier");
    }
    return false;
  }
};

SmallVector<const BasicBlock *, 32> reversePostOrder(Function &F) {
  SmallVector<const BasicBlock *, 32> order;
  ReversePostOrderTraversal<Function *> traversal(&F);
  for (BasicBlock *bb : traversal) {
    order.push_back(bb);
  }
  return order;
}

std::set<std::pair<const BasicBlock *, const BasicBlock *>>
collectBackEdges(Function &F) {
  SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 8> edges;
  FindFunctionBackedges(F, edges);
  return std::set<std::pair<const BasicBlock *, const BasicBlock *>>(edges.begin(),
                                                                     edges.end());
}

bool sameFootprint(const SmallVectorImpl<SpectreObservation> &lhs,
                   const SmallVectorImpl<SpectreObservation> &rhs) {
  auto project = [](const SmallVectorImpl<SpectreObservation> &observations) {
    std::set<std::pair<std::string, unsigned>> footprint;
    for (const SpectreObservation &observation : observations) {
      for (unsigned line : observation.CacheLines) {
        footprint.emplace(observation.ObjectName, line);
      }
    }
    return footprint;
  };
  return project(lhs) == project(rhs);
}

} // namespace

CacheSpecuAnalysis::CacheSpecuAnalysis(Function &function, DominatorTree &domTree,
                                       PostDominatorTree &postDomTree,
                                       AliasAnalysis *aliasAnalysis,
                                       unsigned lineSize, unsigned lineNum,
                                       unsigned setNum, unsigned depth,
                                       unsigned merge)
    : F(&function), DT(&domTree), PDT(&postDomTree), AA(aliasAnalysis),
      model(new CacheModel(lineSize, lineNum, setNum == 0 ? 1 : setNum)),
      SpeculationDepth(depth == 0 ? 1 : depth), MergeOption(merge),
      RunSpeculation(depth > 0), cacheChanged(false) {}

bool CacheSpecuAnalysis::wideningOp(CacheModel *last, CacheModel *current) {
  if (last == nullptr || current == nullptr) {
    return false;
  }
  if (!last->ConfigConsistent(current)) {
    errs() << "Fatal: cache configuration inconsistent when widening!";
    return false;
  }
  return current->widenFrom(*last);
}

bool CacheSpecuAnalysis::SpecuSim(BasicBlock *from, BasicBlock *to,
                                  CacheModel *init) {
  (void)to;
  Result = SpectreAnalysisResult{};
  ArchitecturalInStates.clear();
  ArchitecturalOutStates.clear();

  if (from == nullptr) {
    from = &F->getEntryBlock();
  }

  std::unique_ptr<CacheModel> ownedInitial(init != nullptr ? init->fork()
                                                           : model->fork());
  model = ownedInitial.get();
  InitModel();

  MemoryResolution memory(*F, AA, *model);
  memory.seedObjects();
  ownedInitial.reset(model->fork());

  std::set<std::pair<const BasicBlock *, const BasicBlock *>> backEdges =
      collectBackEdges(*F);
  SmallVector<const BasicBlock *, 32> order = reversePostOrder(*F);

  bool changed = true;
  unsigned iterations = 0;
  while (changed && ++iterations <= 8) {
    changed = false;
    for (const BasicBlock *bb : order) {
      std::unique_ptr<CacheModel> inState;
      if (bb == from) {
        inState.reset(ownedInitial->fork());
      } else {
        for (const BasicBlock *pred : predecessors(bb)) {
          auto predOut = ArchitecturalOutStates.find(pred);
          if (predOut == ArchitecturalOutStates.end()) {
            continue;
          }
          if (!inState) {
            inState.reset(predOut->second->fork());
          } else {
            inState->merge(predOut->second);
          }
        }
      }
      if (!inState) {
        continue;
      }

      auto existingIn = ArchitecturalInStates.find(bb);
      if (existingIn != ArchitecturalInStates.end()) {
        std::unique_ptr<CacheModel> before(inState->fork());
        for (const BasicBlock *pred : predecessors(bb)) {
          if (backEdges.count({pred, bb}) != 0) {
            wideningOp(before.get(), inState.get());
            break;
          }
        }
      }

      ArchitecturalInStates[bb] = inState->fork();

      model = inState.get();
      for (Instruction &inst : *const_cast<BasicBlock *>(bb)) {
        visit(inst);
      }

      auto existingOut = ArchitecturalOutStates.find(bb);
      if (existingOut == ArchitecturalOutStates.end() ||
          !existingOut->second->equal(model)) {
        changed = true;
        ArchitecturalOutStates[bb] = model->fork();
      }
    }
  }

  Result.ArchitecturalHits = model->HitCount;
  Result.ArchitecturalMisses = model->MissCount;

  if (!RunSpeculation) {
    return true;
  }

  for (BasicBlock &bb : *F) {
    auto *branch = dyn_cast<BranchInst>(bb.getTerminator());
    if (branch == nullptr || branch->isUnconditional()) {
      continue;
    }

    BasicBlock *thenBB = branch->getSuccessor(0);
    BasicBlock *elseBB = branch->getSuccessor(1);
    BasicBlock *mergeBB = PDT->findNearestCommonDominator(thenBB, elseBB);
    if (mergeBB == nullptr) {
      continue;
    }

    CacheModel *branchState = nullptr;
    auto outIt = ArchitecturalOutStates.find(&bb);
    if (outIt != ArchitecturalOutStates.end()) {
      branchState = outIt->second;
    } else {
      branchState = ownedInitial.get();
    }

    SpeculationState speculation{branch, mergeBB, SpeculationDepth};
    BranchSideSimulator thenSimulator(*this, memory, speculation);
    auto thenRun = thenSimulator.run(thenBB, *branchState, true);
    BranchSideSimulator elseSimulator(*this, memory, speculation);
    auto elseRun = elseSimulator.run(elseBB, *branchState, false);

    SpectreFinding finding;
    finding.Branch = branch;
    finding.MergeBlock = mergeBB;
    finding.ExploredThenBlocks = std::move(thenRun.second.ExploredThenBlocks);
    finding.ExploredElseBlocks = std::move(elseRun.second.ExploredElseBlocks);
    finding.ThenObservations = std::move(thenRun.second.ThenObservations);
    finding.ElseObservations = std::move(elseRun.second.ElseObservations);
    finding.HasDivergence =
        !sameFootprint(finding.ThenObservations, finding.ElseObservations);

    for (SpectreObservation &observation : finding.ThenObservations) {
      observation.DivergesFromArchitectural = finding.HasDivergence;
      Result.SpeculativeMisses += observation.CacheLines.empty() ? 0 : 1;
    }
    for (SpectreObservation &observation : finding.ElseObservations) {
      observation.DivergesFromArchitectural = finding.HasDivergence;
      Result.SpeculativeMisses += observation.CacheLines.empty() ? 0 : 1;
    }

    Result.SpeculativeHits +=
        static_cast<unsigned>(finding.ThenObservations.size() +
                              finding.ElseObservations.size());

    if (finding.HasDivergence) {
      Result.Findings.push_back(std::move(finding));
    }
  }

  return true;
}

bool CacheSpecuAnalysis::IsValueInCache(Instruction *inst) {
  if (auto *load = dyn_cast<LoadInst>(inst)) {
    GlobalVariable *global = nullptr;
    unsigned begin = 0;
    unsigned end = 0;
    if (!GetInstCacheRange(load->getPointerOperand(), global, begin, end)) {
      return false;
    }

    unsigned startLine = model->LocateVar(global, begin);
    unsigned endLine = model->LocateVar(global, end);
    if (startLine == static_cast<unsigned>(-1)) {
      return false;
    }

    for (unsigned line = startLine; line <= endLine; ++line) {
      if (model->Ages[line] >= model->CacheLinesPerSet) {
        return false;
      }
    }
    return true;
  }
  return false;
}

bool CacheSpecuAnalysis::GetInstCacheRange(Value *inst, GlobalVariable *&GV,
                                           unsigned &offset_b,
                                           unsigned &offset_e) {
  GV = nullptr;
  offset_b = 0;
  offset_e = 0;

  if (auto *gep = dyn_cast<GetElementPtrInst>(inst)) {
    Value *base = gep->getPointerOperand();
    if (auto *global = dyn_cast<GlobalVariable>(base)) {
      GV = global;
      int precise = CacheModel::GEPInstPos(*gep, offset_b, offset_e);
      return precise != -1;
    }
  }

  if (auto *global = dyn_cast<GlobalVariable>(inst)) {
    GV = global;
    unsigned size = CacheModel::GetTySize(global->getValueType());
    offset_e = size == 0 ? 0 : size - 1;
    return true;
  }

  return false;
}

std::vector<Value *> CacheSpecuAnalysis::GetAlias(Value *val, unsigned offset) {
  (void)offset;
  std::vector<Value *> aliases;
  if (AA == nullptr || !val->getType()->isPointerTy()) {
    return aliases;
  }

  for (const auto &entry : model->Vars) {
    if (!entry.first->getType()->isPointerTy()) {
      continue;
    }
    if (AA->alias(val, entry.first) != AliasResult::NoAlias) {
      aliases.push_back(entry.first);
    }
  }
  return aliases;
}

void CacheSpecuAnalysis::InitModel() {
  unsigned argIndex = 0;
  for (Argument &arg : F->args()) {
    ++argIndex;
    model->AddVar(&arg, arg.getType(), F->getParamAlignment(argIndex));
  }

  Module *module = F->getParent();
  for (GlobalVariable &global : module->globals()) {
    model->AddVar(&global, global.getValueType(), global.getAlignment());
  }
}

void CacheSpecuAnalysis::InitModel(GlobalVariable *var, unsigned b, unsigned e) {
  if (var == nullptr) {
    return;
  }
  model->AddVar(var, var->getValueType(), var->getAlignment());
  model->SetAge(var, 0, b, e);
}

void CacheSpecuAnalysis::ExtractGEPC(ConstantExpr *source, Value *&target,
                                     unsigned &offset) {
  target = source;
  offset = 0;
  if (auto *gep = dyn_cast<GEPOperator>(source)) {
    auto *tmp = cast<GetElementPtrInst>(source->getAsInstruction());
    unsigned from = 0;
    unsigned to = 0;
    CacheModel::GEPInstPos(*tmp, from, to);
    offset = from;
    target = gep->getPointerOperand();
    delete tmp;
  } else if (source->isCast()) {
    target = source->getOperand(0);
  }
}

void CacheSpecuAnalysis::visitAllocaInst(AllocaInst &I) {
  cacheChanged = true;
  model->AddVar(&I, I.getAllocatedType(), I.getAlignment());
}

void CacheSpecuAnalysis::visitLoadInst(LoadInst &I) {
  cacheChanged = true;
  if (!I.getPointerOperand()->getType()->isPointerTy()) {
    return;
  }

  unsigned hit = 1;
  std::vector<Value *> aliases = GetAlias(I.getPointerOperand());
  if (!aliases.empty()) {
    hit = model->Access(aliases.front(), static_cast<unsigned>(0));
  } else if (model->Vars.find(I.getPointerOperand()) != model->Vars.end()) {
    hit = model->Access(I.getPointerOperand(), static_cast<unsigned>(0));
  }

  if (hit == 0) {
    model->MissCount++;
  } else {
    model->HitCount++;
  }
}

void CacheSpecuAnalysis::visitBitCastInst(BitCastInst &I) { cacheChanged = false; }

void CacheSpecuAnalysis::visitStoreInst(StoreInst &I) {
  cacheChanged = true;
  unsigned hit = 1;
  std::vector<Value *> aliases = GetAlias(I.getPointerOperand());
  if (!aliases.empty()) {
    hit = model->Access(aliases.front(), static_cast<unsigned>(0));
  } else if (model->Vars.find(I.getPointerOperand()) != model->Vars.end()) {
    hit = model->Access(I.getPointerOperand(), static_cast<unsigned>(0));
  }

  if (hit == 0) {
    model->MissCount++;
  } else {
    model->HitCount++;
  }
}

void CacheSpecuAnalysis::visitCallInst(CallInst &I) {
  cacheChanged = true;
  Function *callee = I.getCalledFunction();
  if (callee != nullptr && callee->onlyReadsMemory() && I.arg_size() > 0 &&
      I.getArgOperand(0)->getType()->isPointerTy()) {
    if (model->Vars.find(I.getArgOperand(0)) != model->Vars.end()) {
      unsigned hit =
          model->Access(I.getArgOperand(0), static_cast<unsigned>(0));
      if (hit == 0) {
        model->MissCount++;
      } else {
        model->HitCount++;
      }
      return;
    }
  }

  model->invalidateAll();
  model->MissCount++;
}

void CacheSpecuAnalysis::visitPHINode(PHINode &I) { cacheChanged = false; }

void CacheSpecuAnalysis::visitSelectInst(SelectInst &I) { cacheChanged = false; }

void CacheSpecuAnalysis::visitIntrinsicInst(IntrinsicInst &I) {
  cacheChanged = true;
  if (I.getIntrinsicID() == Intrinsic::memcpy ||
      I.getIntrinsicID() == Intrinsic::memmove) {
    if (I.getArgOperand(0)->getType()->isPointerTy() &&
        model->Vars.find(I.getArgOperand(0)) != model->Vars.end()) {
      unsigned hit =
          model->Access(I.getArgOperand(0), static_cast<unsigned>(0));
      if (hit == 0) {
        model->MissCount++;
      } else {
        model->HitCount++;
      }
    }
  }
}

void CacheSpecuAnalysis::visitVACopyInst(VACopyInst &I) { (void)I; }

void CacheSpecuAnalysis::visitBranchInst(BranchInst &I) { (void)I; }

void CacheSpecuAnalysis::visitGetElementPtrInst(GetElementPtrInst &I) {
  cacheChanged = false;
  Value *base = I.getPointerOperand();
  if (model->Vars.find(base) == model->Vars.end() && isa<GlobalVariable>(base)) {
    auto *global = cast<GlobalVariable>(base);
    model->AddVar(global, global->getValueType(), global->getAlignment());
  }
}

void CacheSpecuAnalysis::visitInstruction(Instruction &I) { (void)I; }

void CacheSpecuAnalysis::dump(int mod) {
  if (mod == 0) {
    Result.dump(dbgs());
    return;
  }
  model->dump(mod > 1);
}

} // namespace spectre
