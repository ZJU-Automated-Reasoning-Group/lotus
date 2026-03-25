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
 * @file AllocAA.h
 * @brief Allocation-site-aware alias analysis for primitive arrays and
 *        contiguously-allocated memory regions.
 *
 * ## Purpose
 *
 * AllocAA is a lightweight, **under-approximate** alias analysis that answers
 * two focused questions about LLVM IR values:
 *
 *  1. **Can two pointer values point to the same memory object?**
 *     (`canPointToTheSameObject`)
 *  2. **Does a load/store instruction access a "primitive array"?**
 *     (`getPrimitiveArrayAccess`)
 *
 * It is designed to support the Program Dependence Graph (PDG) and
 * loop-parallelisation passes, where conservative "may-alias" answers are
 * acceptable but definitive "no-alias" answers unlock optimisations.
 *
 * ## What AllocAA Is NOT
 *
 * - It is **not** a full flow-sensitive or context-sensitive points-to
 *   analysis.  It does not compute points-to sets.
 * - It does **not** implement the LLVM `AAResultBase` interface; it is a
 *   standalone utility class consumed directly by passes.
 * - It does **not** handle all pointer patterns — when in doubt it returns
 *   a conservative answer (may-alias / true).
 *
 * ## Key Concepts
 *
 * ### Primitive Array
 * A contiguous block of memory (allocated by `malloc`/`calloc`/similar, or
 * declared as a global) whose address never "escapes" to an unknown context.
 * Concretely, every use of the allocation must be one of:
 *   - A GEP instruction whose result does not escape.
 *   - A call to a read-only library function.
 *   - A cast whose result satisfies the same constraints recursively.
 *
 * Identifying primitive arrays allows the analysis to conclude that two
 * accesses through different base pointers cannot alias.
 *
 * ### Call-Graph Scope
 * All collection passes are restricted to functions reachable from `main`
 * (stored in `CGUnderMain`).  This avoids wasting time on dead code and
 * keeps the analysis focused on the live program.
 *
 * ## Construction
 *
 * AllocAA requires three lazy analysis accessors supplied by the caller:
 *   - `getSCEV`     — returns `ScalarEvolution` for a given function.
 *   - `getLoopInfo` — returns `LoopInfo` for a given function.
 *   - `getCallGraph`— returns the module-level `CallGraph`.
 *
 * These are stored as `std::function` objects so that the caller can wrap
 * legacy pass-manager analyses, new pass-manager analyses, or stubs for
 * testing.
 *
 * ## Usage Example
 *
 * ```cpp
 * AllocAA aa(M,
 *   [&](Function &F) -> ScalarEvolution & { return
 * getAnalysis<ScalarEvolutionWrapperPass>(F).getSE(); },
 *   [&](Function &F) -> LoopInfo &        { return
 * getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo(); },
 *   [&]()            -> CallGraph &       { return
 * getAnalysis<CallGraphWrapperPass>().getCallGraph(); });
 *
 * // Query whether two pointers can alias:
 * if (!aa.canPointToTheSameObject(p1, p2)) { ... }
 *
 * // Identify a primitive-array access:
 * auto [baseArray, gep] = aa.getPrimitiveArrayAccess(loadInst);
 * if (baseArray) { ... }
 * ```
 */

#ifndef ALLOC_AA_ALLOCAA_H_
#define ALLOC_AA_ALLOCAA_H_

#include "Alias/Spec/AliasSpecManager.h"
#include "Utils/LLVM/SystemHeaders.h"

/**
 * @class AllocAA
 * @brief Allocation-site-aware alias analysis utility.
 *
 * Constructed once per module, AllocAA pre-computes several sets during
 * construction (allocator call sites, primitive-array values, memoryless
 * functions) and then answers alias and array-access queries in roughly
 * O(1) per query.
 *
 * ### Thread Safety
 * AllocAA is **not** thread-safe.  All queries must be issued from a single
 * thread, or the caller must provide external synchronisation.
 */
