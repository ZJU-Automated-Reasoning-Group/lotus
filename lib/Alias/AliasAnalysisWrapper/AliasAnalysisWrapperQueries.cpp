/**
 * @file AliasAnalysisWrapperQueries.cpp
 * @brief Public query interface methods
 * 
 * This file implements the public query methods of AliasAnalysisWrapper:
 * - query() - Main alias query interface (Value and MemoryLocation variants)
 * - mayAlias() - Convenience method for may-alias queries
 * - mustAlias() - Convenience method for must-alias queries
 * - mayNull() - Check if a pointer may be null
 * - getPointsToSet() - Get points-to set for a pointer
 * - getAliasSet() - Get alias set for a value
 */

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Alias/DDA/FlowDDA.h"
#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/SparrowAA/AndersenAA.h"
#include "Alias/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Constants.h>

using namespace llvm;
using namespace lotus;

namespace {
/**
 * @brief Combine alias results from multiple sound alias analysis backends
 * 
 * Implements conservative merging of results from multiple backends, prioritizing
 * precision. See AliasAnalysisWrapperCore.cpp for detailed documentation.
 * 
 * @param Results Array of alias results from different backends
 * @return Combined alias result
 */
llvm::AliasResult combineAliasResults(llvm::ArrayRef<llvm::AliasResult> Results) {
  bool SawNo = false, SawMust = false, SawPartial = false;
  for (auto R : Results) {
    if (R == llvm::AliasResult::NoAlias) SawNo = true;
    else if (R == llvm::AliasResult::MustAlias) SawMust = true;
    else if (R == llvm::AliasResult::PartialAlias) SawPartial = true;
  }

  // Contradiction (shouldn't happen with sound analyses): fall back to MayAlias.
  if (SawNo && SawMust) return llvm::AliasResult::MayAlias;
  if (SawNo) return llvm::AliasResult::NoAlias;
  if (SawMust) return llvm::AliasResult::MustAlias;
  if (SawPartial) return llvm::AliasResult::PartialAlias;
  return llvm::AliasResult::MayAlias;
}
} // namespace

/**
 * @brief Query the alias relationship between two pointer values
 * 
 * Determines whether two pointer values may alias, must alias, or do not alias.
 * This is the primary query interface for alias analysis.
 * 
 * @param v1 First pointer value to check
 * @param v2 Second pointer value to check
 * @return AliasResult indicating the relationship:
 *         - NoAlias: The pointers definitely do not alias
 *         - MayAlias: The pointers may alias (conservative answer)
 *         - PartialAlias: The pointers partially alias (one is a subset of the other)
 *         - MustAlias: The pointers must alias (they always point to the same memory)
 * 
 * @note Returns NoAlias immediately if either value is null or not a pointer type.
 *       This is a fast-path optimization for invalid queries.
 * @note The actual query is delegated to queryBackend() which routes to the
 *       appropriate backend based on the configuration.
 */
AliasResult AliasAnalysisWrapper::query(const Value *v1, const Value *v2) {
  if (!isValidPointerQuery(v1, v2)) 
      return AliasResult::NoAlias;
  return queryBackend(v1, v2);
}

/**
 * @brief Query the alias relationship between two memory locations
 * 
 * Determines whether two memory locations may alias. This variant works with
 * MemoryLocation objects which include size information, making it more precise
 * than the Value-based query for some analyses.
 * 
 * @param loc1 First memory location to check
 * @param loc2 Second memory location to check
 * @return AliasResult indicating the relationship between the locations
 * 
 * @note In Combined mode, queries multiple backends (Andersen, DyckAA, LLVM AA)
 *       and merges their results conservatively.
 * @note Falls back to Value-based query if the backend doesn't support
 *       MemoryLocation queries directly.
 * @note Returns MayAlias if the wrapper is not initialized.
 */
