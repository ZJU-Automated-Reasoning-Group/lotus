#include "Alias/UnderApproxAA/EquivDB.h"

#include "Alias/UnderApproxAA/Canonical.h"

#include <algorithm>
#include <limits>
#include <unordered_map>

#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>

using namespace llvm;
using namespace UnderApprox;

namespace {

constexpr VarId kInvalidVar = static_cast<VarId>(-1);

bool isPointerValue(const Value *V) {
  return V && V->getType()->isPointerTy();
}

bool isFreshAllocationRoot(const Value *V) {
  if (isa<AllocaInst>(V))
    return true;

  if (isAllocationCall(V))
    return true;

  const auto *CB = dyn_cast<CallBase>(V);
  return CB && CB->getType()->isPointerTy() &&
         CB->hasRetAttr(Attribute::NoAlias);
}

std::string pointerToString(const void *Ptr) {
  return std::to_string(static_cast<unsigned long long>(
      reinterpret_cast<uintptr_t>(Ptr)));
}

std::string apIntToString(const APInt &Value) {
  SmallString<32> Buffer;
  Value.toString(Buffer, 10, true);
  return Buffer.str().str();
}

bool mayWriteThroughCall(const CallBase &CB) {
  if (CB.onlyReadsMemory() || CB.doesNotAccessMemory())
    return false;
  return true;
}
} // namespace

size_t EquivDB::AccessPathHash::operator()(const AccessPath &Path) const {
  hash_code H = hash_value(Path.Fields.size());
  for (FieldLabel F : Path.Fields)
    H = hash_combine(H, F);
  return static_cast<size_t>(H);
}

size_t EquivDB::PointerKeyHash::operator()(const PointerKey &Key) const {
  hash_code H = hash_combine(static_cast<unsigned>(Key.K), Key.Base);
  for (FieldLabel F : Key.Path.Fields)
    H = hash_combine(H, F);
  return static_cast<size_t>(H);
}

size_t EquivDB::SlotIdHash::operator()(const SlotId &Slot) const {
  return static_cast<size_t>(hash_combine(Slot.Object, Slot.ByteOffset));
}

size_t EquivDB::PathAtomHash::operator()(const PathAtom &Atom) const {
  return static_cast<size_t>(
      hash_combine(Atom.SourceElementType, Atom.OperandIndex, Atom.Signature));
}

EquivDB::EquivDB(Function &Func, MemorySSA *MemSSA, DominatorTree *DomTree)
    : DL(Func.getParent()->getDataLayout()), F(Func), MSSA(MemSSA),
      DT(DomTree) {
  if (F.empty())
    return;

  runDataflow();
  finalizeQueryState();
}

EquivDB::PointerRef::Kind
EquivDB::classifyRootKind(const Value *Base) const {
  if (!Base)
    return PointerRef::Kind::Invalid;
  if (isa<GlobalValue>(Base) || isa<ConstantPointerNull>(Base))
    return PointerRef::Kind::Fixed;
  if (isFreshAllocationRoot(Base))
    return PointerRef::Kind::Fresh;
  return PointerRef::Kind::Value;
}

EquivDB::PointerRef EquivDB::makeRootRef(PointerRef::Kind Kind,
                                         const Value *Base) {
  if (!Base)
    return PointerRef{};

  PointerKey Key;
  Key.K = Kind;
  Key.Base = Base;

  auto It = PointerInterner.find(Key);
  if (It != PointerInterner.end()) {
    PointerRef Ref = RefByVar.lookup(It->second);
    Ref.Node = kNoNode;
    return Ref;
  }

  PointerRef Ref;
  Ref.K = Kind;
  Ref.Base = Base;
  Ref.Var = NextVarId++;
  PointerInterner.emplace(Key, Ref.Var);
  RefByVar[Ref.Var] = Ref;
  return Ref;
}

EquivDB::PointerRef EquivDB::rootOf(const PointerRef &Ref) const {
  if (!Ref.valid())
    return PointerRef{};

  PointerRef::Kind RootKind = Ref.K == PointerRef::Kind::AccessPath
                                  ? classifyRootKind(Ref.Base)
                                  : Ref.K;
  return const_cast<EquivDB *>(this)->makeRootRef(RootKind, Ref.Base);
}

EquivDB::PointerRef EquivDB::makeAccessPathRef(const PointerRef &BaseRef,
                                               const AccessPath &Suffix) {
  if (!BaseRef.valid())
    return PointerRef{};

  PointerRef Root = rootOf(BaseRef);
  AccessPath Full = Root.Path;
  if (BaseRef.K == PointerRef::Kind::AccessPath)
    Full = BaseRef.Path;
  Full.Fields.append(Suffix.Fields.begin(), Suffix.Fields.end());

  if (Full.Fields.empty())
    return Root;

  PointerKey Key;
  Key.K = PointerRef::Kind::AccessPath;
  Key.Base = Root.Base;
  Key.Path = Full;

  auto It = PointerInterner.find(Key);
  if (It != PointerInterner.end()) {
    PointerRef Ref = RefByVar.lookup(It->second);
    Ref.Node = kNoNode;
    return Ref;
  }

  PointerRef Ref;
  Ref.K = PointerRef::Kind::AccessPath;
  Ref.Base = Root.Base;
  Ref.Path = std::move(Full);
  Ref.Var = NextVarId++;
  PointerInterner.emplace(Key, Ref.Var);
  RefByVar[Ref.Var] = Ref;
  return Ref;
}

EquivDB::PointerRef EquivDB::attachRef(MustAliasState &State,
                                       const PointerRef &Ref) {
  if (!Ref.valid())
    return Ref;

  PointerRef Attached = Ref;
  State.Graph.addVariable(Attached.Var);

  if (Attached.K == PointerRef::Kind::AccessPath && !Attached.Path.Fields.empty()) {
    PointerRef Root = rootOf(Attached);
    State.Graph.addVariable(Root.Var);

    PointerRef Prev = Root;
    AccessPath Prefix;
    for (FieldLabel Label : Attached.Path.Fields) {
      Prefix.Fields.push_back(Label);
      PointerRef Next = makeAccessPathRef(Root, Prefix);
      State.Graph.addVariable(Next.Var);
      State.Graph.storeEdge(Prev.Var, Label, Next.Var);
      Prev = Next;
    }
  }

  Attached.Node = State.Graph.getNode(Attached.Var);
  return Attached;
}

void EquivDB::refreshStateNodes(MustAliasState &State) {
  for (auto &KV : State.ExprEnv)
    KV.second = attachRef(State, KV.second);
  for (auto &KV : State.MustStore)
    KV.second = attachRef(State, KV.second);
}

EquivDB::MustAliasState EquivDB::buildEntryState() {
  MustAliasState State;
  for (Argument &Arg : F.args()) {
    if (!Arg.getType()->isPointerTy())
      continue;
    PointerRef Ref = attachRef(State, makeRootRef(PointerRef::Kind::Value, &Arg));
    State.ExprEnv[&Arg] = Ref;
  }
  return State;
}