class AllocAA {
public:
  /**
   * @brief Construct AllocAA and run all pre-computation passes.
   *
   * The constructor performs the following steps in order:
   *  1. Queries `AliasSpecManager` to seed `allocatorFunctionNames` and
   *     `memorylessFunctionNames` from the shared library specification.
   *  2. Calls `collectCGUnderFunctionMain` to build `CGUnderMain`.
   *  3. Calls `collectAllocations` to find all call sites of known allocators.
   *  4. Calls `collectPrimitiveArrayValues` to identify primitive arrays.
   *  5. Calls `collectMemorylessFunctions` to extend `memorylessFunctionNames`
   *     with functions that contain no loads, stores, calls, or global refs.
   *
   * @param M          The LLVM module to analyse.  Must outlive this object.
   * @param getSCEV    Lazy accessor for `ScalarEvolution`; called only from
   *                   `areGEPIndicesConstantOrIV`.
   * @param getLoopInfo Lazy accessor for `LoopInfo`; called only from
   *                   `areIdenticalGEPAccessesInSameLoop`.
   * @param getCallGraph Lazy accessor for the module `CallGraph`; called
   *                   during construction and stored for later use.
   *
   * @pre  The module must contain a function named `"main"`.
   */
  AllocAA(Module &M,
          std::function<llvm::ScalarEvolution &(Function &F)> getSCEV,
          std::function<llvm::LoopInfo &(Function &F)> getLoopInfo,
          std::function<llvm::CallGraph &(void)> getCallGraph);

  // =========================================================================
  // Public Query API
  // =========================================================================

  /**
   * @brief Identify the primitive array (and optional GEP) accessed by a
   *        memory instruction.
   *
   * Given a `LoadInst` or `StoreInst` @p V, this method walks the pointer
   * operand chain to determine whether the access targets a known primitive
   * array.  Three patterns are recognised:
   *
   *  - **Direct access**: `load/store ptr` where `ptr` is itself a primitive
   *    array value.  Returns `{array, nullptr}`.
   *  - **GEP of local array**: `load/store (gep localArray, ...)`.
   *    Returns `{localArray, gep}`.
   *  - **GEP of loaded global**: `load/store (gep (load globalArrayPtr), ...)`.
   *    Returns `{globalArrayPtr, gep}`.
   *
   * @param V  A `LoadInst` or `StoreInst`.
   * @return   A pair `{baseArray, gep}`.  Both are `nullptr` if the access
   *           does not target a known primitive array.
   */
  std::pair<Value *, GetElementPtrInst *> getPrimitiveArrayAccess(Value *V);

  /**
   * @brief Check whether all non-constant GEP indices are induction variables.
   *
   * Returns `true` if every index of @p gep is either:
   *   - A `ConstantInt`, or
   *   - An SCEV expression of type `scAddRecExpr` (i.e., a polynomial
   *     add-recurrence, which is the canonical SCEV form for loop induction
   *     variables).
   *
   * This is used to determine whether a GEP access pattern is "regular"
   * enough to reason about without full dependence analysis.
   *
   * @param gep  The GEP instruction to inspect.
   * @return     `true` if all indices are constants or IVs; `false` otherwise.
   *
   * @note Uses `ScalarEvolution` for the function containing @p gep.
   */
  bool areGEPIndicesConstantOrIV(GetElementPtrInst *gep);

  /**
   * @brief Check whether two GEP instructions perform identical accesses
   *        within the same loop.
   *
   * Two GEPs are considered "identical in the same loop" if:
   *  1. They belong to the same function.
   *  2. They are in basic blocks that belong to the same innermost loop
   *     (as reported by `LoopInfo`).
   *  3. Their pointer operands are the same value, or both are loads from
   *     the same pointer.
   *  4. All corresponding index operands are the same `Value *`.
   *
   * This is a syntactic / structural check — it does not perform value
   * numbering or alias analysis on the indices.
   *
   * @param gep1  First GEP instruction.
   * @param gep2  Second GEP instruction.
   * @return      `true` if the two GEPs are structurally identical and reside
   *              in the same loop; `false` otherwise.
   *
   * @note Uses `LoopInfo` for the function containing the GEPs.
   */
  bool areIdenticalGEPAccessesInSameLoop(GetElementPtrInst *gep1,
                                         GetElementPtrInst *gep2);

  /**
   * @brief Determine whether two pointer values can point to the same object.
   *
   * This is the primary alias query.  It returns `false` (no-alias) only
   * when at least one of the following sub-checks fires:
   *
   *  - **Globals check** (`canPointToTheSameObject_Globals`): distinct
   *    `GlobalValue`s, a `GlobalValue` vs. an `AllocaInst`, or two distinct
   *    `AllocaInst`s cannot alias.
   *  - **Argument-attributes check**
   *    (`canPointToTheSameObject_ArgumentAttributes`): a load through a
   *    `readonly`-attributed argument cannot alias a store.
   *
   * If neither check fires, the method conservatively returns `true`
   * (may-alias).
   *
   * @param p1  First pointer value (any LLVM `Value *`).
   * @param p2  Second pointer value (any LLVM `Value *`).
   * @return    `false` if the analysis can prove no-alias; `true` otherwise.
   *
   * @note This is an **under-approximation** of aliasing: a `false` return
   *       is a sound no-alias proof, but a `true` return does not mean the
   *       pointers definitely alias.
   */
  bool canPointToTheSameObject(Value *p1, Value *p2);