AliasResult AliasAnalysisWrapper::query(const MemoryLocation &loc1, const MemoryLocation &loc2) {
  if (!_initialized) return AliasResult::MayAlias;
  
  // Check for null pointers in memory locations
  if (!loc1.Ptr || !loc2.Ptr) {
    // If either pointer is null, they can't alias
    return AliasResult::NoAlias;
  }
  
  if (_config.impl == AAConfig::Implementation::Combined) {
    SmallVector<AliasResult, 3> Rs;
    if (_andersen_aa) Rs.push_back(_andersen_aa->alias(loc1, loc2));
    if (_dyck_aa) {
      // stripPointerCasts() should not return null for valid pointers, but be defensive
      auto *p1 = const_cast<Value *>(loc1.Ptr->stripPointerCasts());
      auto *p2 = const_cast<Value *>(loc2.Ptr->stripPointerCasts());
      if (p1 && p2) {
        Rs.push_back(_dyck_aa->mayAlias(p1, p2) ? AliasResult::MayAlias : AliasResult::NoAlias);
      }
    }
    if (_llvm_aa) Rs.push_back(_llvm_aa->alias(loc1, loc2));
    // If no backends returned results, return conservative MayAlias
    if (Rs.empty()) return AliasResult::MayAlias;
    return combineAliasResults(Rs);
  }
  if (_andersen_aa) return _andersen_aa->alias(loc1, loc2);
  if (_llvm_aa) return _llvm_aa->alias(loc1, loc2);
  return query(loc1.Ptr, loc2.Ptr);
}

/**
 * @brief Check if two pointer values may alias
 * 
 * Convenience method that returns true if the pointers may alias (i.e., the
 * query result is not NoAlias). This includes MayAlias, PartialAlias, and MustAlias.
 * 
 * @param v1 First pointer value
 * @param v2 Second pointer value
 * @return true if the pointers may alias, false if they definitely do not alias
 * 
 * @note This is equivalent to: query(v1, v2) != AliasResult::NoAlias
 */
bool AliasAnalysisWrapper::mayAlias(const Value *v1, const Value *v2) {
  return query(v1, v2) != AliasResult::NoAlias;
}

/**
 * @brief Check if two pointer values must alias
 * 
 * Convenience method that returns true only if the pointers must alias
 * (i.e., they always point to the same memory location).
 * 
 * @param v1 First pointer value
 * @param v2 Second pointer value
 * @return true if the pointers must alias, false otherwise
 * 
 * @note This is a stronger guarantee than mayAlias(). Only returns true
 *       when the analysis can prove MustAlias.
 */
bool AliasAnalysisWrapper::mustAlias(const Value *v1, const Value *v2) {
  return query(v1, v2) == AliasResult::MustAlias;
}

/**
 * @brief Check if a pointer value may be null
 * 
 * Determines whether a pointer value might be null at runtime. This is useful
 * for null pointer analysis and safety checks.
 * 
 * @param v Pointer value to check
 * @return true if the pointer may be null, false if it is definitely not null
 * 
 * @note Returns false if v is null or not a pointer type
 * @note Returns true immediately for ConstantPointerNull values
 * @note Currently only DyckAA backend supports null analysis. If DyckAA is
 *       not available, returns true conservatively.
 * @note Conservative default: returns true if analysis cannot prove non-null
 */
bool AliasAnalysisWrapper::mayNull(const Value *v) {
  if (!v || !v->getType()->isPointerTy()) return false;
  if (isa<ConstantPointerNull>(v)) return true;
  if (_dyck_aa && _initialized) return _dyck_aa->mayNull(const_cast<Value *>(v));
  return true;
}

/**
 * @brief Get the points-to set for a pointer value
 * 
 * Retrieves the set of all values that a pointer may point to. This is useful
 * for pointer analysis clients that need to know all possible targets.
 * 
 * @param ptr Pointer value to analyze
 * @param ptsSet Output parameter that will be filled with the points-to set
 * @return true if the points-to set was successfully retrieved, false otherwise
 * 
 * @note The ptsSet vector is cleared before being filled
 * @note Currently only supported by SparrowAA (Andersen) backend
 * @note Returns false if ptr is null, not a pointer type, or the backend
 *       is not available/initialized
 */
bool AliasAnalysisWrapper::getPointsToSet(const Value *ptr, std::vector<const Value *> &ptsSet) {
  if (!ptr || !ptr->getType()->isPointerTy()) return false;
  ptsSet.clear();
  if (_andersen_aa && _initialized && _andersen_aa->getPointsToSet(ptr, ptsSet))
    return true;
  if (_dda_aa && _initialized && _dda_aa->getPointsToSet(ptr, ptsSet))
    return true;
  return false;
}

