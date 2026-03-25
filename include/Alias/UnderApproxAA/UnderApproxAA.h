/**
 * @file UnderApproxAA.h
 * @brief Under-approximation alias analysis using an access-path fixed-point
 * solver
 *
 * This file provides an under-approximation alias analysis for definite
 * must-alias relationships. The implementation uses a forward fixed-point over
 * three domains:
 * - an access-path alias graph
 * - a value environment of exact pointer references
 * - a singleton-slot memory map
 *
 * The analysis is sound but incomplete: it only reports MustAlias when it can
 * guarantee the relationship, otherwise returns MayAlias in the LLVM AA
 * interface.
 *
 * Key Features:
 * - Sound: Never produces false positives (if MustAlias, they definitely alias)
 * - Fast queries after per-function construction
 * - Intra-procedural: Analyzes within a single function at a time
 * - Per-function caching: Equivalence databases are reused until the function
 *   IR fingerprint changes
 * - Optional MemorySSA: recovers exact load values through unique clobbers
 * - Optional DominatorTree: fallback recovery for unique dominating stores
 *
 * Algorithm Overview:
 * The analysis uses a three-phase approach:
 * 1. Build canonical pointer references for SSA values and access paths
 * 2. Solve a forward dataflow problem where block joins are intersections
 * 3. Refine load precision with optional MemorySSA / DominatorTree queries
 *
 * This analysis is useful when:
 * - A lightweight, fast alias analysis is needed
 * - Only definite aliases are required (precision over recall)
 * - Soundness is critical (no false positives allowed)
 * - More sophisticated inter-procedural analyses are unavailable or too
 * expensive
 *
 * See README.md for detailed documentation on rules, examples, and limitations.
 */

#pragma once

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

// Forward declarations for optional analyses
namespace llvm {
class MemorySSA;
class DominatorTree;
class Function;
} // namespace llvm

namespace UnderApprox {

/**
 * @class UnderApproxAA
 * @brief Under-approximation alias analysis implementation
 *
 * This class implements a conservative alias analysis that reasons over exact
 * canonical pointer references. It recognizes patterns such as:
 * - identity, casts, zero-GEPs, and round-trip pointer/integer casts
 * - structural GEP access paths
 * - closed PHI/select nodes after predecessor intersection
 * - singleton-slot loads/stores on allocas, globals, and allocation results
 *
 * With optional MemorySSA:
 * - load recovery through unique clobbering stores or MemoryPhi collapse
 *
 * With optional DominatorTree:
 * - fallback recovery through a unique dominating singleton-slot store
 *
 * The analysis is an under-approximation: it only reports MustAlias when
 * certain. Unknown cases remain MayAlias in the LLVM AA interface, making it
 * suitable for optimizations that require definite knowledge without claiming
 * NoAlias spuriously.
 *
 * Performance:
 * - Construction is per-function and dominated by the fixed-point solver
 * - Query is a fast lookup in the finalized function database
 */
class UnderApproxAA {
public:
  /**
   * @brief Construct an under-approximation alias analysis
   *
   * Creates an analysis instance for the given module. The analysis uses lazy
   * initialization: equivalence databases are built per-function on first
   * query.
   *
   * @param M The LLVM module to analyze
   */
  UnderApproxAA(llvm::Module &M);

  /**
   * @brief Destructor
   *
   * Note: The per-function cache is static and persists across instances.
   * Cached entries are rebuilt automatically when the function IR fingerprint
   * changes.
   */
  ~UnderApproxAA();

  /**
   * @brief Query alias relationship between two values
   *
   * This is a convenience wrapper around mustAlias() that returns an
   * AliasResult. Note: This method is deprecated in favor of alias() with
   * MemoryLocation.
   *
   * @param v1 First value (must be pointer type)
   * @param v2 Second value (must be pointer type)
   * @return AliasResult indicating the alias relationship
   *         (MustAlias when proven, MayAlias otherwise)
   *
   * @deprecated Use alias(MemoryLocation, MemoryLocation) instead
   */
  llvm::AliasResult query(const llvm::Value *v1, const llvm::Value *v2);

