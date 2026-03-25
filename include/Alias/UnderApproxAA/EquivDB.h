/**
 * @file EquivDB.h
 * @brief Function-local must-alias database backed by an access-path state
 *
 * The V2 under-approximation engine models must-alias facts with three
 * first-class domains:
 * - AliasGraph: access-path equivalence between canonical pointer references
 * - expr_env: exact pointer expressions proven for SSA values
 * - must_store: singleton-slot memory facts
 *
 * The solver is a monotone forward dataflow analysis. Block joins are
 * intersections: only facts present in every predecessor survive.
 */

#pragma once

#include "Alias/UnderApproxAA/AliasGraph.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

namespace llvm {
class BasicBlock;
class DataLayout;
class DominatorTree;
class MemorySSA;
class Type;
class Value;
} // namespace llvm

namespace UnderApprox {

class EquivDB {
public:
  explicit EquivDB(llvm::Function &F, llvm::MemorySSA *MSSA = nullptr,
                   llvm::DominatorTree *DT = nullptr);

  bool mustAlias(const llvm::Value *A, const llvm::Value *B) const;

private:
  struct AccessPath {
    llvm::SmallVector<FieldLabel, 4> Fields;

    bool operator==(const AccessPath &Other) const {
      return Fields == Other.Fields;
    }
  };

  struct AccessPathHash {
    size_t operator()(const AccessPath &Path) const;
  };

  struct PointerRef {
    enum class Kind : uint8_t { Invalid, Value, AccessPath, Fixed, Fresh };

    Kind K = Kind::Invalid;
    const llvm::Value *Base = nullptr;
    AccessPath Path;
    VarId Var = static_cast<VarId>(-1);
    NodeId Node = kNoNode;

    bool valid() const { return K != Kind::Invalid && Base; }

    bool operator==(const PointerRef &Other) const {
      return K == Other.K && Base == Other.Base && Path == Other.Path;
    }
  };

  struct PointerKey {
    PointerRef::Kind K = PointerRef::Kind::Invalid;
    const llvm::Value *Base = nullptr;
    AccessPath Path;

    bool operator==(const PointerKey &Other) const {
      return K == Other.K && Base == Other.Base && Path == Other.Path;
    }
  };

  struct PointerKeyHash {
    size_t operator()(const PointerKey &Key) const;
  };

  struct SlotId {
    const llvm::Value *Object = nullptr;
    int64_t ByteOffset = 0;

    bool operator==(const SlotId &Other) const {
      return Object == Other.Object && ByteOffset == Other.ByteOffset;
    }
  };

  struct SlotIdHash {
    size_t operator()(const SlotId &Slot) const;
  };

  struct PathAtom {
    const llvm::Type *SourceElementType = nullptr;
    unsigned OperandIndex = 0;
    std::string Signature;

    bool operator==(const PathAtom &Other) const {
      return SourceElementType == Other.SourceElementType &&
             OperandIndex == Other.OperandIndex &&
             Signature == Other.Signature;
    }
  };

  struct PathAtomHash {
    size_t operator()(const PathAtom &Atom) const;
  };

  struct SummaryRef {
    enum class Kind : uint8_t { Unknown, Arg, ArgPath, Null, Global, Fresh };

    Kind K = Kind::Unknown;
    int ArgNo = -1;
    AccessPath Path;
    const llvm::Value *Fixed = nullptr;

    bool operator==(const SummaryRef &Other) const {
      return K == Other.K && ArgNo == Other.ArgNo && Path == Other.Path &&
             Fixed == Other.Fixed;
    }
  };

  struct StoreEffect {
    int BaseArgNo = -1;
    AccessPath DestPath;
    SummaryRef RHS;
    int64_t ByteOffset = 0;
    bool HasConstantOffset = false;

    bool operator==(const StoreEffect &Other) const {
      return BaseArgNo == Other.BaseArgNo && DestPath == Other.DestPath &&
             RHS == Other.RHS && ByteOffset == Other.ByteOffset &&
             HasConstantOffset == Other.HasConstantOffset;
    }
  };

  struct FunctionSummary {
    SummaryRef ReturnRef;
    llvm::SmallVector<StoreEffect, 4> StrongStoreEffects;
    bool FreshReturn = false;
    bool StoreEffectsExact = false;
  };

  struct MustAliasState {
    AliasGraph Graph;
    llvm::DenseMap<const llvm::Value *, PointerRef> ExprEnv;
    std::unordered_map<SlotId, PointerRef, SlotIdHash> MustStore;
  };

