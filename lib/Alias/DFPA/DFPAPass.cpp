#include "Alias/DFPA/DFPAPass.h"

#include "Alias/DFPA/ProgramIndex.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace dfpa;

namespace {

struct AbstractValue {
  DFPATargetSet funcs;
  std::set<SlotKey> slots;
  bool unknown_callable = false;
  bool unknown_pointer = false;
  bool from_unknown_slot = false;

  bool mergeFrom(const AbstractValue &Other) {
    bool Changed = false;
    auto OldFuncs = funcs.size();
    auto OldSlots = slots.size();
    funcs.insert(Other.funcs.begin(), Other.funcs.end());
    slots.insert(Other.slots.begin(), Other.slots.end());
    Changed |= funcs.size() != OldFuncs;
    Changed |= slots.size() != OldSlots;
    if (!unknown_callable && Other.unknown_callable) {
      unknown_callable = true;
      Changed = true;
    }
    if (!unknown_pointer && Other.unknown_pointer) {
      unknown_pointer = true;
      Changed = true;
    }
    if (!from_unknown_slot && Other.from_unknown_slot) {
      from_unknown_slot = true;
      Changed = true;
    }
    return Changed;
  }
};

struct FunctionSummary {
  std::vector<AbstractValue> params;
  AbstractValue ret;
  std::map<unsigned, std::map<SlotKey, AbstractValue>> arg_writes;
};

struct PointerTargets {
  std::set<SlotKey> slots;
  bool unknown = false;

  bool mergeFrom(const PointerTargets &Other) {
    auto Old = slots.size();
    slots.insert(Other.slots.begin(), Other.slots.end());
    bool Changed = slots.size() != Old;
    if (!unknown && Other.unknown) {
      unknown = true;
      Changed = true;
    }
    return Changed;
  }
};

struct ContextKey {
  std::vector<const CallBase *> indirect_stack;

  void push(const CallBase *CB, unsigned K) {
    indirect_stack.push_back(CB);
    if (K == 0) {
      indirect_stack.clear();
      return;
    }
    while (indirect_stack.size() > K)
      indirect_stack.erase(indirect_stack.begin());
  }

  bool operator<(const ContextKey &Other) const {
    return indirect_stack < Other.indirect_stack;
  }
};

struct DemandValueKey {
  const Value *V = nullptr;
  ContextKey Ctx;
  const CallBase *DirectBinding = nullptr;

  bool operator<(const DemandValueKey &Other) const {
    if (V != Other.V)
      return V < Other.V;
    if (Ctx < Other.Ctx)
      return true;
    if (Other.Ctx < Ctx)
      return false;
    return DirectBinding < Other.DirectBinding;
  }
};

struct DemandSlotKey {
  SlotKey Slot;
  ContextKey Ctx;
  const CallBase *DirectBinding = nullptr;

  bool operator<(const DemandSlotKey &Other) const {
    if (Slot < Other.Slot)
      return true;
    if (Other.Slot < Slot)
      return false;
    if (Ctx < Other.Ctx)
      return true;
    if (Other.Ctx < Ctx)
      return false;
    return DirectBinding < Other.DirectBinding;
  }
};

static std::string getLocationString(const CallBase *CB) {
  if (!CB || !CB->getDebugLoc())
    return "<unknown>:0:0";
  const DebugLoc &Loc = CB->getDebugLoc();
  std::string File = Loc->getFilename().str();
  return File + ":" + std::to_string(Loc.getLine()) + ":" +
         std::to_string(Loc.getCol());
}

static bool isMemcpyLike(const Function *F) {
  return F && (F->getName().startswith("llvm.memcpy") ||
               F->getName().startswith("llvm.memmove"));
}

static bool isHeapAllocator(const CallBase *CB) {
  auto *Callee = CB->getCalledFunction();
  if (!Callee)
    return false;
  StringRef Name = Callee->getName();
  return Name == "malloc" || Name == "calloc" || Name == "realloc" ||
         Name == "_Znwm" || Name == "_Znam";
}

static bool isFunctionPointerTy(Type *Ty) {
  if (!Ty || !Ty->isPointerTy())
    return false;
  return Ty->getPointerElementType()->isFunctionTy();
}

class DFPAAnalyzer {
public:
  DFPAAnalyzer(Module &M, const DFPAConfig &Config)
      : module_(M), dl_(M.getDataLayout()), index_(M), config_(Config) {}

  DFPAResult run() {
    initializeSummaries();
    runCoarseFixpoint();
    materializeResults();
    runDemandRefinement();
    return result_;
  }

private:
  Module &module_;
  const DataLayout &dl_;
  ProgramIndex index_;
  DFPAConfig config_;
  DFPAResult result_;
  std::map<Function *, FunctionSummary> summaries_;
  std::map<SlotKey, AbstractValue> slot_state_;
  std::map<const CallBase *, DFPATargetSet> coarse_targets_;
  std::set<const CallBase *> coarse_unknown_;
  std::set<const CallBase *> coarse_missing_targets_;
  std::size_t unknown_slot_degradations_ = 0;
  std::size_t demand_steps_ = 0;
  std::map<unsigned, std::vector<StoreInst *>> stores_by_object_;
  std::vector<StoreInst *> stores_with_unknown_base_;
  std::map<unsigned, std::vector<CallBase *>> memtransfers_by_dst_object_;
  std::vector<CallBase *> memtransfers_with_unknown_base_;