bool AliasAnalysisWrapper::getPointsToSetSize(const Value *ptr, size_t &outSize) {
  if (!ptr || !ptr->getType()->isPointerTy()) return false;
  outSize = 0;
  if (!_initialized) return false;
  if (_andersen_aa) {
    std::vector<const Value *> ptsSet;
    if (_andersen_aa->getPointsToSet(ptr, ptsSet)) {
      outSize = ptsSet.size();
      return true;
    }
    return false;
  }
  if (_tpa_aa) {
    const Value *stripped = ptr->stripPointerCasts();
    if (!stripped) return false;
    tpa::PtsSet pts = _tpa_aa->getPtsSet(stripped);
    outSize = pts.size();
    return true;
  }
  if (_dda_aa) {
    std::vector<const Value *> ptsSet;
    if (_dda_aa->getPointsToSet(ptr, ptsSet)) {
      outSize = ptsSet.size();
      return true;
    }
    return false;
  }
  return false;
}

void AliasAnalysisWrapper::getIndirectCallTargets(CallBase *call,
                                                  std::vector<const llvm::Function *> &targets) {
  targets.clear();
  if (!_initialized || !call)
    return;
  if (call->getCalledFunction()) {
    targets.push_back(call->getCalledFunction());
    return;
  }
  const Value *calledVal = call->getCalledOperand();
  if (!calledVal || !calledVal->getType()->isPointerTy())
    return;
  if (_andersen_aa) {
    std::vector<const Value *> ptsSet;
    if (_andersen_aa->getPointsToSet(calledVal, ptsSet)) {
      for (const Value *v : ptsSet) {
        if (const auto *F = dyn_cast<llvm::Function>(v))
          targets.push_back(F);
      }
    }
    return;
  }
  if (_dyck_aa) {
    auto *Caller = call->getFunction();
    auto *CallGraph = _dyck_aa->getDyckCallGraph();
    if (!Caller || !CallGraph) {
      return;
    }
    auto *CallerNode = CallGraph->getFunction(Caller);
    if (!CallerNode) {
      return;
    }
    auto *DyckCall = CallerNode->getCall(call);
    auto *PtrCall = dyn_cast_or_null<PointerCall>(DyckCall);
    if (!PtrCall) {
      return;
    }
    for (auto *F : *PtrCall) {
      if (F != nullptr) {
        targets.push_back(F);
      }
    }
    return;
  }
  if (_tpa_aa) {
    std::vector<const llvm::Function *> tpaCallees = _tpa_aa->getCallees(call, nullptr);
    targets.assign(tpaCallees.begin(), tpaCallees.end());
  }
}

/**
 * @brief Get the alias set for a value
 * 
 * Retrieves the set of all values that may alias with the given value. This
 * is useful for finding all values that might point to the same memory location.
 * 
 * @param v Pointer value to analyze
 * @param aliasSet Output parameter that will be filled with the alias set
 * @return true if the alias set was successfully retrieved, false otherwise
 * 
 * @note The aliasSet vector is cleared before being filled
 * @note Currently only supported by DyckAA backend
 * @note Returns false if v is null, not a pointer type, or DyckAA is not
 *       available/initialized
 */
bool AliasAnalysisWrapper::getAliasSet(const Value *v, std::vector<const Value *> &aliasSet) {
  if (!v || !v->getType()->isPointerTy()) return false;
  aliasSet.clear();
  if (_dyck_aa && _initialized) {
    if (const auto *dyckSet = _dyck_aa->getAliasSet(const_cast<Value *>(v))) {
      aliasSet.assign(dyckSet->begin(), dyckSet->end());
      return true;
    }
  }
  return false;
}

/**
 * @brief Check if a pointer query is valid
 * 
 * Validates that both values are non-null and are pointer types. This is used
 * as a fast-path check before performing expensive alias queries.
 * 
 * @param v1 First value to check
 * @param v2 Second value to check
 * @return true if both values are valid pointers, false otherwise
 * 
 * @note This is a const method that performs basic type checking only.
 *       It does not verify that the values are from the same module or
 *       that the analysis is initialized.
 */
bool AliasAnalysisWrapper::isValidPointerQuery(const Value *v1, const Value *v2) const {
  return v1 && v2 && v1->getType()->isPointerTy() && v2->getType()->isPointerTy();
}