  // Core solver.
  MustAliasState buildEntryState();
  MustAliasState buildInState(const llvm::BasicBlock &BB);
  MustAliasState transferBlock(const llvm::BasicBlock &BB,
                               const MustAliasState &Input);
  void runDataflow();
  void finalizeQueryState();

  // State lattice.
  MustAliasState intersectStates(const MustAliasState &LHS,
                                 const MustAliasState &RHS);
  bool stateEquivalent(const MustAliasState &LHS, const MustAliasState &RHS);
  void refreshStateNodes(MustAliasState &State);

  // Pointer-reference interning.
  PointerRef makeRootRef(PointerRef::Kind Kind, const llvm::Value *Base);
  PointerRef makeAccessPathRef(const PointerRef &BaseRef,
                               const AccessPath &Suffix);
  PointerRef attachRef(MustAliasState &State, const PointerRef &Ref);
  PointerRef rootOf(const PointerRef &Ref) const;
  PointerRef::Kind classifyRootKind(const llvm::Value *Base) const;

  // Value interpretation.
  PointerRef lookupValueRef(const llvm::Value *V, MustAliasState &State);
  PointerRef evaluateInstruction(const llvm::Instruction &I,
                                 MustAliasState &State);
  PointerRef evaluatePHI(const llvm::PHINode &PN);
  PointerRef evaluateSelect(const llvm::SelectInst &SI,
                            MustAliasState &State);
  PointerRef evaluateGEP(const llvm::GEPOperator &GEP, MustAliasState &State);
  PointerRef evaluateLoad(const llvm::LoadInst &LI, MustAliasState &State);
  PointerRef evaluateCall(const llvm::CallBase &CB, MustAliasState &State);
  PointerRef recoverLoadFromMemorySSA(const llvm::LoadInst &LI,
                                      MustAliasState &State);
  PointerRef recoverLoadFromDominatorTree(const llvm::LoadInst &LI,
                                          MustAliasState &State);

  // Memory model.
  bool tryGetSingletonSlot(const llvm::Value *Ptr, SlotId &Out) const;
  void killUnknownCallEffects(const llvm::CallBase &CB, MustAliasState &State);
  bool isNonEscapingLocalAlloca(const llvm::Value *Obj) const;
  bool slotReachableFromCallArgs(const llvm::CallBase &CB, const SlotId &Slot,
                                 MustAliasState &State);
  bool dominatesInst(const llvm::Instruction *Def,
                     const llvm::Instruction *Use) const;

  // Path construction.
  bool buildGEPPath(const llvm::GEPOperator &GEP, AccessPath &Out);
  FieldLabel internPathAtom(const PathAtom &Atom);
  std::string normalizeIndexExpr(const llvm::Value *V,
                                 unsigned Depth = 0) const;

  // Summaries.
  FunctionSummary summarizeFunction(const llvm::Function *Callee) const;
  SummaryRef summarizePointerValue(const llvm::Value *V,
                                   const llvm::Function *Callee,
                                   unsigned Depth = 0) const;
  PointerRef instantiateSummaryRef(const SummaryRef &Ref,
                                   const llvm::CallBase &CB,
                                   MustAliasState &State);
  void applySummaryEffects(const FunctionSummary &Summary,
                           const llvm::CallBase &CB, MustAliasState &State);

  // Queries.
  PointerRef lookupQueryRef(const llvm::Value *V) const;

  const llvm::DataLayout &DL;
  llvm::Function &F;
  llvm::MemorySSA *MSSA;
  llvm::DominatorTree *DT;

  llvm::DenseMap<const llvm::BasicBlock *, MustAliasState> InStates;
  llvm::DenseMap<const llvm::BasicBlock *, MustAliasState> OutStates;
  llvm::DenseMap<const llvm::Value *, PointerRef> DefinitionRefs;
  mutable MustAliasState QueryState;

  std::unordered_map<PointerKey, VarId, PointerKeyHash> PointerInterner;
  llvm::DenseMap<VarId, PointerRef> RefByVar;
  std::unordered_map<PathAtom, FieldLabel, PathAtomHash> PathInterner;
  mutable llvm::DenseMap<const llvm::Value *, bool> NonEscapingAllocas;
  mutable std::unordered_map<const llvm::Function *, FunctionSummary>
      SummaryCache;
  mutable std::unordered_set<const llvm::Function *> SummaryInProgress;
  mutable VarId NextVarId = 0;
  FieldLabel NextFieldLabel = 1;
};

} // namespace UnderApprox