EquivDB::MustAliasState EquivDB::intersectStates(const MustAliasState &LHS,
                                                 const MustAliasState &RHS) {
  MustAliasState Result;
  Result.Graph = AliasGraph::intersect(LHS.Graph, RHS.Graph);

  for (const auto &KV : LHS.ExprEnv) {
    auto It = RHS.ExprEnv.find(KV.first);
    if (It != RHS.ExprEnv.end() && KV.second == It->second)
      Result.ExprEnv[KV.first] = KV.second;
  }

  for (const auto &KV : LHS.MustStore) {
    auto It = RHS.MustStore.find(KV.first);
    if (It != RHS.MustStore.end() && KV.second == It->second)
      Result.MustStore.emplace(KV.first, KV.second);
  }

  refreshStateNodes(Result);
  return Result;
}

bool EquivDB::stateEquivalent(const MustAliasState &LHS,
                              const MustAliasState &RHS) {
  if (LHS.ExprEnv.size() != RHS.ExprEnv.size() ||
      LHS.MustStore.size() != RHS.MustStore.size())
    return false;

  for (const auto &KV : LHS.ExprEnv) {
    auto It = RHS.ExprEnv.find(KV.first);
    if (It == RHS.ExprEnv.end() || !(KV.second == It->second))
      return false;
  }

  for (const auto &KV : LHS.MustStore) {
    auto It = RHS.MustStore.find(KV.first);
    if (It == RHS.MustStore.end() || !(KV.second == It->second))
      return false;
  }

  return true;
}

EquivDB::MustAliasState EquivDB::buildInState(const BasicBlock &BB) {
  if (&BB == &F.getEntryBlock())
    return buildEntryState();

  auto PI = pred_begin(&BB);
  auto PE = pred_end(&BB);
  if (PI == PE)
    return MustAliasState();

  const BasicBlock *FirstPred = *PI++;
  MustAliasState Result = OutStates.lookup(FirstPred);
  for (; PI != PE; ++PI)
    Result = intersectStates(Result, OutStates.lookup(*PI));
  return Result;
}

std::string EquivDB::normalizeIndexExpr(const Value *V, unsigned Depth) const {
  if (!V)
    return "null";

  if (Depth > 8)
    return "leaf:" + pointerToString(V);

  V = stripNoopArithmetic(V);

  if (const auto *CI = dyn_cast<ConstantInt>(V))
    return "const:" + apIntToString(CI->getValue()) + ":" +
           std::to_string(CI->getBitWidth());

  if (const auto *GV = dyn_cast<GlobalValue>(V))
    return "global:" + GV->getName().str();

  if (const auto *Arg = dyn_cast<Argument>(V))
    return "arg:" + std::to_string(Arg->getArgNo()) + ":" +
           pointerToString(Arg->getType());

  const auto *Op = dyn_cast<Operator>(V);
  if (!Op || !V->getType()->isIntegerTy())
    return "leaf:" + pointerToString(V);

  auto UnarySig = [&](StringRef Name) {
    return (Name + "(" +
            normalizeIndexExpr(Op->getOperand(0), Depth + 1) + ")")
        .str();
  };

  switch (Op->getOpcode()) {
  case Instruction::ZExt:
    return UnarySig("zext");
  case Instruction::SExt:
    return UnarySig("sext");
  case Instruction::Trunc:
    return UnarySig("trunc");
  case Instruction::Freeze:
    return UnarySig("freeze");
  case Instruction::Add:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor: {
    std::string L = normalizeIndexExpr(Op->getOperand(0), Depth + 1);
    std::string R = normalizeIndexExpr(Op->getOperand(1), Depth + 1);
    if (L > R)
      std::swap(L, R);
    return "bin:" + std::to_string(Op->getOpcode()) + "(" + L + "," + R +
           ")";
  }
  case Instruction::Sub:
  case Instruction::Shl:
  case Instruction::AShr:
  case Instruction::LShr:
    return "bin:" + std::to_string(Op->getOpcode()) + "(" +
           normalizeIndexExpr(Op->getOperand(0), Depth + 1) + "," +
           normalizeIndexExpr(Op->getOperand(1), Depth + 1) + ")";
  default:
    return "leaf:" + pointerToString(V);
  }
}

FieldLabel EquivDB::internPathAtom(const PathAtom &Atom) {
  auto It = PathInterner.find(Atom);
  if (It != PathInterner.end())
    return It->second;

  FieldLabel Label = NextFieldLabel++;
  PathInterner.emplace(Atom, Label);
  return Label;
}

bool EquivDB::buildGEPPath(const GEPOperator &GEP, AccessPath &Out) {
  Out.Fields.clear();
  const Type *SourceTy = GEP.getSourceElementType();
  for (unsigned I = 1, E = GEP.getNumOperands(); I < E; ++I) {
    PathAtom Atom;
    Atom.SourceElementType = SourceTy;
    Atom.OperandIndex = I;
    Atom.Signature = normalizeIndexExpr(GEP.getOperand(I));
    Out.Fields.push_back(internPathAtom(Atom));
  }
  return true;
}

EquivDB::PointerRef EquivDB::lookupValueRef(const Value *V,
                                            MustAliasState &State) {
  if (!isPointerValue(V))
    return PointerRef{};

  auto It = State.ExprEnv.find(V);
  if (It != State.ExprEnv.end())
    return attachRef(State, It->second);

  if (isa<ConstantPointerNull>(V))
    return attachRef(State, makeRootRef(PointerRef::Kind::Fixed, V));

  if (isa<GlobalValue>(V))
    return attachRef(State, makeRootRef(PointerRef::Kind::Fixed, V));

  const Value *Stripped = stripNoopCasts(V);
  if (Stripped != V)
    return lookupValueRef(Stripped, State);

  if (const auto *GEP = dyn_cast<GEPOperator>(V))
    return evaluateGEP(*GEP, State);

  if (const auto *Op = dyn_cast<Operator>(V)) {
    if (Op->getOpcode() == Instruction::IntToPtr) {
      const Value *IntVal = stripNoopArithmetic(Op->getOperand(0));
      if (const auto *PTI = dyn_cast<Operator>(IntVal))
        if (PTI->getOpcode() == Instruction::PtrToInt)
          return lookupValueRef(PTI->getOperand(0), State);
    }
  }

  if (const auto *Arg = dyn_cast<Argument>(V)) {
    PointerRef Ref = attachRef(State, makeRootRef(PointerRef::Kind::Value, Arg));
    State.ExprEnv[V] = Ref;
    return Ref;
  }

  if (const auto *AI = dyn_cast<AllocaInst>(V)) {
    PointerRef Ref = attachRef(State, makeRootRef(PointerRef::Kind::Fresh, AI));
    State.ExprEnv[V] = Ref;
    return Ref;
  }

  if (const auto *CB = dyn_cast<CallBase>(V))
    if (CB->hasRetAttr(Attribute::NoAlias) || isAllocationCall(CB)) {
      PointerRef Ref = attachRef(State, makeRootRef(PointerRef::Kind::Fresh, CB));
      State.ExprEnv[V] = Ref;
      return Ref;
    }

  return PointerRef{};
}