  /**
   * @brief Query whether a function is known to be read-only.
   *
   * A function is "read-only" if it is listed in `readOnlyFunctionNames`,
   * which is populated from the `AliasSpecManager` library specification.
   * Read-only functions may read memory but do not write through pointer
   * arguments.
   *
   * @param functionName  The function name to look up.
   * @return              `true` if the function is known read-only.
   */
  bool isReadOnly(StringRef functionName);

  /**
   * @brief Query whether a function is known to be "memoryless".
   *
   * A function is "memoryless" if it neither reads nor writes any memory
   * (no loads, stores, calls, or global-value references).  Such functions
   * are pure integer computations.
   *
   * The set is seeded from `AliasSpecManager` (functions with empty mod/ref
   * sets) and extended by `collectMemorylessFunctions`, which scans all
   * functions reachable from `main`.
   *
   * @param functionName  The function name to look up.
   * @return              `true` if the function is known memoryless.
   */
  bool isMemoryless(StringRef functionName);

private:
  // =========================================================================
  // Analysis inputs (provided by the caller)
  // =========================================================================

  /// The LLVM module being analysed.
  Module &M;

  /// Lazy accessor for `ScalarEvolution`; called per-function on demand.
  std::function<llvm::ScalarEvolution &(Function &F)> getSCEV;

  /// Lazy accessor for `LoopInfo`; called per-function on demand.
  std::function<llvm::LoopInfo &(Function &F)> getLoopInfo;

  /// Lazy accessor for the module-level `CallGraph`.
  std::function<llvm::CallGraph &(void)> getCallGraph;

  // =========================================================================
  // Pre-computed analysis results (built during construction)
  // =========================================================================

  /**
   * @brief All functions reachable from `main` via the call graph.
   *
   * Computed by `collectCGUnderFunctionMain` using a BFS from `main`.
   * All subsequent collection passes are restricted to this set so that
   * dead code is ignored.
   */
  std::set<Function *> CGUnderMain;

  /**
   * @brief All `CallInst`s that invoke a known memory-allocator function
   *        (e.g., `malloc`, `calloc`, `realloc`) within `CGUnderMain`.
   *
   * Populated by `collectAllocations`.  Used by `collectPrimitiveArrayValues`
   * to seed the candidate set of primitive-array locals.
   *
   * TODO: These should become objects representing the full usage of these
   * allocated arrays.
   */
  std::set<CallInst *> allocatorCalls;

  /**
   * @brief Names of functions that allocate contiguous memory
   *        (e.g., `"malloc"`, `"calloc"`).
   *
   * Seeded from `AliasSpecManager::getAllocatorNames()` during construction.
   */
  std::set<std::string> allocatorFunctionNames;

  /**
   * @brief Names of functions that only read memory (no writes through
   *        pointer arguments).
   *
   * Used by `isPrimitiveArray` to allow calls to read-only functions without
   * disqualifying the array from being "primitive".
   */
  std::set<std::string> readOnlyFunctionNames;

  /**
   * @brief Names of functions that perform no memory operations at all
   *        (no loads, stores, calls, or global references).
   *
   * Seeded from `AliasSpecManager` and extended by
   * `collectMemorylessFunctions`.
   */
  std::set<std::string> memorylessFunctionNames;

  /**
   * @brief Global variables that are primitive arrays.
   *
   * A global is a primitive array if every use of it is either:
   *   - A store of a freshly-allocated (single-use) allocator call result, or
   *   - A load whose result is itself used only as a primitive array.
   *
   * Populated by `collectPrimitiveArrayValues`.
   */
  std::set<GlobalValue *> primitiveArrayGlobals;

  /**
   * @brief Local (heap-allocated) values that are primitive arrays.
   *
   * These are `CallInst`s to allocator functions whose return value is used
   * only via GEPs and read-only calls — i.e., the pointer never escapes.
   *
   * Populated by `collectPrimitiveArrayValues`.
   */
  std::set<Instruction *> primitiveArrayLocals;

  /**
   * @brief Shared library-function specification manager.
   *
   * Provides categorised information about well-known library functions
   * (allocators, deallocators, read-only functions, etc.) loaded from the
   * Lotus spec files.
   */
  lotus::alias::AliasSpecManager specManager;