  /**
   * @brief Query alias relationship between two memory locations
   *
   * This is the standard LLVM AAResult interface method. It extracts pointer
   * values from memory locations and delegates to mustAlias().
   *
   * Note: Size information in MemoryLocation is ignored - we only check pointer
   * equality. This is acceptable for an under-approximation: if pointers must
   * alias, the memory locations must alias regardless of size.
   *
   * @param loc1 First memory location
   * @param loc2 Second memory location
   * @return AliasResult::MustAlias if pointers must alias, MayAlias otherwise
   */
  llvm::AliasResult alias(const llvm::MemoryLocation &loc1,
                          const llvm::MemoryLocation &loc2);

  /**
   * @brief Check if two values must alias
   *
   * This is the core query method. It checks if two pointer values are in the
   * same equivalence class, indicating they must alias.
   *
   * Behavior:
   * - Returns true only if both values are in the same function and guaranteed
   *   to alias (same equivalence class)
   * - Returns false if values are in different functions (cross-function
   * queries are not supported)
   * - Returns false if values are not valid pointers
   * - Returns false if alias relationship is unknown (conservative)
   *
   * @param v1 First value (should be pointer type)
   * @param v2 Second value (should be pointer type)
   * @return true if v1 and v2 must alias, false otherwise (unknown or different
   * functions)
   *
   * Time complexity: O(α(N)) ≈ O(1) amortized after initial construction
   */
  bool mustAlias(const llvm::Value *v1, const llvm::Value *v2);

  /**
   * @brief Set MemorySSA provider for enhanced analysis
   *
   * When set, singleton-slot loads may recover exact values through unique
   * MemorySSA clobbers or collapsed MemoryPhi nodes.
   *
   * @param Provider A function that returns MemorySSA for a given function
   */
  using MemorySSAProvider = llvm::MemorySSA *(*)(llvm::Function & F);
  void setMemorySSAProvider(MemorySSAProvider Provider) {
    MSSAProvider = Provider;
  }

  /**
   * @brief Set DominatorTree provider for enhanced analysis
   *
   * When set, singleton-slot loads may fall back to a unique dominating store
   * when MemorySSA is unavailable.
   *
   * @param Provider A function that returns DominatorTree for a given function
   */
  using DominatorTreeProvider = llvm::DominatorTree *(*)(llvm::Function & F);
  void setDominatorTreeProvider(DominatorTreeProvider Provider) {
    DTProvider = Provider;
  }

  /**
   * @brief Get the module being analyzed
   * @return Reference to the module
   */
  llvm::Module &getModule() { return _module; }

  /**
   * @brief Invalidate the cached EquivDB for a specific function.
   *
   * This is optional: cached entries are rebuilt automatically when the
   * function IR fingerprint changes. Call this if you want to eagerly discard
   * the cached entry after modifying @p F.
   *
   * @param F The function whose cached analysis should be discarded.
   */
  static void invalidateCache(const llvm::Function *F);

  /**
   * @brief Invalidate all cached EquivDB entries.
   *
   * Clears the entire per-function cache. Use this when starting a fresh
   * compilation pipeline or when eager cache cleanup is preferable.
   */
  static void invalidateAllCaches();

private:
  /// The module being analyzed
  llvm::Module &_module;

  /// Optional MemorySSA provider for store-load forwarding
  MemorySSAProvider MSSAProvider = nullptr;

  /// Optional DominatorTree provider for single-store alloca forwarding
  DominatorTreeProvider DTProvider = nullptr;

  /**
   * @brief Validate that two values are valid for pointer alias queries
   *
   * Checks that both values are non-null and have pointer types. This prevents
   * invalid queries and avoids crashes during analysis.
   *
   * @param v1 First value to validate
   * @param v2 Second value to validate
   * @return true if both values are valid pointers, false otherwise
   */
  bool isValidPointerQuery(const llvm::Value *v1, const llvm::Value *v2) const;
};

} // namespace UnderApprox