EquivDB::PointerRef EquivDB::evaluatePHI(const PHINode &PN) {
  if (!PN.getType()->isPointerTy())
    return PointerRef{};

  PointerRef Common;
  bool Seen = false;

  for (unsigned I = 0, E = PN.getNumIncomingValues(); I < E; ++I) {
    const BasicBlock *Pred = PN.getIncomingBlock(I);
    MustAliasState PredState = OutStates.lookup(Pred);
    PointerRef Incoming = lookupValueRef(PN.getIncomingValue(I), PredState);
    if (!Incoming.valid())
      return PointerRef{};

    if (!Seen) {
      Common = Incoming;
      Seen = true;
      continue;
    }

    if (!(Common == Incoming))
      return PointerRef{};
  }

  return Common;
}

EquivDB::PointerRef EquivDB::evaluateSelect(const SelectInst &SI,
                                            MustAliasState &State) {
  if (!SI.getType()->isPointerTy())
    return PointerRef{};

  if (const auto *Cond = dyn_cast<ConstantInt>(SI.getCondition())) {
    return lookupValueRef(Cond->isOne() ? SI.getTrueValue() : SI.getFalseValue(),
                          State);
  }

  PointerRef T = lookupValueRef(SI.getTrueValue(), State);
  PointerRef FRef = lookupValueRef(SI.getFalseValue(), State);
  if (T.valid() && FRef.valid() && T == FRef)
    return T;
  return PointerRef{};
}

EquivDB::PointerRef EquivDB::evaluateGEP(const GEPOperator &GEP,
                                         MustAliasState &State) {
  PointerRef BaseRef = lookupValueRef(GEP.getPointerOperand(), State);
  if (!BaseRef.valid())
    return PointerRef{};

  if (GEP.hasAllZeroIndices())
    return BaseRef;

  AccessPath Path;
  if (!buildGEPPath(GEP, Path))
    return PointerRef{};

  return attachRef(State, makeAccessPathRef(BaseRef, Path));
}

bool EquivDB::tryGetSingletonSlot(const Value *Ptr, SlotId &Out) const {
  if (!isPointerValue(Ptr))
    return false;

  unsigned AddrSpace = Ptr->getType()->getPointerAddressSpace();
  APInt Offset(DL.getPointerSizeInBits(AddrSpace), 0);
  const Value *Base = Ptr->stripAndAccumulateInBoundsConstantOffsets(DL, Offset);
  const Value *Obj = getUnderlyingObject(Ptr);
  if (!Obj || Base != Obj)
    return false;

  if (!isa<AllocaInst>(Obj) && !isa<GlobalVariable>(Obj) &&
      !isFreshAllocationRoot(Obj))
    return false;

  Out.Object = Obj;
  Out.ByteOffset = Offset.getSExtValue();
  return true;
}

EquivDB::PointerRef EquivDB::evaluateLoad(const LoadInst &LI,
                                          MustAliasState &State) {
  if (!LI.getType()->isPointerTy())
    return PointerRef{};

  SlotId Slot;
  if (!tryGetSingletonSlot(LI.getPointerOperand(), Slot))
    return PointerRef{};

  auto It = State.MustStore.find(Slot);
  if (It != State.MustStore.end())
    return attachRef(State, It->second);

  PointerRef Ref = recoverLoadFromMemorySSA(LI, State);
  if (Ref.valid())
    return Ref;

  Ref = recoverLoadFromDominatorTree(LI, State);
  if (Ref.valid())
    return Ref;

  return PointerRef{};
}

bool EquivDB::dominatesInst(const Instruction *Def,
                            const Instruction *Use) const {
  if (!DT || !Def || !Use || Def == Use)
    return false;
  return DT->dominates(Def, Use);
}

EquivDB::PointerRef EquivDB::recoverLoadFromMemorySSA(const LoadInst &LI,
                                                      MustAliasState &State) {
  if (!MSSA)
    return PointerRef{};

  SlotId LoadSlot;
  if (!tryGetSingletonSlot(LI.getPointerOperand(), LoadSlot))
    return PointerRef{};

  MemoryAccess *MA = MSSA->getMemoryAccess(&LI);
  if (!MA)
    return PointerRef{};

  MemorySSAWalker *Walker = MSSA->getWalker();
  MemoryAccess *Clob = nullptr;
  if (Walker) {
    Clob = Walker->getClobberingMemoryAccess(MA);
  } else if (auto *MUD = dyn_cast<MemoryUseOrDef>(MA)) {
    Clob = MUD->getDefiningAccess();
  }
  if (!Clob)
    return PointerRef{};

  SmallVector<PointerRef, 4> StoredRefs;
  SmallPtrSet<const MemoryAccess *, 8> Visited;

  std::function<bool(MemoryAccess *)> Collect =
      [&](MemoryAccess *Access) -> bool {
    if (!Access || !Visited.insert(Access).second)
      return false;

    if (auto *Phi = dyn_cast<MemoryPhi>(Access)) {
      if (Phi->getNumIncomingValues() == 0)
        return false;

      for (const Use &IncU : Phi->incoming_values()) {
        auto *Incoming = cast<MemoryAccess>(IncU.get());
        if (!Collect(Incoming))
          return false;
      }
      return !StoredRefs.empty();
    }

    auto *MUD = dyn_cast<MemoryUseOrDef>(Access);
    auto *SI = MUD ? dyn_cast_or_null<StoreInst>(MUD->getMemoryInst()) : nullptr;
    if (!SI)
      return false;

    SlotId StoreSlot;
    if (!tryGetSingletonSlot(SI->getPointerOperand(), StoreSlot) ||
        !(StoreSlot == LoadSlot))
      return false;

    PointerRef Stored = lookupValueRef(SI->getValueOperand(), State);
    if (!Stored.valid())
      return false;

    StoredRefs.push_back(Stored);
    return true;
  };

  if (!Collect(Clob) || StoredRefs.empty())
    return PointerRef{};

  PointerRef Common = StoredRefs.front();
  for (const PointerRef &Ref : StoredRefs)
    if (!(Ref == Common))
      return PointerRef{};

  return attachRef(State, Common);
}

EquivDB::PointerRef
EquivDB::recoverLoadFromDominatorTree(const LoadInst &LI, MustAliasState &State) {
  if (MSSA || !DT)
    return PointerRef{};

  SlotId LoadSlot;
  if (!tryGetSingletonSlot(LI.getPointerOperand(), LoadSlot))
    return PointerRef{};

  const StoreInst *UniqueStore = nullptr;
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      const auto *SI = dyn_cast<StoreInst>(&I);
      if (!SI)
        continue;

      SlotId StoreSlot;
      if (!tryGetSingletonSlot(SI->getPointerOperand(), StoreSlot) ||
          !(StoreSlot == LoadSlot) || !dominatesInst(SI, &LI))
        continue;

      if (UniqueStore && UniqueStore != SI)
        return PointerRef{};
      UniqueStore = SI;
    }
  }

  if (!UniqueStore)
    return PointerRef{};

  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      if (&I == UniqueStore || &I == &LI)
        continue;
      if (!I.mayWriteToMemory())
        continue;
      if (!dominatesInst(UniqueStore, &I) || !dominatesInst(&I, &LI))
        continue;
      return PointerRef{};
    }
  }

  PointerRef Stored = lookupValueRef(UniqueStore->getValueOperand(), State);
  if (!Stored.valid())
    return PointerRef{};
  return attachRef(State, Stored);
}