  struct LocalState {
    std::map<Value *, AbstractValue> value_cache;
    std::set<Value *> eval_visiting;
    std::set<Value *> ptr_visiting;
  };

  void initializeSummaries() {
    for (Function &F : module_) {
      FunctionSummary Summary;
      Summary.params.resize(F.arg_size());
      summaries_[&F] = Summary;
    }
    seedGlobalInitializers();
  }

  void buildDemandIndexes() {
    stores_by_object_.clear();
    stores_with_unknown_base_.clear();
    memtransfers_by_dst_object_.clear();
    memtransfers_with_unknown_base_.clear();

    for (StoreInst *SI : index_.getStores()) {
      std::set<unsigned> BaseObjects;
      bool UnknownBase = false;
      collectSyntacticBaseObjects(SI->getPointerOperand(), BaseObjects,
                                  UnknownBase);
      if (BaseObjects.empty() || UnknownBase) {
        stores_with_unknown_base_.push_back(SI);
      }
      for (unsigned ObjectId : BaseObjects) {
        stores_by_object_[ObjectId].push_back(SI);
      }
    }

    for (CallBase *CB : index_.getMemTransfers()) {
      std::set<unsigned> BaseObjects;
      bool UnknownBase = false;
      collectSyntacticBaseObjects(CB->getArgOperand(0), BaseObjects,
                                  UnknownBase);
      if (BaseObjects.empty() || UnknownBase) {
        memtransfers_with_unknown_base_.push_back(CB);
      }
      for (unsigned ObjectId : BaseObjects) {
        memtransfers_by_dst_object_[ObjectId].push_back(CB);
      }
    }
  }

  void collectSyntacticBaseObjects(Value *V, std::set<unsigned> &Objects,
                                   bool &UnknownBase) const {
    if (!V)
      return;

    V = V->stripPointerCasts();
    if (const AbstractObject *Obj = index_.lookupObject(V)) {
      Objects.insert(Obj->id);
      return;
    }
    if (auto *Arg = dyn_cast<Argument>(V)) {
      if (const AbstractObject *Obj = index_.lookupFormalObject(Arg)) {
        Objects.insert(Obj->id);
        return;
      }
      UnknownBase = true;
      return;
    }
    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      collectSyntacticBaseObjects(GEP->getPointerOperand(), Objects,
                                  UnknownBase);
      return;
    }
    if (auto *BCO = dyn_cast<BitCastOperator>(V)) {
      collectSyntacticBaseObjects(BCO->getOperand(0), Objects, UnknownBase);
      return;
    }
    if (auto *BCI = dyn_cast<BitCastInst>(V)) {
      collectSyntacticBaseObjects(BCI->getOperand(0), Objects, UnknownBase);
      return;
    }
    if (auto *PN = dyn_cast<PHINode>(V)) {
      for (unsigned I = 0; I < PN->getNumIncomingValues(); ++I) {
        collectSyntacticBaseObjects(PN->getIncomingValue(I), Objects,
                                    UnknownBase);
      }
      return;
    }
    if (auto *SI = dyn_cast<SelectInst>(V)) {
      collectSyntacticBaseObjects(SI->getTrueValue(), Objects, UnknownBase);
      collectSyntacticBaseObjects(SI->getFalseValue(), Objects, UnknownBase);
      return;
    }

    UnknownBase = true;
  }

  void seedGlobalInitializers() {
    for (GlobalVariable *GV : index_.getGlobalsWithInitializers()) {
      if (!GV || !GV->hasInitializer())
        continue;
      const AbstractObject *Obj = index_.lookupObject(GV);
      if (!Obj)
        continue;
      std::set<const Constant *> Visited;
      seedConstantIntoSlot(GV->getInitializer(), {Obj->id, 0, false}, Visited);
    }
  }

  void seedConstantIntoSlot(Constant *C, const SlotKey &BaseSlot,
                            std::set<const Constant *> &Visited) {
    if (!C || Visited.count(C))
      return;
    Visited.insert(C);

    if (Function *F = dyn_cast<Function>(C)) {
      slot_state_[BaseSlot].funcs.insert(F);
      return;
    }
    if (auto *GV = dyn_cast<GlobalVariable>(C)) {
      const AbstractObject *Obj = index_.lookupObject(GV);
      if (Obj)
        slot_state_[BaseSlot].slots.insert({Obj->id, 0, false});
      return;
    }
    if (auto *CE = dyn_cast<ConstantExpr>(C)) {
      if (CE->isCast())
        return seedConstantIntoSlot(cast<Constant>(CE->getOperand(0)), BaseSlot,
                                    Visited);
      if (CE->getOpcode() == Instruction::GetElementPtr) {
        APInt Offset(dl_.getPointerSizeInBits(), 0);
        if (auto *GEP = dyn_cast<GEPOperator>(CE)) {
          if (GEP->accumulateConstantOffset(dl_, Offset)) {
            SlotKey Next = BaseSlot;
            if (!Next.unknown)
              Next.offset += Offset.getSExtValue();
            seedConstantIntoSlot(cast<Constant>(CE->getOperand(0)), Next,
                                 Visited);
          } else {
            SlotKey Next = BaseSlot;
            Next.unknown = true;
            slot_state_[Next].from_unknown_slot = true;
            ++unknown_slot_degradations_;
          }
        } else {
          SlotKey Next = BaseSlot;
          Next.unknown = true;
          slot_state_[Next].from_unknown_slot = true;
          ++unknown_slot_degradations_;
        }
      }
      return;
    }
    if (auto *Agg = dyn_cast<ConstantAggregate>(C)) {
      for (unsigned I = 0; I < Agg->getNumOperands(); ++I) {
        Constant *Op = cast<Constant>(Agg->getOperand(I));
        SlotKey FieldSlot = BaseSlot;
        if (StructType *STy = dyn_cast<StructType>(Agg->getType())) {
          const StructLayout *SL = dl_.getStructLayout(STy);
          FieldSlot.offset += SL->getElementOffset(I);
        }
        seedConstantIntoSlot(Op, FieldSlot, Visited);
      }
      return;
    }
    if (isa<ConstantPointerNull>(C))
      return;
    slot_state_[BaseSlot].from_unknown_slot = true;
    ++unknown_slot_degradations_;
  }

