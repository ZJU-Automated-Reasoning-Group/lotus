#ifndef LOTUS_DATAFLOW_MONO_SUPPORT_ALIASUTILS_H_
#define LOTUS_DATAFLOW_MONO_SUPPORT_ALIASUTILS_H_

// Shared alias-classification utilities for monotone dataflow analyses.
//
// Previously this logic was copy-pasted into IntraConstantPropagation,
// IntraFullConstantPropagation, InterConstantPropagation,
// InterFullConstantPropagation, and InterTaintAnalysis.

#include "llvm/IR/Value.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <vector>

namespace mono {
namespace alias_utils {

struct AliasPartition {
  std::vector<const llvm::Value *> MustAliases;
  std::vector<const llvm::Value *> MayAliases;
};

/// Classify every pointer-typed key in \p Candidates as a must-alias or
/// may-alias of \p Ptr.
///
/// Rules (in priority order):
///  1. Same stripped base object → MustAlias.
///  2. Pointer identity (Ptr == Candidate) → MustAlias.
///  3. No alias analysis available → MayAlias (conservative).
///  4. AA says MustAlias → MustAlias.
///  5. AA says NoAlias → skip.
///  6. Otherwise → MayAlias.
///
/// \p Ptr itself is always included in the result (as MustAlias of itself).
template <typename CandidateRange>
AliasPartition classifyAliases(const llvm::Value *Ptr,
                               const CandidateRange &Candidates,
                               lotus::AliasAnalysisWrapper *AA) {
  AliasPartition AP;
  if (Ptr == nullptr) {
    return AP;
  }

  const bool HaveAA = AA != nullptr && AA->isInitialized();
  const llvm::Value *PtrBase = Ptr->stripPointerCasts();

  auto Add = [&](const llvm::Value *Candidate) {
    if (Candidate == nullptr || !Candidate->getType()->isPointerTy()) {
      return;
    }
    // Identity check first.
    if (Candidate == Ptr) {
      AP.MustAliases.push_back(Candidate);
      return;
    }
    // Same stripped base → must-alias.
    const llvm::Value *CandidateBase = Candidate->stripPointerCasts();
    if (PtrBase != nullptr && CandidateBase != nullptr &&
        PtrBase == CandidateBase) {
      AP.MustAliases.push_back(Candidate);
      return;
    }
    if (!HaveAA || !Ptr->getType()->isPointerTy()) {
      AP.MayAliases.push_back(Candidate);
      return;
    }
    auto Res = AA->query(Ptr, Candidate);
    if (Res == llvm::AliasResult::MustAlias) {
      AP.MustAliases.push_back(Candidate);
    } else if (Res != llvm::AliasResult::NoAlias) {
      AP.MayAliases.push_back(Candidate);
    }
  };

  // Always include Ptr itself.
  Add(Ptr);
  for (const auto *C : Candidates) {
    Add(C);
  }
  return AP;
}

/// Returns true if \p V (or any of its aliases present in \p Facts) is
/// considered tainted/uninitialized.
///
/// Without alias analysis every pointer in \p Facts is conservatively treated
/// as a potential alias of \p V (this is still over-approximate but
/// is the best we can do without AA; the caller should prefer providing AA).
template <typename FactSet>
bool isAliasedInSet(const llvm::Value *V, const FactSet &Facts,
                    lotus::AliasAnalysisWrapper *AA) {
  if (V == nullptr) {
    return false;
  }
  // Direct membership.
  if (Facts.count(const_cast<llvm::Value *>(V))) {
    return true;
  }
  if (!V->getType()->isPointerTy()) {
    return false;
  }

  const bool HaveAA = AA != nullptr && AA->isInitialized();
  const llvm::Value *VBase = V->stripPointerCasts();

  for (auto *Candidate : Facts) {
    if (Candidate == nullptr || !Candidate->getType()->isPointerTy()) {
      continue;
    }
    // Same stripped base → alias.
    const llvm::Value *CandidateBase = Candidate->stripPointerCasts();
    if (VBase != nullptr && CandidateBase != nullptr &&
        VBase == CandidateBase) {
      return true;
    }
    if (!HaveAA) {
      // Without AA we must be conservative, but only for pointer
      // types (already checked above).  We still return true here because we
      // cannot rule out aliasing.
      return true;
    }
    if (AA->query(V, Candidate) != llvm::AliasResult::NoAlias) {
      return true;
    }
  }
  return false;
}

} // namespace alias_utils
} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_SUPPORT_ALIASUTILS_H_