EquivDB::SummaryRef EquivDB::summarizePointerValue(const Value *V,
                                                   const Function *Callee,
                                                   unsigned Depth) const {
  SummaryRef Unknown;
  if (!isPointerValue(V) || !Callee || Depth > 8)
    return Unknown;

  V = stripNoopCasts(V);

  if (const auto *Arg = dyn_cast<Argument>(V)) {
    if (Arg->getParent() == Callee) {
      SummaryRef Ref;
      Ref.K = SummaryRef::Kind::Arg;
      Ref.ArgNo = static_cast<int>(Arg->getArgNo());
      return Ref;
    }
  }

  if (const auto *GEP = dyn_cast<GEPOperator>(V)) {
    SummaryRef Base = summarizePointerValue(GEP->getPointerOperand(), Callee,
                                            Depth + 1);
    if (Base.K != SummaryRef::Kind::Arg && Base.K != SummaryRef::Kind::ArgPath)
      return Unknown;

    AccessPath Path = Base.Path;
    const_cast<EquivDB *>(this)->buildGEPPath(*GEP, Path);

    SummaryRef Ref;
    Ref.K = SummaryRef::Kind::ArgPath;
    Ref.ArgNo = Base.ArgNo;
    Ref.Path = std::move(Path);
    return Ref;
  }

  if (const auto *SI = dyn_cast<SelectInst>(V)) {
    if (const auto *Cond = dyn_cast<ConstantInt>(SI->getCondition()))
      return summarizePointerValue(Cond->isOne() ? SI->getTrueValue()
                                                 : SI->getFalseValue(),
                                   Callee, Depth + 1);

    SummaryRef T = summarizePointerValue(SI->getTrueValue(), Callee, Depth + 1);
    SummaryRef FRef =
        summarizePointerValue(SI->getFalseValue(), Callee, Depth + 1);
    return T == FRef ? T : Unknown;
  }

  if (const auto *PN = dyn_cast<PHINode>(V)) {
    SummaryRef Common;
    bool Seen = false;
    for (const Value *Incoming : PN->incoming_values()) {
      SummaryRef Cur = summarizePointerValue(Incoming, Callee, Depth + 1);
      if (Cur.K == SummaryRef::Kind::Unknown)
        return Unknown;
      if (!Seen) {
        Common = Cur;
        Seen = true;
      } else if (!(Common == Cur)) {
        return Unknown;
      }
    }
    return Common;
  }

  if (isa<ConstantPointerNull>(V)) {
    SummaryRef Ref;
    Ref.K = SummaryRef::Kind::Null;
    Ref.Fixed = V;
    return Ref;
  }

  if (isa<GlobalValue>(V)) {
    SummaryRef Ref;
    Ref.K = SummaryRef::Kind::Global;
    Ref.Fixed = V;
    return Ref;
  }

  if (isFreshAllocationRoot(V)) {
    SummaryRef Ref;
    Ref.K = SummaryRef::Kind::Fresh;
    return Ref;
  }

  return Unknown;
}

