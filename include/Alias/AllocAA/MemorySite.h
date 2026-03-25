/*
 * Copyright 2016 - 2024  Angelo Matni, Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do
 so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file MemorySite.h
 * @brief Data structures for a call-graph-traversing memory-site analysis.
 *
 * ## Design Intent (Future Work)
 *
 * This file defines the skeleton of a more ambitious analysis that would
 * complement `AllocAA` by building a **site graph**: a structured
 * representation of every allocation site reachable from `main`, together
 * with all references to that site and the byte offsets at which those
 * references access it.
 *
 * The intended capabilities are:
 *  - Traverse the call graph to discover all memory sites.
 *  - Aggregate all references (arguments, instructions) to a site so that
 *    the same allocation can be tracked across function boundaries.
 *  - Track "escaping" values — references that flow to contexts the analysis
 *    cannot fully understand — so that the analysis knows when it has lost
 *    precision.
 *  - Answer queries such as:
 *      - Do two memory operations access **different** sites?
 *      - Do two memory operations access the **same** site at the **same**
 *        byte offset?
 *      - What is the aggregated byte offset of a particular reference into
 *        its allocation?
 *  - Provide a tree of `MemorySite` nodes with inter-site offset edges
 *    (e.g., a struct field that is itself a pointer to another allocation).
 *
 * ## Current Status
 *
 * **This analysis is a stub.**  The data structures are defined and
 * `MemorySiteInfo::doesAlias` is implemented, but the code that *builds*
 * the site graph (populating `allocCallSites`, `allocaSites`, and
 * `referenceSites`) does not yet exist.  As a result, `doesAlias` will
 * always return `AllocAAResult::May` for any pair of values because the
 * maps are always empty.
 *
 * The `AllocAAResult` enum and the three structs (`MemoryLayout`,
 * `MemorySite`, `MemoryReference`) are stable and can be used by future
 * implementations.
 */

#ifndef ALLOC_AA_MEMORYSITE_H_
#define ALLOC_AA_MEMORYSITE_H_

#include "Utils/LLVM/SystemHeaders.h"

// Forward declarations (the structs reference each other).
struct MemoryLayout;
struct MemorySite;
struct MemoryReference;

/**
 * @enum AllocAAResult
 * @brief Three-valued alias result used by the memory-site analysis.
 *
 * This mirrors the semantics of LLVM's `AliasResult` but is kept separate
 * so that `MemorySiteInfo` can be used independently of the LLVM AA
 * infrastructure.
 */
enum class AllocAAResult {
  /// The two values definitely do **not** alias.
  No,

  /// The two values **may** alias (conservative / unknown).
  May,

  /// The two values **must** alias (they refer to the same object).
  Must
};

/**
 * @class MemorySiteInfo
 * @brief Top-level query interface for the memory-site analysis.
 *
 * Intended to be constructed once per module (or per analysis scope) and
 * then queried for alias information between pairs of LLVM values.
 *
 * ### Current Status
 * The internal maps (`allocCallSites`, `allocaSites`, `referenceSites`) are
 * never populated by any existing code.  `doesAlias` therefore always returns
 * `AllocAAResult::May`.  A future implementation should add a `build(Module
 * &M, CallGraph &CG)` method (or equivalent) that walks the call graph and
 * fills these maps.
 */
class MemorySiteInfo {
public:
  /**
   * @brief Query whether two LLVM values alias.
   *
   * The logic is:
   *  1. If either value is not tracked in `referenceSites`, return `May`
   *     (the analysis has no information about it).
   *  2. If both values map to the **same** `MemorySite`, return `Must`.
   *  3. If both sites have escaping values, return `May` (the analysis
   *     cannot rule out aliasing through the escaped pointers).
   *  4. Otherwise, at least one site is fully understood and the two values
   *     map to different sites, so return `No`.
   *
   * @param V1  First LLVM value (typically a pointer).
   * @param V2  Second LLVM value (typically a pointer).
   * @return    `AllocAAResult::No`, `May`, or `Must`.
   *
   * @note Because the site graph is never built, this always returns `May`
   *       in the current implementation.
   */
  AllocAAResult doesAlias(Value *V1, Value *V2);

private:
  /**
   * @brief Maps each heap-allocation call site to its `MemorySite`.
   *
   * Key:   A `CallInst` that invokes a known allocator (e.g., `malloc`).
   * Value: The `MemorySite` representing the memory region returned by
   *        that call.
   *
   * @note Not yet populated.
   */
  std::unordered_map<CallInst *, MemorySite *> allocCallSites;

  /**
   * @brief Maps each `alloca` instruction to its `MemorySite`.
   *
   * Key:   An `AllocaInst` (stack allocation).
   * Value: The `MemorySite` representing that stack slot.
   *
   * @note Not yet populated.
   */
  std::unordered_map<AllocaInst *, MemorySite *> allocaSites;

  /**
   * @brief Maps every tracked reference value to the `MemorySite` it
   *        accesses.
   *
   * A "reference" is any LLVM value that is derived from an allocation —
   * e.g., the result of a GEP, a bitcast of an allocation, or a function
   * argument that receives an allocation as an actual parameter.
   *
   * Key:   Any `Value *` that references a known memory site.
   * Value: The `MemorySite` that the value refers into.
   *
   * @note Not yet populated.
   */
  std::unordered_map<Value *, MemorySite *> referenceSites;

