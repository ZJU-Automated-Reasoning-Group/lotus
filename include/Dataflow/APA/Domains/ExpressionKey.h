#ifndef DATAFLOW_APA_DOMAINS_EXPRESSIONKEY_H_
#define DATAFLOW_APA_DOMAINS_EXPRESSIONKEY_H_

#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Type.h"

#include <tuple>
#include <vector>

namespace elimination {

// Canonical key for expression-availability style analyses.
//
// Important: operands/types/metadata are compared by pointer identity. Keys are
// therefore intended for in-process analysis of one LLVM context/module, not as
// a stable cross-process serialization format.
struct ExpressionKey {
  unsigned Opcode = 0;
  unsigned Predicate = 0;
  const llvm::Type *Ty = nullptr;
  std::vector<const llvm::Value *> Ops;
  unsigned FMF = 0;
  unsigned OverflowFlags = 0;
  bool Exact = false;
  bool IsVolatile = false;
  bool IsAtomic = false;
  llvm::AtomicOrdering Ordering = llvm::AtomicOrdering::NotAtomic;
  unsigned SyncScopeID = 0;
  llvm::AAMDNodes AATags{};
  const void *MemoryAccess = nullptr;

  bool operator<(const ExpressionKey &Other) const {
    if (Opcode != Other.Opcode)
      return Opcode < Other.Opcode;
    if (Predicate != Other.Predicate)
      return Predicate < Other.Predicate;
    if (Ty != Other.Ty)
      return Ty < Other.Ty;
    if (FMF != Other.FMF)
      return FMF < Other.FMF;
    if (OverflowFlags != Other.OverflowFlags)
      return OverflowFlags < Other.OverflowFlags;
    if (Exact != Other.Exact)
      return Exact < Other.Exact;
    if (IsVolatile != Other.IsVolatile)
      return IsVolatile < Other.IsVolatile;
    if (IsAtomic != Other.IsAtomic)
      return IsAtomic < Other.IsAtomic;
    if (Ordering != Other.Ordering) {
      return static_cast<unsigned>(Ordering) <
             static_cast<unsigned>(Other.Ordering);
    }
    if (SyncScopeID != Other.SyncScopeID)
      return SyncScopeID < Other.SyncScopeID;
    if (AATags != Other.AATags) {
      return std::tie(AATags.TBAA, AATags.TBAAStruct, AATags.Scope,
                      AATags.NoAlias) <
             std::tie(Other.AATags.TBAA, Other.AATags.TBAAStruct,
                      Other.AATags.Scope, Other.AATags.NoAlias);
    }
    if (MemoryAccess != Other.MemoryAccess) {
      return MemoryAccess < Other.MemoryAccess;
    }
    return Ops < Other.Ops;
  }

  bool operator==(const ExpressionKey &Other) const {
    return Opcode == Other.Opcode && Predicate == Other.Predicate &&
           Ty == Other.Ty && FMF == Other.FMF &&
           OverflowFlags == Other.OverflowFlags && Exact == Other.Exact &&
           IsVolatile == Other.IsVolatile && IsAtomic == Other.IsAtomic &&
           Ordering == Other.Ordering && SyncScopeID == Other.SyncScopeID &&
           AATags == Other.AATags && MemoryAccess == Other.MemoryAccess &&
           Ops == Other.Ops;
  }
};

inline bool isCommutativeOpcode(unsigned Opcode) {
  switch (Opcode) {
  case llvm::Instruction::Add:
  case llvm::Instruction::FAdd:
  case llvm::Instruction::Mul:
  case llvm::Instruction::FMul:
  case llvm::Instruction::And:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
    return true;
  default:
    return false;
  }
}

inline ExpressionKey makeExpressionKey(const llvm::Instruction *Inst) {
  // Normalize instruction semantics into a structural key. Commutative ops and
  // comparisons are canonicalized so equivalent forms map to one key.
  ExpressionKey Key;
  if (Inst == nullptr) {
    return Key;
  }
  Key.Opcode = Inst->getOpcode();
  Key.Ty = Inst->getType();
  Key.Ops.reserve(Inst->getNumOperands());

  if (auto *Cmp = llvm::dyn_cast<llvm::CmpInst>(Inst)) {
    Key.Predicate = static_cast<unsigned>(Cmp->getPredicate());
  }

  if (auto *FPMath = llvm::dyn_cast<llvm::FPMathOperator>(Inst)) {
    Key.FMF = FPMath->getFastMathFlags().any() ? 1u : 0u;
    if (FPMath->hasAllowReassoc())
      Key.FMF |= 1u << 1;
    if (FPMath->hasNoNaNs())
      Key.FMF |= 1u << 2;
    if (FPMath->hasNoInfs())
      Key.FMF |= 1u << 3;
    if (FPMath->hasNoSignedZeros())
      Key.FMF |= 1u << 4;
    if (FPMath->hasAllowReciprocal())
      Key.FMF |= 1u << 5;
    if (FPMath->hasAllowContract())
      Key.FMF |= 1u << 6;
    if (FPMath->hasApproxFunc())
      Key.FMF |= 1u << 7;
  }

  if (auto *OBO = llvm::dyn_cast<llvm::OverflowingBinaryOperator>(Inst)) {
    if (OBO->hasNoUnsignedWrap()) {
      Key.OverflowFlags |= 1u;
    }
    if (OBO->hasNoSignedWrap()) {
      Key.OverflowFlags |= 2u;
    }
  }

  if (auto *PEO = llvm::dyn_cast<llvm::PossiblyExactOperator>(Inst)) {
    Key.Exact = PEO->isExact();
  }

  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
    Key.IsVolatile = Load->isVolatile();
    Key.IsAtomic = Load->isAtomic();
    Key.Ordering = Load->getOrdering();
    Key.SyncScopeID = Load->getSyncScopeID();
    Key.AATags = Load->getAAMetadata();
  }

  for (auto &Op : Inst->operands()) {
    Key.Ops.push_back(Op.get());
  }

  if (Key.Ops.size() == 2 && isCommutativeOpcode(Key.Opcode)) {
    if (Key.Ops[1] < Key.Ops[0]) {
      std::swap(Key.Ops[0], Key.Ops[1]);
    }
  }

  if (auto *Cmp = llvm::dyn_cast<llvm::CmpInst>(Inst)) {
    if (Key.Ops.size() == 2 && Key.Ops[1] < Key.Ops[0]) {
      std::swap(Key.Ops[0], Key.Ops[1]);
      Key.Predicate = static_cast<unsigned>(Cmp->getSwappedPredicate());
    }
  }

  return Key;
}

inline bool isLoadKey(const ExpressionKey &Key) {
  return Key.Opcode == llvm::Instruction::Load;
}

inline const llvm::Value *getLoadPointerOperand(const ExpressionKey &Key) {
  if (!isLoadKey(Key) || Key.Ops.empty()) {
    return nullptr;
  }
  return Key.Ops[0];
}

} // namespace elimination

#endif // DATAFLOW_APA_DOMAINS_EXPRESSIONKEY_H_