EquivDB::FunctionSummary EquivDB::summarizeFunction(const Function *Callee) const {
  if (!Callee)
    return FunctionSummary{};

  auto CacheIt = SummaryCache.find(Callee);
  if (CacheIt != SummaryCache.end())
    return CacheIt->second;

  if (!SummaryInProgress.insert(Callee).second)
    return FunctionSummary{};

  FunctionSummary Summary;
  auto Finish = [&](FunctionSummary Result) {
    SummaryCache[Callee] = Result;
    SummaryInProgress.erase(Callee);
    return Result;
  };

  if (Callee->isDeclaration() || Callee->empty())
    return Finish(Summary);

  struct SummarySlotKey {
    int BaseArgNo = -1;
    AccessPath DestPath;
    int64_t ByteOffset = 0;

    bool operator==(const SummarySlotKey &Other) const {
      return BaseArgNo == Other.BaseArgNo && DestPath == Other.DestPath &&
             ByteOffset == Other.ByteOffset;
    }
  };

  struct SummarySlotKeyHash {
    size_t operator()(const SummarySlotKey &Key) const {
      hash_code H = hash_combine(Key.BaseArgNo, Key.ByteOffset);
      for (FieldLabel F : Key.DestPath.Fields)
        H = hash_combine(H, F);
      return static_cast<size_t>(H);
    }
  };

  struct SummaryState {
    DenseMap<const Value *, SummaryRef> Env;
    std::unordered_map<SummarySlotKey, SummaryRef, SummarySlotKeyHash> Stores;

    SummaryState() : Env(0) {}
  };

  DenseMap<const BasicBlock *, SummaryState> SummaryIn;
  DenseMap<const BasicBlock *, SummaryState> SummaryOut;
  bool StoreEffectsExact = true;

  auto statesEqual = [](const SummaryState &LHS, const SummaryState &RHS) {
    if (LHS.Env.size() != RHS.Env.size() || LHS.Stores.size() != RHS.Stores.size())
      return false;

    for (const auto &KV : LHS.Env) {
      auto It = RHS.Env.find(KV.first);
      if (It == RHS.Env.end() || !(KV.second == It->second))
        return false;
    }

    for (const auto &KV : LHS.Stores) {
      auto It = RHS.Stores.find(KV.first);
      if (It == RHS.Stores.end() || !(KV.second == It->second))
        return false;
    }

    return true;
  };

  auto intersectStates = [&](const SummaryState &LHS,
                             const SummaryState &RHS) -> SummaryState {
    SummaryState Result;

    for (const auto &KV : LHS.Env) {
      auto It = RHS.Env.find(KV.first);
      if (It != RHS.Env.end() && KV.second == It->second)
        Result.Env[KV.first] = KV.second;
    }

    for (const auto &KV : LHS.Stores) {
      auto It = RHS.Stores.find(KV.first);
      if (It != RHS.Stores.end() && KV.second == It->second) {
        Result.Stores.emplace(KV.first, KV.second);
      } else {
        StoreEffectsExact = false;
      }
    }
    for (const auto &KV : RHS.Stores)
      if (LHS.Stores.find(KV.first) == LHS.Stores.end())
        StoreEffectsExact = false;

    return Result;
  };

  auto buildEntryState = [&]() -> SummaryState {
    SummaryState State;
    for (const Argument &Arg : Callee->args()) {
      if (!Arg.getType()->isPointerTy())
        continue;
      SummaryRef Ref;
      Ref.K = SummaryRef::Kind::Arg;
      Ref.ArgNo = static_cast<int>(Arg.getArgNo());
      State.Env[&Arg] = Ref;
    }
    return State;
  };

  std::function<SummaryRef(const Value *, SummaryState &)> lookupSummaryValue;

  auto tryGetSummarySlot = [&](const Value *Ptr, SummaryState &State,
                               SummarySlotKey &Out) -> bool {
    if (!isPointerValue(Ptr))
      return false;

    unsigned AddrSpace = Ptr->getType()->getPointerAddressSpace();
    APInt Offset(DL.getPointerSizeInBits(AddrSpace), 0);
    const Value *Base =
        Ptr->stripAndAccumulateInBoundsConstantOffsets(DL, Offset);
    SummaryRef BaseRef = lookupSummaryValue(Base, State);
    if (BaseRef.K != SummaryRef::Kind::Arg &&
        BaseRef.K != SummaryRef::Kind::ArgPath)
      return false;

    Out.BaseArgNo = BaseRef.ArgNo;
    Out.DestPath = BaseRef.Path;
    Out.ByteOffset = Offset.getSExtValue();
    return true;
  };

  auto summaryArgMayReachSlot = [&](SummaryRef Ref,
                                    const SummarySlotKey &Slot) -> bool {
    if (Ref.K != SummaryRef::Kind::Arg && Ref.K != SummaryRef::Kind::ArgPath)
      return false;
    return Ref.ArgNo == Slot.BaseArgNo;
  };

  auto instantiateLocalSummaryRef =
      [&](const SummaryRef &Ref, const CallBase &CB,
          SummaryState &State) -> SummaryRef {
    switch (Ref.K) {
    case SummaryRef::Kind::Arg:
      if (Ref.ArgNo >= 0 && static_cast<unsigned>(Ref.ArgNo) < CB.arg_size())
        return lookupSummaryValue(
            CB.getArgOperand(static_cast<unsigned>(Ref.ArgNo)), State);
      return SummaryRef{};
    case SummaryRef::Kind::ArgPath:
      if (Ref.ArgNo < 0 || static_cast<unsigned>(Ref.ArgNo) >= CB.arg_size())
        return SummaryRef{};
      {
        SummaryRef Base = lookupSummaryValue(
            CB.getArgOperand(static_cast<unsigned>(Ref.ArgNo)), State);
        if (Base.K != SummaryRef::Kind::Arg &&
            Base.K != SummaryRef::Kind::ArgPath)
          return SummaryRef{};
        SummaryRef Result;
        Result.K = SummaryRef::Kind::ArgPath;
        Result.ArgNo = Base.ArgNo;
        Result.Path = Base.Path;
        Result.Path.Fields.append(Ref.Path.Fields.begin(), Ref.Path.Fields.end());
        return Result;
      }
    case SummaryRef::Kind::Null:
    case SummaryRef::Kind::Global:
      return Ref;
    case SummaryRef::Kind::Fresh:
      return Ref;
    case SummaryRef::Kind::Unknown:
      return SummaryRef{};
    }

    return SummaryRef{};
  };

  auto applyLocalSummaryEffects =
      [&](const FunctionSummary &Nested, const CallBase &CB,
          SummaryState &State) {
    for (const StoreEffect &Effect : Nested.StrongStoreEffects) {
      if (!Effect.HasConstantOffset || Effect.BaseArgNo < 0 ||
          static_cast<unsigned>(Effect.BaseArgNo) >= CB.arg_size())
        continue;

      SummaryRef Base = lookupSummaryValue(
          CB.getArgOperand(static_cast<unsigned>(Effect.BaseArgNo)), State);
      if (Base.K != SummaryRef::Kind::Arg &&
          Base.K != SummaryRef::Kind::ArgPath)
        continue;

      SummarySlotKey Slot;
      Slot.BaseArgNo = Base.ArgNo;
      Slot.DestPath = Base.Path;
      Slot.DestPath.Fields.append(Effect.DestPath.Fields.begin(),
                                  Effect.DestPath.Fields.end());
      Slot.ByteOffset = Effect.ByteOffset;

      SummaryRef RHS = instantiateLocalSummaryRef(Effect.RHS, CB, State);
      if (RHS.K == SummaryRef::Kind::Unknown)
        continue;
      State.Stores[Slot] = RHS;
    }
  };

  auto killSummaryCallEffects = [&](const CallBase &CB, SummaryState &State) {
    if (CB.onlyReadsMemory() || CB.doesNotAccessMemory())
      return;

    if (!CB.onlyAccessesArgMemory()) {
      State.Stores.clear();
      StoreEffectsExact = false;
      return;
    }

    for (auto It = State.Stores.begin(); It != State.Stores.end();) {
      bool Reachable = false;
      for (const Value *Arg : CB.args()) {
        if (!isPointerValue(Arg))
          continue;
        SummaryRef ArgRef = lookupSummaryValue(Arg, State);
        if (summaryArgMayReachSlot(ArgRef, It->first)) {
          Reachable = true;
          break;
        }
      }

      if (Reachable) {
        It = State.Stores.erase(It);
        StoreEffectsExact = false;
      } else {
        ++It;
      }
    }
  };

  std::function<SummaryRef(const Instruction &, SummaryState &)>
      evaluateSummaryInstruction;

  std::function<SummaryRef(const PHINode &)> evaluateSummaryPHI =
      [&](const PHINode &PN) -> SummaryRef {
    if (!PN.getType()->isPointerTy())
      return SummaryRef{};

    SummaryRef Common;
    bool Seen = false;
    for (unsigned I = 0, E = PN.getNumIncomingValues(); I < E; ++I) {
      const BasicBlock *Pred = PN.getIncomingBlock(I);
      SummaryState PredState = SummaryOut.lookup(Pred);
      SummaryRef Cur = lookupSummaryValue(PN.getIncomingValue(I), PredState);
      if (Cur.K == SummaryRef::Kind::Unknown)
        return SummaryRef{};
      if (!Seen) {
        Common = Cur;
        Seen = true;
      } else if (!(Common == Cur)) {
        return SummaryRef{};
      }
    }
    return Common;
  };

  auto evaluateSummarySelect = [&](const SelectInst &SI,
                                   SummaryState &State) -> SummaryRef {
    if (!SI.getType()->isPointerTy())
      return SummaryRef{};

    if (const auto *Cond = dyn_cast<ConstantInt>(SI.getCondition()))
      return lookupSummaryValue(Cond->isOne() ? SI.getTrueValue()
                                              : SI.getFalseValue(),
                                State);

    SummaryRef T = lookupSummaryValue(SI.getTrueValue(), State);
    SummaryRef FRef = lookupSummaryValue(SI.getFalseValue(), State);
    return T == FRef ? T : SummaryRef{};
  };

  auto evaluateSummaryGEP = [&](const GEPOperator &GEP,
                                SummaryState &State) -> SummaryRef {
    SummaryRef Base = lookupSummaryValue(GEP.getPointerOperand(), State);
    if (Base.K == SummaryRef::Kind::Unknown)
      return SummaryRef{};

    if (GEP.hasAllZeroIndices())
      return Base;

    if (Base.K != SummaryRef::Kind::Arg && Base.K != SummaryRef::Kind::ArgPath)
      return SummaryRef{};

    AccessPath Path = Base.Path;
    AccessPath Suffix;
    if (!const_cast<EquivDB *>(this)->buildGEPPath(GEP, Suffix))
      return SummaryRef{};

    SummaryRef Result;
    Result.K = SummaryRef::Kind::ArgPath;
    Result.ArgNo = Base.ArgNo;
    Result.Path = std::move(Path);
    Result.Path.Fields.append(Suffix.Fields.begin(), Suffix.Fields.end());
    return Result;
  };

  auto evaluateSummaryLoad = [&](const LoadInst &LI,
                                 SummaryState &State) -> SummaryRef {
    if (!LI.getType()->isPointerTy())
      return SummaryRef{};

    SummarySlotKey Slot;
    if (!tryGetSummarySlot(LI.getPointerOperand(), State, Slot))
      return SummaryRef{};

    auto It = State.Stores.find(Slot);
    if (It == State.Stores.end())
      return SummaryRef{};
    return It->second;
  };

  auto evaluateSummaryCall = [&](const CallBase &CB,
                                 SummaryState &State) -> SummaryRef {
    FunctionSummary Nested;
    if (Function *NestedCallee = CB.getCalledFunction())
      Nested = summarizeFunction(NestedCallee);

    if (Nested.StoreEffectsExact) {
      applyLocalSummaryEffects(Nested, CB, State);
    } else if (mayWriteThroughCall(CB)) {
      killSummaryCallEffects(CB, State);
    }

    SummaryRef Ret = instantiateLocalSummaryRef(Nested.ReturnRef, CB, State);
    if (Ret.K == SummaryRef::Kind::Unknown &&
        (Nested.FreshReturn || isAllocationCall(&CB) ||
         CB.hasRetAttr(Attribute::NoAlias))) {
      Ret.K = SummaryRef::Kind::Fresh;
    }
    return Ret;
  };

  lookupSummaryValue = [&](const Value *V, SummaryState &State) -> SummaryRef {
    if (!isPointerValue(V))
      return SummaryRef{};

    auto It = State.Env.find(V);
    if (It != State.Env.end())
      return It->second;

    if (isa<ConstantPointerNull>(V)) {
      SummaryRef Ref;
      Ref.K = SummaryRef::Kind::Null;
      Ref.Fixed = V;
      return Ref;
    }

    if (isa<GlobalValue>(V)) {
      SummaryRef Ref;
      Ref.K = SummaryRef::Kind::Global;
      Ref.Fixed = V;
      return Ref;
    }

    const Value *Stripped = stripNoopCasts(V);
    if (Stripped != V)
      return lookupSummaryValue(Stripped, State);

    if (const auto *GEP = dyn_cast<GEPOperator>(V))
      return evaluateSummaryGEP(*GEP, State);

    if (const auto *Op = dyn_cast<Operator>(V)) {
      if (Op->getOpcode() == Instruction::IntToPtr) {
        const Value *IntVal = stripNoopArithmetic(Op->getOperand(0));
        if (const auto *PTI = dyn_cast<Operator>(IntVal))
          if (PTI->getOpcode() == Instruction::PtrToInt)
            return lookupSummaryValue(PTI->getOperand(0), State);
      }
    }

    if (const auto *Arg = dyn_cast<Argument>(V)) {
      SummaryRef Ref;
      Ref.K = SummaryRef::Kind::Arg;
      Ref.ArgNo = static_cast<int>(Arg->getArgNo());
      State.Env[V] = Ref;
      return Ref;
    }

    if (isa<AllocaInst>(V) ||
        (isa<CallBase>(V) &&
         (cast<CallBase>(V)->hasRetAttr(Attribute::NoAlias) ||
          isAllocationCall(V)))) {
      SummaryRef Ref;
      Ref.K = SummaryRef::Kind::Fresh;
      State.Env[V] = Ref;
      return Ref;
    }

    return SummaryRef{};
  };

  evaluateSummaryInstruction = [&](const Instruction &I,
                                   SummaryState &State) -> SummaryRef {
    if (!I.getType()->isPointerTy())
      return SummaryRef{};

    if (isa<BitCastInst>(I) || isa<AddrSpaceCastInst>(I) ||
        isa<IntrinsicInst>(I) || isa<FreezeInst>(I)) {
      const Value *Stripped = stripNoopCasts(&I);
      if (Stripped != &I)
        return lookupSummaryValue(Stripped, State);
      if (const auto *FI = dyn_cast<FreezeInst>(&I))
        return lookupSummaryValue(FI->getOperand(0), State);
    }

    if (isa<AllocaInst>(I)) {
      SummaryRef Ref;
      Ref.K = SummaryRef::Kind::Fresh;
      return Ref;
    }

    if (const auto *PN = dyn_cast<PHINode>(&I))
      return evaluateSummaryPHI(*PN);

    if (const auto *SI = dyn_cast<SelectInst>(&I))
      return evaluateSummarySelect(*SI, State);

    if (isa<GetElementPtrInst>(I))
      return evaluateSummaryGEP(*cast<GEPOperator>(&I), State);

    if (const auto *LI = dyn_cast<LoadInst>(&I))
      return evaluateSummaryLoad(*LI, State);

    if (const auto *CB = dyn_cast<CallBase>(&I))
      return evaluateSummaryCall(*CB, State);

    if (const auto *ITP = dyn_cast<IntToPtrInst>(&I)) {
      const Value *IntVal = stripNoopArithmetic(ITP->getOperand(0));
      if (const auto *PTI = dyn_cast<PtrToIntInst>(IntVal))
        return lookupSummaryValue(PTI->getOperand(0), State);
    }

    return SummaryRef{};
  };

  auto transferSummaryBlock = [&](const BasicBlock &BB,
                                  const SummaryState &Input) -> SummaryState {
    SummaryState State = Input;

    for (const Instruction &I : BB) {
      if (!isa<PHINode>(I))
        break;
      if (!I.getType()->isPointerTy())
        continue;
      SummaryRef Ref = evaluateSummaryPHI(cast<PHINode>(I));
      if (Ref.K == SummaryRef::Kind::Unknown)
        State.Env.erase(&I);
      else
        State.Env[&I] = Ref;
    }

    for (const Instruction &I : BB) {
      if (isa<PHINode>(I))
        continue;

      if (const auto *SI = dyn_cast<StoreInst>(&I)) {
        SummarySlotKey Slot;
        SummaryRef PtrRef = lookupSummaryValue(SI->getPointerOperand(), State);
        SummaryRef RHS = lookupSummaryValue(SI->getValueOperand(), State);
        if (tryGetSummarySlot(SI->getPointerOperand(), State, Slot) &&
            RHS.K != SummaryRef::Kind::Unknown) {
          State.Stores[Slot] = RHS;
        } else if (tryGetSummarySlot(SI->getPointerOperand(), State, Slot)) {
          State.Stores.erase(Slot);
          StoreEffectsExact = false;
        } else if (PtrRef.K == SummaryRef::Kind::Arg ||
                   PtrRef.K == SummaryRef::Kind::ArgPath ||
                   PtrRef.K == SummaryRef::Kind::Unknown) {
          StoreEffectsExact = false;
          State.Stores.clear();
        }
        continue;
      }

      if (const auto *CB = dyn_cast<CallBase>(&I)) {
        SummaryRef Ret = evaluateSummaryCall(*CB, State);
        if (I.getType()->isPointerTy()) {
          if (Ret.K == SummaryRef::Kind::Unknown)
            State.Env.erase(&I);
          else
            State.Env[&I] = Ret;
        }
        continue;
      }

      if (!I.getType()->isPointerTy())
        continue;

      SummaryRef Ref = evaluateSummaryInstruction(I, State);
      if (Ref.K == SummaryRef::Kind::Unknown)
        State.Env.erase(&I);
      else
        State.Env[&I] = Ref;
    }

    return State;
  };

  for (const BasicBlock &BB : *Callee) {
    SummaryIn[&BB] = SummaryState();
    SummaryOut[&BB] = SummaryState();
  }

  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (const BasicBlock &BB : *Callee) {
      SummaryState InState;
      if (&BB == &Callee->getEntryBlock()) {
        InState = buildEntryState();
      } else {
        auto PI = pred_begin(&BB);
        auto PE = pred_end(&BB);
        if (PI != PE) {
          InState = SummaryOut.lookup(*PI++);
          for (; PI != PE; ++PI)
            InState = intersectStates(InState, SummaryOut.lookup(*PI));
        }
      }

      SummaryState OutState = transferSummaryBlock(BB, InState);
      if (!statesEqual(SummaryIn.lookup(&BB), InState)) {
        SummaryIn[&BB] = InState;
        Changed = true;
      }
      if (!statesEqual(SummaryOut.lookup(&BB), OutState)) {
        SummaryOut[&BB] = OutState;
        Changed = true;
      }
    }
  }

  bool SawReturn = false;
  SummaryRef CommonReturn;
  bool SeenReturnRef = false;
  SummaryState ExitStores;
  bool HaveExitStores = false;

  for (const BasicBlock &BB : *Callee) {
    const auto *RI = dyn_cast<ReturnInst>(BB.getTerminator());
    if (!RI)
      continue;

    SawReturn = true;
    SummaryState State = SummaryOut.lookup(&BB);
    SummaryRef Cur = lookupSummaryValue(RI->getReturnValue(), State);
    if (!SeenReturnRef) {
      CommonReturn = Cur;
      SeenReturnRef = true;
    } else if (!(CommonReturn == Cur)) {
      CommonReturn = SummaryRef{};
    }

    if (!HaveExitStores) {
      ExitStores = State;
      HaveExitStores = true;
    } else {
      ExitStores = intersectStates(ExitStores, State);
    }
  }

  if (SawReturn)
    Summary.ReturnRef = CommonReturn;
  Summary.FreshReturn = Summary.ReturnRef.K == SummaryRef::Kind::Fresh;
  Summary.StoreEffectsExact = StoreEffectsExact && HaveExitStores;

  if (Summary.StoreEffectsExact) {
    for (const auto &KV : ExitStores.Stores) {
      StoreEffect Effect;
      Effect.BaseArgNo = KV.first.BaseArgNo;
      Effect.DestPath = KV.first.DestPath;
      Effect.ByteOffset = KV.first.ByteOffset;
      Effect.HasConstantOffset = true;
      Effect.RHS = KV.second;
      Summary.StrongStoreEffects.push_back(Effect);
    }
  }

  return Finish(Summary);
}