  /**
   * @brief Owns all `MemorySite` objects created during analysis.
   *
   * Using a `std::set` of raw pointers with manual ownership is a
   * placeholder; a future implementation should use `std::unique_ptr` or
   * an arena allocator.
   *
   * @note Not yet populated.
   */
  std::set<MemorySite *> memorySites;
};

/**
 * @struct MemoryLayout
 * @brief Describes the parent/child relationships between memory sites.
 *
 * A `MemoryLayout` captures the tree structure of allocations: a "parent"
 * site contains sub-regions ("children") at known byte offsets.  For
 * example, a struct allocation is the parent of its field allocations.
 *
 * Each edge in the tree is represented by a `MemoryReference` that records
 * the offset of the child within the parent.
 *
 * @note This struct is defined but not yet used by any analysis code.
 */
struct MemoryLayout {
  /**
   * @brief For each site, the set of references through which it is accessed
   *        as a child (i.e., the site is a sub-region of some parent).
   *
   * Key:   A `MemorySite *` that is a child in the layout tree.
   * Value: The set of `MemoryReference *` objects that describe how the
   *        child is reached from its parent(s).
   */
  std::unordered_map<MemorySite *, std::set<MemoryReference *>> parents;

  /**
   * @brief For each site, the set of references to its child sub-regions.
   *
   * Key:   A `MemorySite *` that is a parent in the layout tree.
   * Value: The set of `MemoryReference *` objects that describe the
   *        sub-regions (children) embedded within this site.
   */
  std::unordered_map<MemorySite *, std::set<MemoryReference *>> children;
};

/**
 * @struct MemorySite
 * @brief Represents a single contiguous memory allocation.
 *
 * A `MemorySite` is the canonical representation of one allocation — either
 * a heap allocation (from `malloc`/`calloc`/etc.) or a stack allocation
 * (`alloca`).  It aggregates all the ways the allocation is referenced
 * across the program so that alias queries can be answered by comparing
 * sites rather than individual pointer values.
 *
 * @note This struct is defined but not yet populated by any analysis code.
 */
struct MemorySite {
  /**
   * @brief The LLVM value that created this allocation.
   *
   * Either a `CallInst` (heap allocation) or an `AllocaInst` (stack
   * allocation).
   */
  Value *allocation;

  /**
   * @brief The size of this allocation in bits, if statically known.
   *
   * Set to 0 if the size is unknown or dynamically determined.
   */
  uint32_t sizeInBits;

  /**
   * @brief References to this site that arrive via function arguments.
   *
   * When a pointer to this allocation is passed as an argument to a
   * function, the corresponding `Argument *` is mapped to a
   * `MemoryReference` that records the offset at which the argument
   * accesses the allocation.
   *
   * Key:   An `Argument *` in some callee function.
   * Value: The `MemoryReference` describing how that argument relates to
   *        this site.
   */
  std::unordered_map<Argument *, MemoryReference *> argumentReferences;

  /**
   * @brief References to this site that are instruction results.
   *
   * Any instruction whose result is a pointer into this allocation (e.g.,
   * a GEP, a bitcast, or the allocation instruction itself) is recorded
   * here.
   *
   * Key:   An `Instruction *` whose result points into this site.
   * Value: The `MemoryReference` describing the offset of that pointer
   *        within the allocation.
   */
  std::unordered_map<Instruction *, MemoryReference *> instructionReferences;

  /**
   * @brief Values derived from this allocation that have "escaped".
   *
   * A value "escapes" when it flows to a context the analysis cannot fully
   * track — for example, it is stored to a global, passed to an unknown
   * function, or returned from the allocating function.
   *
   * When `escapingValues` is non-empty, the analysis cannot guarantee that
   * all accesses to this site are accounted for, so alias queries involving
   * this site must conservatively return `May`.
   */
  std::set<Value *> escapingValues;
};

/**
 * @struct MemoryReference
 * @brief Describes a single reference into a `MemorySite` at a known offset.
 *
 * A `MemoryReference` connects a pointer value (an instruction result or a
 * function argument) to the `MemorySite` it accesses, and records the byte
 * offset of that access within the allocation.
 *
 * Offset information enables the analysis to distinguish accesses to
 * different fields of the same allocation (must-not-alias at different
 * offsets) from accesses to the same field (must-alias).
 *
 * @note This struct is defined but not yet populated by any analysis code.
 */
struct MemoryReference {
  /**
   * @brief The LLVM value (instruction result or argument) that holds a
   *        pointer into the associated `MemorySite`.
   */
  Value *reference;

  /**
   * @brief The LLVM value representing the dynamic offset of this reference
   *        within the allocation, or `nullptr` if the offset is a compile-
   *        time constant (see `offsetInBits`).
   *
   * For GEP-based references, this is typically the index operand scaled
   * by the element size.
   */
  Value *offsetValue;

  /**
   * @brief Whether the byte offset of this reference has been statically
   *        determined.
   *
   * If `true`, `offsetInBits` holds the exact offset.
   * If `false`, the offset is dynamic and `offsetValue` should be consulted.
   */
  bool offsetDetermined;

  /**
   * @brief The statically-known byte offset (in bits) of this reference
   *        within its `MemorySite`.
   *
   * Valid only when `offsetDetermined` is `true`.  A value of 0 means the
   * reference points to the start of the allocation.
   */
  int32_t offsetInBits;
};

#endif // ALLOC_AA_MEMORYSITE_H_