  // =========================================================================
  // Collection helpers (run during construction)
  // =========================================================================

  /**
   * @brief Populate `CGUnderMain` with all functions reachable from `main`.
   *
   * Performs a BFS over the call graph starting from the function named
   * `"main"`.  Only functions with non-empty bodies are added (declarations
   * are skipped).
   *
   * @pre  @p M must contain a function named `"main"`.
   * @post `CGUnderMain` contains `main` and all transitively called functions.
   *
   * TODO: Find a way to extract this into a helper module for all passes in
   * the PDG project.
   */
  void collectCGUnderFunctionMain(Module &M, CallGraph &callGraph);

  /**
   * @brief Populate `allocatorCalls` with all call sites of known allocators.
   *
   * Iterates over all functions in the module to find declarations that are
   * recognised as allocators by `specManager`, then calls
   * `collectFunctionCallsTo` to find all call sites within `CGUnderMain`.
   */
  void collectAllocations(Module &M, CallGraph &callGraph);

  /**
   * @brief Collect all `CallInst`s within `CGUnderMain` that call one of the
   *        functions in @p called.
   *
   * Iterates over the call-graph edges of every function in `CGUnderMain`
   * and records matching `CallInst`s into @p calls.
   *
   * @param callGraph  The module call graph.
   * @param called     Set of callee functions to look for.
   * @param calls      Output set; matching `CallInst`s are inserted here.
   */
  void collectFunctionCallsTo(CallGraph &callGraph,
                              std::set<Function *> &called,
                              std::set<CallInst *> &calls);

  /**
   * @brief Extend `memorylessFunctionNames` by scanning `CGUnderMain`.
   *
   * A function is added to `memorylessFunctionNames` if it contains no
   * `LoadInst`, `StoreInst`, or `CallInst`, and none of its instructions
   * reference a `GlobalValue` operand.
   *
   * This intra-procedural scan complements the spec-based seed: it catches
   * user-defined helper functions that happen to be pure integer computations.
   *
   * TODO(angelo): Trigger a re-check of callers when a callee is found to be
   * memoryless, so that callers may also qualify.
   */
  void collectMemorylessFunctions(Module &M);

  // =========================================================================
  // Primitive-array identification helpers
  // =========================================================================

  /**
   * @brief Collect all user instructions of @p V into @p userInstructions.
   *
   * Handles three cases:
   *  - Direct `Instruction` users are added as-is.
   *  - `BitCastOperator` or `ZExtOperator` users with a single use are
   *    "looked through" and their single `Instruction` user is added.
   *  - Any other non-instruction user causes the method to return `false`,
   *    indicating that the value has an unrecognised use.
   *
   * @param V                 The value whose users to collect.
   * @param userInstructions  Output set of user instructions.
   * @return                  `true` if all users were successfully collected;
   *                          `false` if an unrecognised (non-instruction) user
   *                          was encountered.
   */
  bool collectUserInstructions(Value *V,
                               std::set<Instruction *> &userInstructions);

  /**
   * @brief Populate `primitiveArrayGlobals` and `primitiveArrayLocals`.
   *
   * For globals: iterates over all global variables in the module, restricts
   * to those used within `CGUnderMain`, and calls `isPrimitiveArrayPointer`.
   *
   * For locals: iterates over `allocatorCalls` and calls `isPrimitiveArray`
   * on each call result.
   */
  void collectPrimitiveArrayValues(Module &M);

  /**
   * @brief Determine whether @p V (a pointer to an array) is a primitive
   *        array pointer.
   *
   * A value is a primitive array *pointer* (i.e., a pointer-to-array, such
   * as a global `i32**`) if every use is either:
   *  - A store of a freshly-allocated (single-use) allocator call result, or
   *  - A load whose result satisfies `isPrimitiveArray`.
   *
   * This is used for global variables that hold pointers to heap-allocated
   * arrays.
   *
   * @param V                 The pointer value to test.
   * @param userInstructions  Pre-collected set of user instructions of @p V.
   * @return                  `true` if @p V is a primitive array pointer.
   */
  bool isPrimitiveArrayPointer(Value *V,
                               std::set<Instruction *> &userInstructions);

  /**
   * @brief Determine whether @p V (an array value) is a primitive array.
   *
   * A value is a primitive array if every use is one of:
   *  - A `CastInst` whose result is also a primitive array (recursive).
   *  - A `GetElementPtrInst` whose result does not escape
   *    (`doesValueNotEscape`).
   *  - A `CallInst` to a known read-only function.
   *
   * Any other use disqualifies the value.
   *
   * @param V                 The value to test (typically a `CallInst` result
   *                          or a cast thereof).
   * @param userInstructions  Pre-collected set of user instructions of @p V.
   * @return                  `true` if @p V is a primitive array.
   */
  bool isPrimitiveArray(Value *V, std::set<Instruction *> &userInstructions);