EquivDB::PointerRef EquivDB::instantiateSummaryRef(const SummaryRef &Ref,
                                                   const CallBase &CB,
                                                   MustAliasState &State) {
  switch (Ref.K) {
  case SummaryRef::Kind::Arg:
    if (Ref.ArgNo >= 0 && static_cast<unsigned>(Ref.ArgNo) < CB.arg_size())
      return lookupValueRef(CB.getArgOperand(static_cast<unsigned>(Ref.ArgNo)),
                            State);
    return PointerRef{};
  case SummaryRef::Kind::ArgPath:
    if (Ref.ArgNo >= 0 && static_cast<unsigned>(Ref.ArgNo) < CB.arg_size()) {
      PointerRef Base =
          lookupValueRef(CB.getArgOperand(static_cast<unsigned>(Ref.ArgNo)),
                         State);
      if (Base.valid())
        return attachRef(State, makeAccessPathRef(Base, Ref.Path));
    }
    return PointerRef{};
  case SummaryRef::Kind::Null:
  case SummaryRef::Kind::Global:
    return attachRef(State, makeRootRef(PointerRef::Kind::Fixed, Ref.Fixed));
  case SummaryRef::Kind::Fresh:
    return attachRef(State, makeRootRef(PointerRef::Kind::Fresh, &CB));
  case SummaryRef::Kind::Unknown:
    return PointerRef{};
  }

  return PointerRef{};
}

