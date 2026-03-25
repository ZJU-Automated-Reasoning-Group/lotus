/**
 * @file AliasAnalysisWrapperBackend.cpp
 * @brief Backend query routing logic
 * 
 * This file implements the core backend query routing logic that dispatches
 * alias queries to the appropriate underlying alias analysis backend based on
 * the configured AAConfig. It handles:
 * - Routing queries to individual backends (Andersen, DyckAA, CFL, TPA, etc.)
 * - Combined mode that merges results from multiple backends
 * - Helper function for combining alias results
 */

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Alias/AllocAA/AllocAA.h"
#include "Alias/DDA/FlowDDA.h"
#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/SparrowAA/AndersenAA.h"
#include "Alias/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/TPA/PointerAnalysis/Support/PtsSet.h"
#include "Alias/UnderApproxAA/UnderApproxAA.h"
#include "Alias/seadsa/SeaDsaAliasAnalysis.hh"
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CFLAndersAliasAnalysis.h>
#include <llvm/Analysis/CFLSteensAliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>

using namespace llvm;
using namespace lotus;

namespace {
/**
 * @brief Combine alias results from multiple sound alias analysis backends
 * 
 * Implements conservative merging of results from multiple backends. See
 * AliasAnalysisWrapperCore.cpp for detailed documentation.
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
 * @brief Route alias queries to the appropriate backend based on configuration
 * 
 * This is the core query routing logic that dispatches alias queries to the
 * correct backend based on the configured implementation. It handles:
 * - Combined mode: queries multiple backends and merges results
 * - Individual backends: SparrowAA, DyckAA, TPA, CFL analyses, etc.
 * - Fast paths: pointer cast stripping, same-value detection
 * 
 * @param v1 First pointer value
 * @param v2 Second pointer value
 * @return AliasResult from the appropriate backend, or MayAlias if uninitialized
 * 
 * @note Returns MayAlias conservatively if the wrapper is not initialized
 * @note Strips pointer casts before querying backends for better precision
 * @note Returns MustAlias immediately if both values (after cast stripping)
 *       are the same
 * @note In Combined mode, queries multiple backends and uses combineAliasResults()
 *       to merge results conservatively
 * @note TPA backend uses points-to set intersection to determine aliasing:
 *       - If sets don't intersect -> NoAlias
 *       - If both are singletons and equal -> MustAlias
 *       - Otherwise -> MayAlias
 */
AliasResult AliasAnalysisWrapper::queryBackend(const Value *v1, const Value *v2) {
  if (!_initialized) return AliasResult::MayAlias;

  // stripPointerCasts() should not return null for valid pointers, but be defensive
  const auto *v1s = v1->stripPointerCasts();
  const auto *v2s = v2->stripPointerCasts();
  if (!v1s || !v2s) return AliasResult::MayAlias; // Conservative fallback
  if (v1s == v2s) return AliasResult::MustAlias;

  auto mkLoc = [](const Value *v) { return MemoryLocation(v, LocationSize::beforeOrAfterPointer(), AAMDNodes()); };

  if (_config.impl == AAConfig::Implementation::Combined) {
    SmallVector<AliasResult, 3> Rs;
    if (_andersen_aa) Rs.push_back(_andersen_aa->alias(mkLoc(v1s), mkLoc(v2s)));
    if (_dyck_aa && v1s && v2s) {
      Rs.push_back(_dyck_aa->mayAlias(const_cast<Value *>(v1s), const_cast<Value *>(v2s))
                                   ? AliasResult::MayAlias
                                   : AliasResult::NoAlias);
    }
    if (_llvm_aa) Rs.push_back(_llvm_aa->alias(mkLoc(v1), mkLoc(v2)));
    // If no backends returned results, return conservative MayAlias
    if (Rs.empty()) return AliasResult::MayAlias;
    return combineAliasResults(Rs);
  }

  if (_andersen_aa) return _andersen_aa->alias(mkLoc(v1s), mkLoc(v2s));
  if (_dda_aa) return _dda_aa->mayAlias(v1s, v2s) ? AliasResult::MayAlias : AliasResult::NoAlias;
  if (_dyck_aa) return _dyck_aa->mayAlias(const_cast<Value *>(v1s), const_cast<Value *>(v2s)) 
                       ? AliasResult::MayAlias : AliasResult::NoAlias;
  if (_llvm_aa) return _llvm_aa->alias(mkLoc(v1), mkLoc(v2));
  if (_underapprox_aa) return _underapprox_aa->mustAlias(v1, v2) ? AliasResult::MustAlias : AliasResult::NoAlias;
  if (_cflanders_pass) return _cflanders_pass->getResult().query(mkLoc(v1), mkLoc(v2));
  if (_cflsteens_pass) return _cflsteens_pass->getResult().query(mkLoc(v1), mkLoc(v2));
  if (_seadsa_aa) { SimpleAAQueryInfo AAQI; return _seadsa_aa->alias(mkLoc(v1), mkLoc(v2), AAQI); }
  if (_alloc_aa) return _alloc_aa->canPointToTheSameObject(const_cast<Value *>(v1), const_cast<Value *>(v2))
                        ? AliasResult::MayAlias : AliasResult::NoAlias;
  if (_tpa_aa) {
    // Get points-to sets for both values (context-insensitive)
    tpa::PtsSet pts1 = _tpa_aa->getPtsSet(v1s);
    tpa::PtsSet pts2 = _tpa_aa->getPtsSet(v2s);
    
    // Check if sets are empty (value not tracked)
    if (pts1.empty() || pts2.empty()) {
      // If we can't find one, conservatively return MayAlias
      return AliasResult::MayAlias;
    }
    
    // Check if sets intersect
    auto common = tpa::PtsSet::intersects(pts1, pts2);
    if (common.empty()) {
      return AliasResult::NoAlias;
    }
    
    // If both sets are singletons and equal, it's MustAlias
    if (pts1.size() == 1 && pts2.size() == 1 && pts1 == pts2) {
      return AliasResult::MustAlias;
    }
    
    // Otherwise, they may alias
    return AliasResult::MayAlias;
  }
  
  return AliasResult::MayAlias;
}