  /**
   * @brief Recursively determine whether the value produced by @p I does not
   *        escape to an unknown context.
   *
   * A value "does not escape" if every use along every def-use chain is one
   * of the following:
   *  - A branch or switch instruction (control-flow, not a data use).
   *  - A return of an integer-typed value (the integer cannot be
   *    reinterpreted as a pointer by any downstream use of the original
   *    array value).
   *  - A store where both the stored value and the storage location are
   *    themselves non-escaping.
   *  - An integer-typed instruction whose result is non-escaping.
   *
   * The @p checked set prevents infinite recursion on cycles in the use graph.
   *
   * @param checked  Set of already-visited instructions (cycle guard).
   * @param I        The instruction whose result is being tested.
   * @return         `true` if the value does not escape; `false` otherwise.
   */
  bool doesValueNotEscape(std::set<Instruction *> checked, Instruction *I);

  // =========================================================================
  // Primitive-array lookup helpers
  // =========================================================================

  /**
   * @brief Return the primitive array (local or global) that @p V refers to,
   *        or `nullptr`.
   *
   * Tries `getLocalPrimitiveArray` first; falls back to
   * `getGlobalValuePrimitiveArray`.
   */
  Value *getPrimitiveArray(Value *V);

  /**
   * @brief Return the local (heap-allocated) primitive array that @p V refers
   *        to, or `nullptr`.
   *
   * Strips a `CastInst` wrapper if present, then checks whether the
   * underlying `Instruction` is in `primitiveArrayLocals`.
   */
  Value *getLocalPrimitiveArray(Value *V);

  /**
   * @brief Return the global primitive array that @p V refers to, or
   *        `nullptr`.
   *
   * Strips a `CastInst` wrapper if present, then checks whether the
   * underlying `GlobalValue` is in `primitiveArrayGlobals`.
   */
  Value *getGlobalValuePrimitiveArray(Value *V);

  /**
   * @brief Return the pointer operand of a `LoadInst` or `StoreInst`, or
   *        `nullptr` for any other value.
   *
   * This is a convenience wrapper used to uniformly extract the memory
   * address from either kind of memory instruction.
   */
  Value *getMemoryPointerOperand(Value *V);

  // =========================================================================
  // canPointToTheSameObject sub-checks
  // =========================================================================

  /**
   * @brief Strip a GEP to its base pointer, or return @p p unchanged.
   *
   * Used by `canPointToTheSameObject_Globals` to compare the allocation
   * origins of two pointer values.
   *
   * @param p  A non-null pointer value.
   * @return   The pointer operand of @p p if it is a `GetElementPtrInst`;
   *           otherwise @p p itself.
   */
  Value *getBasePointer(Value *p);

  /**
   * @brief Sub-check: can @p p1 and @p p2 alias based on argument attributes?
   *
   * Returns `false` (no-alias) if:
   *  - One of @p p1 / @p p2 is a `LoadInst` and the other is a `StoreInst`,
   *    AND the load's pointer operand (after stripping a GEP) is an `Argument`
   *    marked `readonly`.
   *
   * A `readonly` argument guarantees the callee does not write through it,
   * so a load through it cannot alias a store.
   *
   * Returns `true` (may-alias) in all other cases.
   *
   * @param p1  First pointer value.
   * @param p2  Second pointer value.
   * @return    `false` if no-alias is proven; `true` otherwise.
   */
  bool canPointToTheSameObject_ArgumentAttributes(Value *p1, Value *p2);

  /**
   * @brief Sub-check: can @p p1 and @p p2 alias based on their allocation
   *        origins?
   *
   * Strips GEPs to base pointers and applies the following rules:
   *  - `GlobalValue` vs. `AllocaInst`  → no-alias (different storage classes).
   *  - Two distinct `GlobalValue`s     → no-alias (different global objects).
   *  - Two distinct `AllocaInst`s      → no-alias (different stack slots).
   *
   * Returns `true` (may-alias) in all other cases.
   *
   * @param p1  First pointer value.
   * @param p2  Second pointer value.
   * @return    `false` if no-alias is proven; `true` otherwise.
   */
  bool canPointToTheSameObject_Globals(Value *p1, Value *p2);
};

#endif // ALLOC_AA_ALLOCAA_H_