void EquivDB::applySummaryEffects(const FunctionSummary &Summary,
                                  const CallBase &CB, MustAliasState &State) {
  for (const StoreEffect &Effect : Summary.StrongStoreEffects) {
    if (!Effect.HasConstantOffset || Effect.BaseArgNo < 0 ||
        static_cast<unsigned>(Effect.BaseArgNo) >= CB.arg_size())
      continue;

    SlotId BaseSlot;
    if (!tryGetSingletonSlot(CB.getArgOperand(static_cast<unsigned>(Effect.BaseArgNo)),
                             BaseSlot))
      continue;

    PointerRef RHS = instantiateSummaryRef(Effect.RHS, CB, State);
    if (!RHS.valid())
      continue;

    SlotId Dest = BaseSlot;
    Dest.ByteOffset += Effect.ByteOffset;
    State.MustStore[Dest] = attachRef(State, RHS);
  }
}

bool EquivDB::slotReachableFromCallArgs(const CallBase &CB, const SlotId &Slot,
                                        MustAliasState &State) {
  PointerRef SlotRoot = attachRef(
      State, makeRootRef(classifyRootKind(Slot.Object), Slot.Object));
  if (!SlotRoot.valid())
    return true;

  for (const Value *Arg : CB.args()) {
    if (!isPointerValue(Arg))
      continue;
    PointerRef ArgRef = lookupValueRef(Arg, State);
    if (!ArgRef.valid())
      continue;
    if (ArgRef.Base == SlotRoot.Base || ArgRef.Var == SlotRoot.Var)
      return true;
  }

  return false;
}

bool EquivDB::isNonEscapingLocalAlloca(const Value *Obj) const {
  auto It = NonEscapingAllocas.find(Obj);
  if (It != NonEscapingAllocas.end())
    return It->second;

  const auto *AI = dyn_cast<AllocaInst>(Obj);
  if (!AI)
    return false;

  SmallVector<const Value *, 8> WorkList;
  SmallPtrSet<const Value *, 16> Seen;
  WorkList.push_back(AI);
  bool Escapes = false;

  while (!WorkList.empty() && !Escapes) {
    const Value *Cur = WorkList.pop_back_val();
    if (!Seen.insert(Cur).second)
      continue;

    for (const User *U : Cur->users()) {
      if (const auto *LI = dyn_cast<LoadInst>(U)) {
        (void)LI;
        continue;
      }

      if (const auto *SI = dyn_cast<StoreInst>(U)) {
        if (SI->getValueOperand() == Cur)
          Escapes = true;
        continue;
      }

      if (isa<GetElementPtrInst>(U) || isa<BitCastInst>(U) ||
          isa<PHINode>(U) || isa<SelectInst>(U) || isa<FreezeInst>(U) ||
          isa<AddrSpaceCastInst>(U)) {
        WorkList.push_back(U);
        continue;
      }

      if (const auto *II = dyn_cast<IntrinsicInst>(U)) {
        switch (II->getIntrinsicID()) {
        case Intrinsic::launder_invariant_group:
        case Intrinsic::strip_invariant_group:
          WorkList.push_back(U);
          continue;
        default:
          Escapes = true;
          continue;
        }
      }

      if (isa<ReturnInst>(U) || isa<CallBase>(U)) {
        Escapes = true;
        continue;
      }

      Escapes = true;
    }
  }

  NonEscapingAllocas[Obj] = !Escapes;
  return !Escapes;
}