  void runCoarseFixpoint() {
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (Function &F : module_) {
        if (F.isDeclaration())
          continue;
        Changed |= analyzeFunction(F);
      }
    }
  }

  bool analyzeFunction(Function &F) {
    bool Changed = false;
    auto &Summary = summaries_[&F];

    bool LocalChanged = true;
    unsigned Iteration = 0;
    while (LocalChanged && Iteration++ < 8) {
      LocalState State;
      for (Argument &Arg : F.args()) {
        if (isFunctionPointerTy(Arg.getType())) {
          State.value_cache[&Arg] = Summary.params[Arg.getArgNo()];
          continue;
        }
        if (Arg.getType()->isPointerTy()) {
          const AbstractObject *Obj = index_.lookupFormalObject(&Arg);
          if (Obj)
            State.value_cache[&Arg].slots.insert({Obj->id, 0, false});
        }
      }

      LocalChanged = false;
      for (Instruction &I : instructions(F)) {
        if (auto *SI = dyn_cast<StoreInst>(&I)) {
          LocalChanged |= handleStore(*SI, F, State);
          continue;
        }
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          LocalChanged |= handleCall(*CB, F, State);
          continue;
        }
        if (isa<PHINode>(I) || isa<SelectInst>(I) || isa<CastInst>(I) ||
            isa<LoadInst>(I)) {
          AbstractValue V = evaluateValue(&I, F, State);
          LocalChanged |= State.value_cache[&I].mergeFrom(V);
        }
      }

      AbstractValue Ret;
      for (ReturnInst *RI : index_.getReturns().at(&F)) {
        if (Value *RV = RI->getReturnValue())
          Ret.mergeFrom(evaluateValue(RV, F, State));
      }
      Changed |= summaries_[&F].ret.mergeFrom(Ret);
    }
    return Changed || LocalChanged;
  }

  AbstractValue evaluateValue(Value *V, Function &Current, LocalState &State) {
    auto Cached = State.value_cache.find(V);
    if (Cached != State.value_cache.end())
      return Cached->second;

    if (State.eval_visiting.count(V))
      return AbstractValue();
    State.eval_visiting.insert(V);

    AbstractValue Result;
    if (auto *F = dyn_cast<Function>(V)) {
      if (!F->isIntrinsic())
        Result.funcs.insert(F);
    } else if (isa<AllocaInst>(V) || isa<GlobalVariable>(V) ||
               isa<GetElementPtrInst>(V) || isa<GEPOperator>(V) ||
               isa<BitCastInst>(V) || isa<BitCastOperator>(V) ||
               isa<Argument>(V) || isa<CallBase>(V)) {
      PointerTargets Ptrs = resolvePointerTargets(V, Current, State);
      Result.slots.insert(Ptrs.slots.begin(), Ptrs.slots.end());
      Result.unknown_pointer = Ptrs.unknown;
    }

    if (auto *BCO = dyn_cast<BitCastOperator>(V)) {
      Result.mergeFrom(evaluateValue(BCO->getOperand(0), Current, State));
    } else if (auto *CI = dyn_cast<CastInst>(V)) {
      Result.mergeFrom(evaluateValue(CI->getOperand(0), Current, State));
      if (isa<PtrToIntInst>(CI) || isa<IntToPtrInst>(CI)) {
        Result.unknown_callable = true;
        Result.unknown_pointer = true;
      }
    } else if (auto *PIO = dyn_cast<PtrToIntOperator>(V)) {
      Result.mergeFrom(evaluateValue(PIO->getOperand(0), Current, State));
      Result.unknown_callable = true;
      Result.unknown_pointer = true;
    } else if (auto *ITO = dyn_cast<IntToPtrInst>(V)) {
      Result.mergeFrom(evaluateValue(ITO->getOperand(0), Current, State));
      Result.unknown_callable = true;
      Result.unknown_pointer = true;
    } else if (auto *PN = dyn_cast<PHINode>(V)) {
      for (unsigned I = 0; I < PN->getNumIncomingValues(); ++I)
        Result.mergeFrom(
            evaluateValue(PN->getIncomingValue(I), Current, State));
    } else if (auto *SI = dyn_cast<SelectInst>(V)) {
      Result.mergeFrom(evaluateValue(SI->getTrueValue(), Current, State));
      Result.mergeFrom(evaluateValue(SI->getFalseValue(), Current, State));
    } else if (auto *LI = dyn_cast<LoadInst>(V)) {
      PointerTargets Ptrs = resolvePointerTargets(LI->getPointerOperand(),
                                                 Current, State);
      for (const SlotKey &Slot : Ptrs.slots) {
        auto It = slot_state_.find(Slot);
        if (It != slot_state_.end())
          Result.mergeFrom(It->second);
      }
      if (Ptrs.unknown) {
        Result.unknown_callable = true;
        Result.unknown_pointer = true;
        Result.from_unknown_slot = true;
      }
    } else if (auto *CB = dyn_cast<CallBase>(V)) {
      if (CB->isIndirectCall()) {
        Result.unknown_callable = isFunctionPointerTy(CB->getType());
        Result.unknown_pointer = CB->getType()->isPointerTy();
      } else if (Function *Callee = CB->getCalledFunction()) {
        if (isHeapAllocator(CB)) {
          PointerTargets Ptrs = resolvePointerTargets(CB, Current, State);
          Result.slots.insert(Ptrs.slots.begin(), Ptrs.slots.end());
        } else if (!Callee->isDeclaration() && !Callee->isIntrinsic()) {
          Result.mergeFrom(summaries_[Callee].ret);
        } else if (CB->getType()->isPointerTy()) {
          Result.unknown_callable = isFunctionPointerTy(CB->getType());
          Result.unknown_pointer = true;
        }
      }
    }

    State.eval_visiting.erase(V);
    State.value_cache[V] = Result;
    return Result;
  }

  PointerTargets resolvePointerTargets(Value *V, Function &Current,
                                       LocalState &State) {
    if (State.ptr_visiting.count(V))
      return PointerTargets();
    State.ptr_visiting.insert(V);

    PointerTargets Targets;
    if (auto *Obj = index_.lookupObject(V)) {
      Targets.slots.insert({Obj->id, 0, false});
      State.ptr_visiting.erase(V);
      return Targets;
    }

    if (auto *Arg = dyn_cast<Argument>(V)) {
      if (const AbstractObject *Obj = index_.lookupFormalObject(Arg))
        Targets.slots.insert({Obj->id, 0, false});
      State.ptr_visiting.erase(V);
      return Targets;
    }

    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      Targets = resolvePointerTargets(GEP->getPointerOperand(), Current, State);
      APInt Offset(dl_.getPointerSizeInBits(), 0);
      if (GEP->accumulateConstantOffset(dl_, Offset)) {
        std::set<SlotKey> Adjusted;
        for (SlotKey Slot : Targets.slots) {
          if (Slot.unknown)
            Adjusted.insert(Slot);
          else {
            Slot.offset += Offset.getSExtValue();
            Adjusted.insert(Slot);
          }
        }
        Targets.slots.swap(Adjusted);
      } else {
        std::set<SlotKey> Adjusted;
        for (SlotKey Slot : Targets.slots) {
          Slot.unknown = true;
          Adjusted.insert(Slot);
        }
        Targets.slots.swap(Adjusted);
        Targets.unknown = true;
      }
      State.ptr_visiting.erase(V);
      return Targets;
    }

    if (auto *BCO = dyn_cast<BitCastOperator>(V)) {
      Targets = resolvePointerTargets(BCO->getOperand(0), Current, State);
      State.ptr_visiting.erase(V);
      return Targets;
    }
    if (auto *BCI = dyn_cast<BitCastInst>(V)) {
      Targets = resolvePointerTargets(BCI->getOperand(0), Current, State);
      State.ptr_visiting.erase(V);
      return Targets;
    }
    if (auto *PN = dyn_cast<PHINode>(V)) {
      for (unsigned I = 0; I < PN->getNumIncomingValues(); ++I)
        Targets.mergeFrom(
            resolvePointerTargets(PN->getIncomingValue(I), Current, State));
      State.ptr_visiting.erase(V);
      return Targets;
    }
    if (auto *SI = dyn_cast<SelectInst>(V)) {
      Targets.mergeFrom(resolvePointerTargets(SI->getTrueValue(), Current, State));
      Targets.mergeFrom(
          resolvePointerTargets(SI->getFalseValue(), Current, State));
      State.ptr_visiting.erase(V);
      return Targets;
    }
    if (auto *LI = dyn_cast<LoadInst>(V)) {
      AbstractValue Loaded = evaluateValue(LI, Current, State);
      Targets.slots.insert(Loaded.slots.begin(), Loaded.slots.end());
      Targets.unknown = Loaded.unknown_pointer || Loaded.from_unknown_slot;
      State.ptr_visiting.erase(V);
      return Targets;
    }
    if (auto *CB = dyn_cast<CallBase>(V)) {
      if (isHeapAllocator(CB)) {
        if (const AbstractObject *Obj = index_.lookupObject(CB))
          Targets.slots.insert({Obj->id, 0, false});
      } else if (!CB->isIndirectCall()) {
        Function *Callee = CB->getCalledFunction();
        if (!Callee || Callee->isDeclaration())
          Targets.unknown = true;
        else {
          AbstractValue Ret = summaries_[Callee].ret;
          Targets.slots.insert(Ret.slots.begin(), Ret.slots.end());
          Targets.unknown = Ret.unknown_pointer;
        }
      } else {
        Targets.unknown = true;
      }
      State.ptr_visiting.erase(V);
      return Targets;
    }

    Targets.unknown = true;
    State.ptr_visiting.erase(V);
    return Targets;
  }

  bool handleStore(StoreInst &SI, Function &Current, LocalState &State) {
    PointerTargets Ptrs =
        resolvePointerTargets(SI.getPointerOperand(), Current, State);
    AbstractValue Value = evaluateValue(SI.getValueOperand(), Current, State);
    bool Changed = false;

    if (Ptrs.unknown)
      ++unknown_slot_degradations_;

    for (const SlotKey &Slot : Ptrs.slots) {
      AbstractValue Stored = Value;
      if (Slot.unknown)
        Stored.from_unknown_slot = true;
      Changed |= slot_state_[Slot].mergeFrom(Stored);

      if (const Argument *Arg =
              dyn_cast<Argument>(stripToBaseObject(SI.getPointerOperand()))) {
        auto &Summary = summaries_[&Current];
        Changed |= Summary.arg_writes[Arg->getArgNo()][Slot].mergeFrom(Stored);
      }
    }
    return Changed;
  }

  bool handleCall(CallBase &CB, Function &Current, LocalState &State) {
    bool Changed = false;
    if (Function *Callee = CB.getCalledFunction()) {
      if (isMemcpyLike(Callee) && CB.arg_size() >= 2) {
        PointerTargets Dst =
            resolvePointerTargets(CB.getArgOperand(0), Current, State);
        PointerTargets Src =
            resolvePointerTargets(CB.getArgOperand(1), Current, State);
        for (const SlotKey &DstSlot : Dst.slots) {
          for (const SlotKey &SrcSlot : Src.slots) {
            auto It = slot_state_.find(SrcSlot);
            if (It != slot_state_.end())
              Changed |= slot_state_[DstSlot].mergeFrom(It->second);
          }
          if (Src.unknown) {
            slot_state_[DstSlot].from_unknown_slot = true;
            ++unknown_slot_degradations_;
            Changed = true;
          }
        }
        return Changed;
      }

      if (!Callee->isDeclaration()) {
        auto &Summary = summaries_[Callee];
        unsigned ArgNo = 0;
        for (Value *ArgV : CB.args()) {
          if (ArgNo >= Summary.params.size())
            break;
          Changed |= Summary.params[ArgNo].mergeFrom(
              evaluateValue(ArgV, Current, State));
          ++ArgNo;
        }

        for (const auto &ArgWrites : Summary.arg_writes) {
          if (ArgWrites.first >= CB.arg_size())
            continue;
          PointerTargets Actual =
              resolvePointerTargets(CB.getArgOperand(ArgWrites.first), Current,
                                    State);
          for (const auto &Write : ArgWrites.second) {
            for (const SlotKey &ActualSlot : Actual.slots) {
              SlotKey Mapped = ActualSlot;
              if (!Write.first.unknown)
                Mapped.offset += Write.first.offset;
              else
                Mapped.unknown = true;
              Changed |= slot_state_[Mapped].mergeFrom(Write.second);
            }
          }
        }

        Changed |= applyDirectCalleeStores(CB, *Callee, Current, State);

        if (CB.getType()->isPointerTy()) {
          AbstractValue Ret = Summary.ret;
          Changed |= State.value_cache[&CB].mergeFrom(Ret);
        }
      } else if (CB.getType()->isPointerTy()) {
        AbstractValue Unknown;
        Unknown.unknown_pointer = true;
        Unknown.unknown_callable = isFunctionPointerTy(CB.getType());
        Changed |= State.value_cache[&CB].mergeFrom(Unknown);
      }
    } else if (CB.getType()->isPointerTy()) {
      AbstractValue Unknown;
      Unknown.unknown_callable = isFunctionPointerTy(CB.getType());
      Unknown.unknown_pointer = true;
      Changed |= State.value_cache[&CB].mergeFrom(Unknown);
    }
    return Changed;
  }

  bool applyDirectCalleeStores(CallBase &CallerCB, Function &Callee,
                               Function &Current, LocalState &CallerState) {
    bool Changed = false;
    LocalState CalleeState;
    for (Argument &Arg : Callee.args()) {
      if (Arg.getArgNo() >= CallerCB.arg_size())
        break;
      if (isFunctionPointerTy(Arg.getType())) {
        CalleeState.value_cache[&Arg] =
            evaluateValue(CallerCB.getArgOperand(Arg.getArgNo()), Current,
                          CallerState);
        continue;
      }
      if (Arg.getType()->isPointerTy()) {
        const AbstractObject *Obj = index_.lookupFormalObject(&Arg);
        if (Obj)
          CalleeState.value_cache[&Arg].slots.insert({Obj->id, 0, false});
      }
    }

    for (Instruction &I : instructions(Callee)) {
      auto *SI = dyn_cast<StoreInst>(&I);
      if (!SI)
        continue;

      Argument *BaseArg = dyn_cast<Argument>(stripToBaseObject(
          SI->getPointerOperand()->stripPointerCasts()));
      if (!BaseArg || BaseArg->getArgNo() >= CallerCB.arg_size())
        continue;

      PointerTargets FormalPtrs =
          resolvePointerTargets(SI->getPointerOperand(), Callee, CalleeState);
      PointerTargets ActualPtrs =
          resolvePointerTargets(CallerCB.getArgOperand(BaseArg->getArgNo()),
                                Current, CallerState);
      AbstractValue Stored =
          evaluateValue(SI->getValueOperand(), Callee, CalleeState);

      for (const SlotKey &FormalSlot : FormalPtrs.slots) {
        for (const SlotKey &ActualSlot : ActualPtrs.slots) {
          SlotKey Mapped = ActualSlot;
          if (!FormalSlot.unknown)
            Mapped.offset += FormalSlot.offset;
          else
            Mapped.unknown = true;
          Changed |= slot_state_[Mapped].mergeFrom(Stored);
        }
      }
    }
    return Changed;
  }

  Value *stripToBaseObject(Value *V) {
    Value *Cur = V;
    while (true) {
      if (auto *GEP = dyn_cast<GEPOperator>(Cur)) {
        Cur = GEP->getPointerOperand();
        continue;
      }
      if (auto *BCO = dyn_cast<BitCastOperator>(Cur)) {
        Cur = BCO->getOperand(0);
        continue;
      }
      break;
    }
    return Cur;
  }

  DFPATargetSet signatureFilter(const CallBase *CB) const {
    DFPATargetSet Targets;
    if (!config_.enable_signature_filter) {
      for (const auto &BySig : index_.getAddressTakenBySignature())
        Targets.insert(BySig.second.begin(), BySig.second.end());
      return Targets;
    }

    auto It = index_.getAddressTakenBySignature().find(index_.getSignature(CB));
    if (It != index_.getAddressTakenBySignature().end())
      Targets.insert(It->second.begin(), It->second.end());
    return Targets;
  }

  void materializeResults() {
    DFPAStats Stats;
    Stats.num_unknown_slot_degradations = unknown_slot_degradations_;
    for (CallBase *CB : index_.getIndirectCalls()) {
      ++Stats.num_indirect_calls;
      LocalState State;
      AbstractValue V = evaluateValue(CB->getCalledOperand(), *CB->getFunction(),
                                      State);
      DFPATargetSet Targets = V.funcs;
      DFPATargetSet Sig = signatureFilter(CB);

      if (V.unknown_callable) {
        Targets.insert(Sig.begin(), Sig.end());
        coarse_unknown_.insert(CB);
      } else if (Targets.empty()) {
        Targets.insert(Sig.begin(), Sig.end());
        coarse_missing_targets_.insert(CB);
      } else if (!Sig.empty()) {
        DFPATargetSet Filtered;
        for (Function *F : Targets)
          if (Sig.count(F))
            Filtered.insert(F);
        Targets = Filtered.empty() ? Sig : Filtered;
      }

      coarse_targets_[CB] = Targets;
      Stats.coarse_total_targets += Targets.size();

      DFPATargetInfo Info;
      Info.targets = Targets;
      Info.precise = Targets.size() <= 1 && !coarse_unknown_.count(CB);
      Info.refined = false;
      Info.had_unknown_flow = coarse_unknown_.count(CB);
      result_.setTargets(CB, Info);
      Stats.refined_total_targets += Targets.size();
      if (Info.precise)
        ++Stats.num_precise_calls;
    }
    result_.setStats(Stats);
  }

  struct DemandOutcome {
    DFPATargetSet targets;
    bool precise = true;
    bool budget_hit = false;
    bool unknown = false;
  };

  std::set<DemandValueKey> active_value_states_;
  std::set<DemandSlotKey> active_slot_states_;
  std::map<DemandValueKey, DemandOutcome> value_cache_;
  std::map<DemandSlotKey, DemandOutcome> slot_cache_;

  void runDemandRefinement() {
    buildDemandIndexes();
    DFPAStats Stats = result_.getStats();
    for (const auto &Entry : coarse_targets_) {
      const CallBase *CB = Entry.first;
      const DFPATargetSet &Coarse = Entry.second;
      bool NeedsRefine = !config_.refine_ambiguous_only || Coarse.size() > 1 ||
                         coarse_unknown_.count(CB) ||
                         coarse_missing_targets_.count(CB);
      if (!NeedsRefine)
        continue;

      ++Stats.num_refined_calls;
      demand_steps_ = 0;
      active_value_states_.clear();
      active_slot_states_.clear();
      value_cache_.clear();
      slot_cache_.clear();
      DemandOutcome Outcome =
          resolveCallableDemand(CB->getCalledOperand(), ContextKey(), CB, 0,
                                nullptr);
      DFPATargetSet Final = Outcome.targets;
      if (Outcome.unknown || Outcome.budget_hit) {
        Final.insert(Coarse.begin(), Coarse.end());
      }
      if (config_.enable_signature_filter) {
        DFPATargetSet Sig = signatureFilter(CB);
        DFPATargetSet Filtered;
        for (Function *F : Final)
          if (Sig.empty() || Sig.count(F))
            Filtered.insert(F);
        if (!Filtered.empty())
          Final.swap(Filtered);
      }

      DFPATargetInfo Info;
      Info.targets = Final;
      Info.refined = true;
      Info.had_unknown_flow = Outcome.unknown || coarse_unknown_.count(CB);
      Info.precise =
          !Info.had_unknown_flow && !Outcome.budget_hit;
      result_.setTargets(CB, Info);
      if (Outcome.budget_hit)
        ++Stats.num_budget_fallbacks;
    }

    Stats.num_precise_calls = 0;
    Stats.refined_total_targets = 0;
    for (const auto &Entry : result_.getAllTargets()) {
      if (Entry.second.precise)
        ++Stats.num_precise_calls;
      Stats.refined_total_targets += Entry.second.targets.size();
    }
    result_.setStats(Stats);
  }

  DemandOutcome resolveCallableDemand(Value *V, ContextKey Ctx,
                                      const CallBase *Root,
                                      std::size_t Depth,
                                      const CallBase *DirectBinding) {
    DemandValueKey Key{V, Ctx, DirectBinding};
    auto CacheIt = value_cache_.find(Key);
    if (CacheIt != value_cache_.end())
      return CacheIt->second;

    DemandOutcome Outcome;
    (void)Root;
    (void)Depth;
    if (++demand_steps_ > config_.max_demand_states) {
      Outcome.budget_hit = true;
      Outcome.precise = false;
      return Outcome;
    }
    if (active_value_states_.count(Key)) {
      Outcome.unknown = true;
      Outcome.precise = false;
      return Outcome;
    }
    active_value_states_.insert(Key);

    if (Function *F = dyn_cast<Function>(V)) {
      if (!F->isIntrinsic())
        Outcome.targets.insert(F);
      active_value_states_.erase(Key);
      value_cache_[Key] = Outcome;
      return Outcome;
    }

    if (auto *BCO = dyn_cast<BitCastOperator>(V)) {
      Outcome =
          resolveCallableDemand(BCO->getOperand(0), Ctx, Root, Depth + 1,
                                DirectBinding);
      active_value_states_.erase(Key);
      value_cache_[Key] = Outcome;
      return Outcome;
    }
    if (auto *CI = dyn_cast<CastInst>(V)) {
      Outcome = resolveCallableDemand(CI->getOperand(0), Ctx, Root, Depth + 1,
                                      DirectBinding);
      active_value_states_.erase(Key);
      value_cache_[Key] = Outcome;
      return Outcome;
    }
    if (auto *PN = dyn_cast<PHINode>(V)) {
      for (unsigned I = 0; I < PN->getNumIncomingValues(); ++I)
        mergeDemandOutcome(
            Outcome,
            resolveCallableDemand(PN->getIncomingValue(I), Ctx, Root,
                                  Depth + 1, DirectBinding));
      active_value_states_.erase(Key);
      value_cache_[Key] = Outcome;
      return Outcome;
    }
    if (auto *SI = dyn_cast<SelectInst>(V)) {
      mergeDemandOutcome(
          Outcome,
          resolveCallableDemand(SI->getTrueValue(), Ctx, Root, Depth + 1,
                                DirectBinding));
      mergeDemandOutcome(
          Outcome,
          resolveCallableDemand(SI->getFalseValue(), Ctx, Root, Depth + 1,
                                DirectBinding));
      active_value_states_.erase(Key);
      value_cache_[Key] = Outcome;
      return Outcome;
    }
    if (auto *LI = dyn_cast<LoadInst>(V)) {
      LocalState State;
      PointerTargets Ptrs =
          resolvePointerTargets(LI->getPointerOperand(), *LI->getFunction(), State);
      for (const SlotKey &Slot : Ptrs.slots)
        mergeDemandOutcome(Outcome,
                           resolveCallableFromSlotDemand(Slot, Ctx, Root,
                                                         Depth + 1,
                                                         DirectBinding));
      if (Ptrs.unknown)
        Outcome.unknown = true;
      active_value_states_.erase(Key);
      value_cache_[Key] = Outcome;
      return Outcome;
    }
    if (auto *Arg = dyn_cast<Argument>(V)) {
      if (!isFunctionPointerTy(Arg->getType())) {
        Outcome.unknown = true;
        active_value_states_.erase(Key);
        value_cache_[Key] = Outcome;
        return Outcome;
      }
      if (DirectBinding && DirectBinding->getCalledFunction() == Arg->getParent()
          && Arg->getArgNo() < DirectBinding->arg_size()) {
        Outcome = resolveCallableDemand(DirectBinding->getArgOperand(
                                            Arg->getArgNo()),
                                        Ctx, Root, Depth + 1, nullptr);
        active_value_states_.erase(Key);
        value_cache_[Key] = Outcome;
        return Outcome;
      }
      auto CallersIt = index_.getDirectCallers().find(Arg->getParent());
      if (CallersIt == index_.getDirectCallers().end()) {
        Outcome.unknown = true;
        active_value_states_.erase(Key);
        value_cache_[Key] = Outcome;
        return Outcome;
      }
      for (CallBase *Caller : CallersIt->second) {
        if (Arg->getArgNo() < Caller->arg_size())
          mergeDemandOutcome(
              Outcome,
              resolveCallableDemand(Caller->getArgOperand(Arg->getArgNo()), Ctx,
                                    Root, Depth + 1, nullptr));
      }
      active_value_states_.erase(Key);
      value_cache_[Key] = Outcome;
      return Outcome;
    }
    if (auto *CB = dyn_cast<CallBase>(V)) {
      if (CB->isIndirectCall()) {
        DemandOutcome Callees =
            resolveCallableDemand(CB->getCalledOperand(), Ctx, Root, Depth + 1,
                                  DirectBinding);
        DFPATargetSet CandidateCallees = Callees.targets;
        if (CandidateCallees.empty())
          CandidateCallees = coarse_targets_[CB];
        ContextKey Next = Ctx;
        Next.push(CB, config_.indirect_ctx_k);
        for (Function *Callee : CandidateCallees) {
          if (Callee->isDeclaration()) {
            Outcome.unknown = true;
            continue;
          }
          for (ReturnInst *RI : index_.getReturns().at(Callee)) {
            if (Value *RV = RI->getReturnValue())
              mergeDemandOutcome(Outcome,
                                 resolveCallableDemand(RV, Next, Root,
                                                       Depth + 1, nullptr));
          }
        }
        if (Callees.unknown)
          Outcome.unknown = true;
        active_value_states_.erase(Key);
        value_cache_[Key] = Outcome;
        return Outcome;
      }
      Function *Callee = CB->getCalledFunction();
      if (!Callee || Callee->isDeclaration()) {
        Outcome.unknown = true;
        active_value_states_.erase(Key);
        value_cache_[Key] = Outcome;
        return Outcome;
      }
      auto RetIt = index_.getReturns().find(Callee);
      if (RetIt == index_.getReturns().end()) {
        Outcome.unknown = true;
        active_value_states_.erase(Key);
        value_cache_[Key] = Outcome;
        return Outcome;
      }
      for (ReturnInst *RI : RetIt->second) {
        if (Value *RV = RI->getReturnValue())
          mergeDemandOutcome(
              Outcome,
              resolveCallableDemand(RV, Ctx, Root, Depth + 1, CB));
      }
      active_value_states_.erase(Key);
      value_cache_[Key] = Outcome;
      return Outcome;
    }

    Outcome.unknown = true;
    active_value_states_.erase(Key);
    value_cache_[Key] = Outcome;
    return Outcome;
  }

  DemandOutcome resolveCallableFromSlotDemand(const SlotKey &Slot, ContextKey Ctx,
                                              const CallBase *Root,
                                              std::size_t Depth,
                                              const CallBase *DirectBinding) {
    DemandSlotKey Key{Slot, Ctx, DirectBinding};
    auto CacheIt = slot_cache_.find(Key);
    if (CacheIt != slot_cache_.end())
      return CacheIt->second;

    DemandOutcome Outcome;
    (void)Root;
    (void)Depth;
    if (++demand_steps_ > config_.max_demand_states) {
      Outcome.budget_hit = true;
      Outcome.precise = false;
      return Outcome;
    }
    if (active_slot_states_.count(Key)) {
      Outcome.unknown = true;
      Outcome.precise = false;
      return Outcome;
    }
    active_slot_states_.insert(Key);

    auto VisitStore = [&](StoreInst *SI) {
      LocalState State;
      PointerTargets Ptrs =
          resolvePointerTargets(SI->getPointerOperand(), *SI->getFunction(), State);
      for (const SlotKey &Writer : Ptrs.slots) {
        if (!slotMatches(Writer, Slot))
          continue;
        mergeDemandOutcome(Outcome,
                           resolveCallableDemand(SI->getValueOperand(), Ctx, Root,
                                                 Depth + 1, DirectBinding));
      }
      if (Ptrs.unknown)
        Outcome.unknown = true;
    };

    auto StoreIt = stores_by_object_.find(Slot.object_id);
    if (StoreIt != stores_by_object_.end()) {
      for (StoreInst *SI : StoreIt->second)
        VisitStore(SI);
    }
    for (StoreInst *SI : stores_with_unknown_base_)
      VisitStore(SI);

    auto VisitMemTransfer = [&](CallBase *CB) {
      LocalState State;
      PointerTargets Dst =
          resolvePointerTargets(CB->getArgOperand(0), *CB->getFunction(), State);
      PointerTargets Src =
          resolvePointerTargets(CB->getArgOperand(1), *CB->getFunction(), State);
      for (const SlotKey &DstSlot : Dst.slots) {
        if (!slotMatches(DstSlot, Slot))
          continue;
        for (const SlotKey &SrcSlot : Src.slots)
          mergeDemandOutcome(Outcome,
                             resolveCallableFromSlotDemand(SrcSlot, Ctx, Root,
                                                           Depth + 1,
                                                           DirectBinding));
        if (Src.unknown)
          Outcome.unknown = true;
      }
    };

    auto MemTransferIt = memtransfers_by_dst_object_.find(Slot.object_id);
    if (MemTransferIt != memtransfers_by_dst_object_.end()) {
      for (CallBase *CB : MemTransferIt->second)
        VisitMemTransfer(CB);
    }
    for (CallBase *CB : memtransfers_with_unknown_base_)
      VisitMemTransfer(CB);

    if (slot_state_[Slot].from_unknown_slot)
      Outcome.unknown = true;
    active_slot_states_.erase(Key);
    slot_cache_[Key] = Outcome;
    return Outcome;
  }

  bool slotMatches(const SlotKey &A, const SlotKey &B) const {
    if (A.object_id != B.object_id)
      return false;
    if (A.unknown || B.unknown)
      return true;
    return A.offset == B.offset;
  }

  void mergeDemandOutcome(DemandOutcome &Dst, const DemandOutcome &Src) {
    Dst.targets.insert(Src.targets.begin(), Src.targets.end());
    Dst.precise &= Src.precise;
    Dst.budget_hit |= Src.budget_hit;
    Dst.unknown |= Src.unknown;
  }
};

} // namespace

char DFPAPass::ID = 0;

DFPAPass::DFPAPass(DFPAConfig Config)
    : ModulePass(ID), config_(std::move(Config)) {}

void DFPAPass::getAnalysisUsage(AnalysisUsage &AU) const { AU.setPreservesAll(); }

bool DFPAPass::runOnModule(Module &M) {
  DFPAAnalyzer Analyzer(M, config_);
  result_ = Analyzer.run();
  return false;
}