void EquivDB::killUnknownCallEffects(const CallBase &CB, MustAliasState &State) {
  if (CB.onlyReadsMemory() || CB.doesNotAccessMemory())
    return;

  for (auto It = State.MustStore.begin(); It != State.MustStore.end();) {
    const SlotId &Slot = It->first;
    bool Preserve = false;

    if (CB.onlyAccessesArgMemory()) {
      Preserve = !slotReachableFromCallArgs(CB, Slot, State);
    } else {
      Preserve = isa<AllocaInst>(Slot.Object) &&
                 isNonEscapingLocalAlloca(Slot.Object) &&
                 !slotReachableFromCallArgs(CB, Slot, State);
    }

    if (Preserve)
      ++It;
    else
      It = State.MustStore.erase(It);
  }
}

EquivDB::PointerRef EquivDB::evaluateCall(const CallBase &CB,
                                          MustAliasState &State) {
  FunctionSummary Summary;
  if (Function *Callee = CB.getCalledFunction())
    Summary = summarizeFunction(Callee);

  if (Summary.StoreEffectsExact) {
    applySummaryEffects(Summary, CB, State);
  } else {
    killUnknownCallEffects(CB, State);
  }

  PointerRef Ret = instantiateSummaryRef(Summary.ReturnRef, CB, State);
  if (!Ret.valid() &&
      (Summary.FreshReturn || isAllocationCall(&CB) ||
       CB.hasRetAttr(Attribute::NoAlias))) {
    Ret = attachRef(State, makeRootRef(PointerRef::Kind::Fresh, &CB));
  }

  return Ret;
}

EquivDB::PointerRef EquivDB::evaluateInstruction(const Instruction &I,
                                                 MustAliasState &State) {
  if (!I.getType()->isPointerTy())
    return PointerRef{};

  if (isa<BitCastInst>(I) || isa<AddrSpaceCastInst>(I) ||
      isa<IntrinsicInst>(I) || isa<FreezeInst>(I)) {
    const Value *Stripped = stripNoopCasts(&I);
    if (Stripped != &I)
      return lookupValueRef(Stripped, State);
    if (const auto *FI = dyn_cast<FreezeInst>(&I))
      return lookupValueRef(FI->getOperand(0), State);
  }

  if (isa<AllocaInst>(I))
    return attachRef(State, makeRootRef(PointerRef::Kind::Fresh, &I));

  if (const auto *PN = dyn_cast<PHINode>(&I))
    return evaluatePHI(*PN);

  if (const auto *SI = dyn_cast<SelectInst>(&I))
    return evaluateSelect(*SI, State);

  if (isa<GetElementPtrInst>(I))
    return evaluateGEP(*cast<GEPOperator>(&I), State);

  if (const auto *LI = dyn_cast<LoadInst>(&I))
    return evaluateLoad(*LI, State);

  if (const auto *CB = dyn_cast<CallBase>(&I))
    return evaluateCall(*CB, State);

  if (const auto *ITP = dyn_cast<IntToPtrInst>(&I)) {
    const Value *IntVal = stripNoopArithmetic(ITP->getOperand(0));
    if (const auto *PTI = dyn_cast<PtrToIntInst>(IntVal))
      return lookupValueRef(PTI->getOperand(0), State);
  }

  return PointerRef{};
}

EquivDB::MustAliasState EquivDB::transferBlock(const BasicBlock &BB,
                                               const MustAliasState &Input) {
  MustAliasState State = Input;

  for (const Instruction &I : BB) {
    if (!isa<PHINode>(I))
      break;
    if (I.getType()->isPointerTy()) {
      PointerRef Ref = evaluatePHI(cast<PHINode>(I));
      if (Ref.valid())
        State.ExprEnv[&I] = attachRef(State, Ref);
      else
        State.ExprEnv.erase(&I);
    }
  }

  for (const Instruction &I : BB) {
    if (isa<PHINode>(I))
      continue;

    if (const auto *SI = dyn_cast<StoreInst>(&I)) {
      SlotId Slot;
      if (tryGetSingletonSlot(SI->getPointerOperand(), Slot)) {
        PointerRef Val = lookupValueRef(SI->getValueOperand(), State);
        if (Val.valid())
          State.MustStore[Slot] = attachRef(State, Val);
        else
          State.MustStore.erase(Slot);
      }
      continue;
    }

    if (const auto *CB = dyn_cast<CallBase>(&I)) {
      PointerRef Ret = evaluateCall(*CB, State);
      if (I.getType()->isPointerTy() && Ret.valid())
        State.ExprEnv[&I] = attachRef(State, Ret);
      else if (I.getType()->isPointerTy())
        State.ExprEnv.erase(&I);
      continue;
    }

    if (!I.getType()->isPointerTy())
      continue;

    PointerRef Ref = evaluateInstruction(I, State);
    if (Ref.valid())
      State.ExprEnv[&I] = attachRef(State, Ref);
    else
      State.ExprEnv.erase(&I);
  }

  refreshStateNodes(State);
  return State;
}

void EquivDB::runDataflow() {
  for (const BasicBlock &BB : F) {
    InStates[&BB] = MustAliasState();
    OutStates[&BB] = MustAliasState();
  }

  bool Changed = true;
  while (Changed) {
    Changed = false;

    for (const BasicBlock &BB : F) {
      MustAliasState In = buildInState(BB);
      MustAliasState Out = transferBlock(BB, In);

      if (!stateEquivalent(InStates.lookup(&BB), In)) {
        InStates[&BB] = In;
        Changed = true;
      }

      if (!stateEquivalent(OutStates.lookup(&BB), Out)) {
        OutStates[&BB] = Out;
        Changed = true;
      }
    }
  }
}

void EquivDB::finalizeQueryState() {
  DefinitionRefs.clear();
  QueryState = MustAliasState();

  MustAliasState Entry = buildEntryState();
  for (const auto &KV : Entry.ExprEnv)
    DefinitionRefs[KV.first] = KV.second;

  for (const BasicBlock &BB : F) {
    const MustAliasState &State = OutStates.lookup(&BB);
    for (const auto &KV : State.ExprEnv) {
      if (isPointerValue(KV.first))
        DefinitionRefs[KV.first] = KV.second;
    }
  }

  for (auto &KV : DefinitionRefs)
    QueryState.ExprEnv[KV.first] = attachRef(QueryState, KV.second);
}

EquivDB::PointerRef EquivDB::lookupQueryRef(const Value *V) const {
  MustAliasState &State = const_cast<MustAliasState &>(QueryState);
  auto It = State.ExprEnv.find(V);
  if (It != State.ExprEnv.end())
    return const_cast<EquivDB *>(this)->attachRef(State, It->second);

  return const_cast<EquivDB *>(this)->lookupValueRef(V, State);
}

bool EquivDB::mustAlias(const Value *A, const Value *B) const {
  if (!isPointerValue(A) || !isPointerValue(B))
    return false;

  PointerRef RefA = lookupQueryRef(A);
  PointerRef RefB = lookupQueryRef(B);
  if (!RefA.valid() || !RefB.valid())
    return false;

  if (RefA == RefB)
    return true;

  NodeId NodeA = QueryState.Graph.getNode(RefA.Var);
  NodeId NodeB = QueryState.Graph.getNode(RefB.Var);
  return NodeA != kNoNode && NodeA == NodeB;
}
