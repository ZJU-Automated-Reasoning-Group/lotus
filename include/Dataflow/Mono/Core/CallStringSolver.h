/**
 * @file CallStringInterProceduralDataFlow.h
 * @brief A lightweight call-string based inter-procedural monotone data-flow
 * engine
 *
 * This file provides a reusable, context-sensitive interprocedural dataflow
 * analysis engine based on the call-string approach. It's designed to be
 * framework-agnostic and can be used independently of the InterMonoSolver.
 *
 * ## Overview
 *
 * The implementation maintains separate IN/OUT lattices per (Instruction,
 * CallString) pair, where the call string is bounded to length K. When K is
 * exceeded, the oldest call site is dropped (FIFO truncation), falling back to
 * less precise context.
 *
 * ### Key Design Principles
 *
 * 1. **Separation of Concerns**: The engine handles CFG traversal, context
 * management, and fixpoint iteration. The client provides analysis-specific
 * transfer functions.
 *
 * 2. **Flexibility**: Supports arbitrary container types (std::set,
 * BitVectorSet, etc.) and custom lattice operations via callbacks.
 *
 * 3. **Efficiency**: Uses worklist-based fixpoint iteration with lazy context
 *    initialization and precise successor/predecessor computation.
 *
 * 4. **Compatibility**: API mirrors intraprocedural solvers but extends
 * transfer functions to receive predecessor context for precise call/return
 * handling.
 *
 * ## Call-String Context Sensitivity
 *
 * **What is a call string?**
 * A call string is a sequence of call sites that led to the current program
 * point:
 *   - Empty context: [] (the root call string)
 *   - 1-call-string: [call_1]
 *   - 2-call-string: [call_1, call_2]
 *   - K-call-string: [call_1, ..., call_K]
 *
 * For K>0, [] is an ordinary root context, not a collapsed caller summary.
 * Only K=0 is context-insensitive.
 *
 * **Example:**
 * ```
 * void foo() { bar(); }       // call_1
 * void bar() { baz(); }       // call_2
 * void baz() { x = 1; }       // x defined here
 *
 * Contexts at x=1:
 * - K=0: [] (context-insensitive: x defined somewhere)
 * - K=1: [call_2] (x defined when called from bar)
 * - K=2: [call_1, call_2] (x defined when foo→bar→baz)
 * ```
 *
 * **Precision vs Performance:**
 * - K=0: Fast, least precise (context-insensitive)
 * - K=1: Moderate, distinguishes direct callers
 * - K=2: Slower, distinguishes call paths of length 2
 * - K=3+: Slowest, high precision (diminishing returns)
 *
 * **Typical values:** K ∈ {0, 1, 2, 3}. Most analyses use K=1 or K=2.
 *
 * ## Architecture
 *
 * ### Components
 *
 * 1. **CallStringInterProceduralDataFlowEngine**: Main fixpoint engine
 *    - Manages worklist, context propagation, and fixpoint iteration
 *    - Delegates analysis-specific logic to callbacks
 *
 * 2. **ContextSensitiveDataFlowResult**: Result container
 *    - Stores GEN/KILL (context-insensitive, per-instruction)
 *    - Stores IN/OUT (context-sensitive, per (Instruction, Context) pair)
 *    - Provides efficient lookup and iteration
 *
 * 3. **CallStringCTX**: Context representation
 *    - Bounded deque of call sites with K-limit
 *    - Automatic FIFO truncation on overflow
 *    - Hash/equality support for use in maps
 *
 * ### Data Flow Equations (Forward Analysis)
 *
 * For each (Instruction, Context) pair:
 *
 * ```
 * IN[n, ctx]  = merge { OUT[p, ctx_p] | (p, ctx_p) is a predecessor of (n, ctx)
 * } OUT[n, ctx] = transfer(IN[n, ctx], GEN[n], KILL[n])
 * ```
 *
 * **Predecessor computation** handles three edge types:
 * 1. **Normal edges**: p → n within the same function, same context
 * 2. **Call edges**: call_site → callee_entry, context = caller_ctx +
 * [call_site]
 * 3. **Return edges**: return → ret_site, context = callee_ctx - [last_call]
 *
 * ### Worklist Algorithm
 *
 * ```
 * 1. Initialize worklist with seed (Instruction, Context) pairs
 * 2. While worklist not empty:
 *    a. Dequeue (n, ctx)
 *    b. Compute IN[n, ctx] from predecessors
 *    c. Compute OUT[n, ctx] from IN[n, ctx]
 *    d. If IN or OUT changed:
 *       - Enqueue all successors of (n, ctx)
 * 3. Return fixed-point results
 * ```
 *
 * ## Usage Example
 *
 * ### Simple Forward Taint Analysis (K=1)
 *
 * ```cpp
 * using Engine = CallStringInterProceduralDataFlowEngine<1, std::set<Value*>>;
 * using Result = Engine::ResultTy;
 *
 * Engine engine;
 *
 * // Define transfer functions
 * auto computeGEN = [](Instruction *I, Result *DF) {
 *   if (isTaintSource(I)) {
 *     DF->GEN(I).insert(I);
 *   }
 * };
 *
 * auto computeIN = [](Instruction *I, Instruction *Pred,
 *                     const Context &PredCtx,
 *                     std::set<Value*> &IN, Result *DF) {
 *   // Union: taint flows if present on any path
 *   auto &PredOut = DF->OUT(Pred, PredCtx);
 *   IN.insert(PredOut.begin(), PredOut.end());
 * };
 *
 * auto computeOUT = [](Instruction *I, const Context &Ctx,
 *                      std::set<Value*> &OUT, Result *DF) {
 *   OUT = DF->IN(I, Ctx);
 *   // Propagate taint through uses
 *   for (auto *Operand : I->operands()) {
 *     if (OUT.count(Operand)) {
 *       OUT.insert(I);
 *     }
 *   }
 * };
 *
 * auto equal = [](const auto &L, const auto &R) { return L == R; };
 *
 * auto results = engine.applyForward(
 *   EntryFunc, ICFG, computeGEN, initIN, initOUT,
 *   computeIN, computeOUT, equal
 * );
 *
 * // Query results
 * for (auto &[Key, Facts] : results->getINMap()) {
 *   llvm::errs() << "At " << *Key.Inst << " with context ";
 *   Key.Ctx.print(llvm::errs()) << ": ";
 *   printFacts(Facts);
 * }
 * ```
 *
 * ## Callback Reference
 *
 * The engine requires several callbacks to define analysis behavior:
 *
 * ### computeGEN(Instruction *I, Result *DF)
 * Computes GEN[I] - facts generated by instruction I (context-insensitive).
 * Called once per instruction before fixpoint iteration.
 *
 * ### computeKILL(Instruction *I, Result *DF)
 * Computes KILL[I] - facts killed by instruction I (context-insensitive).
 * Called once per instruction before fixpoint iteration.
 *
 * ### initializeIN(Instruction *I, Container &IN)
 * Initializes IN[I, ctx] when a new (I, ctx) pair is first seen.
 * Typically sets IN to bottom (empty set) or top (universal set).
 *
 * ### initializeOUT(Instruction *I, Container &OUT)
 * Initializes OUT[I, ctx] when a new (I, ctx) pair is first seen.
 *
 * ### computeIN(Instruction *I, Instruction *Pred, Context &PredCtx, Container
 * &IN, Result *DF) Merges predecessor OUT into current IN. Called for each
 * predecessor edge.
 * **Key insight:** Receives predecessor context to handle call/return
 * precisely.
 *
 * Example (union-based):
 * ```cpp
 * auto &PredOut = DF->OUT(Pred, PredCtx);
 * IN.insert(PredOut.begin(), PredOut.end());
 * ```
 *
 * ### computeOUT(Instruction *I, Context &Ctx, Container &OUT, Result *DF)
 * Computes OUT from IN, GEN, and KILL for instruction I in context Ctx.
 *
 * Example (standard monotone):
 * ```cpp
 * OUT = DF->IN(I, Ctx);
 * OUT.insert(DF->GEN(I).begin(), DF->GEN(I).end());
 * for (auto *K : DF->KILL(I)) {
 *   OUT.erase(K);
 * }
 * ```
 *
 * ### equal(Container &L, Container &R)
 * Tests if two fact sets are equal (for fixpoint detection).
 *
 * ## Advanced Features
 *
 * ### Seed-Based Initialization
 *
 * Use `applyForwardFromSeeds()` to start from specific (Instruction, Context)
 * pairs with pre-initialized facts. This supports:
 * - Multiple entry points
 * - Mid-function analysis seeds
 * - Incremental analysis (start from previous results)
 *
 * Example:
 * ```cpp
 * std::vector<ContextKey> seeds = {
 *   {FirstInst, Context()},
 *   {TaintSource, Context()}
 * };
 * std::map<ContextKey, Container> seedFacts;
 * seedFacts[{TaintSource, Context()}].insert(taintedValue);
 *
 * auto results = engine.applyForwardFromSeeds(
 *   Module, seeds, ICFG, seedFacts, ...
 * );
 * ```
 *
 * ### Custom ICFG
 *
 * Provide your own ICFG implementation for:
 * - Indirect call resolution
 * - Virtual dispatch handling
 * - Concurrency modeling
 * - Exception handling
 *
 * The engine uses ICFG::getSuccsOf(), getPredsOf(), getCalleesOfCallAt(),
 * getReturnSitesOfCallAt(), and related queries.
 *
 * ### Bit-Vector Optimization
 *
 * For large universes, use BitVectorSet as the container:
 * ```cpp
 * using Engine = CallStringInterProceduralDataFlowEngine<2,
 * BitVectorSet<Value*>>;
 * ```
 * This reduces memory by ~30x and speeds up set operations by ~5-10x.
 *
 * ## Performance Characteristics
 *
 * ### Time Complexity
 * - Worst case: O(K * N * E * |L|)
 *   - K: call-string depth
 *   - N: number of instructions
 *   - E: number of CFG edges
 *   - |L|: lattice height (iterations to fixpoint)
 *
 * ### Space Complexity
 * - O(K^d * N * |Facts|)
 *   - d: maximum call depth
 *   - |Facts|: average fact set size
 *
 * ## References
 *
 * - Sharir & Pnueli (1981): "Two Approaches to Interprocedural Data Flow
 * Analysis"
 * - Reps, Horwitz, Sagiv (1995): "Precise Interprocedural Dataflow Analysis via
 * Graph Reachability"
 * - Phasar framework: https://github.com/secure-software-engineering/phasar
 *
 * @see InterMonoSolver for the problem-based wrapper around this engine
 * @see CallStringCTX for context representation details
 * @see ContextSensitiveDataFlowResult for result container details
 */

#ifndef LOTUS_DATAFLOW_MONO_CORE_CALLSTRINGSOLVER_H_
#define LOTUS_DATAFLOW_MONO_CORE_CALLSTRINGSOLVER_H_

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "Dataflow/ControlFlow/FlowDirection.h"
#include "Dataflow/ControlFlow/InterCFG.h"
#include "Dataflow/Mono/Core/CallStringContext.h"

#include <deque>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <unordered_set>
#include <vector>

namespace dataflow {

/**
 * @brief Context-sensitive dataflow analysis result container
 *
 * This class stores the results of a context-sensitive dataflow analysis,
 * maintaining separate lattice values for each (Instruction, Context) pair.
 *
 * **Storage strategy:**
 * - GEN/KILL: Per-instruction, context-insensitive (shared across all contexts)
 * - IN/OUT: Per (Instruction, Context) pair, context-sensitive
 *
 * This matches the standard monotone framework where GEN/KILL are local
 * properties of an instruction, while IN/OUT depend on the calling context.
 *
 * **Memory layout example** (K=1, 3 instructions, 2 call sites):
 * ```
 * GEN:  { inst1 → {v1}, inst2 → {v2}, inst3 → {} }
 * KILL: { inst1 → {},   inst2 → {v1}, inst3 → {} }
 *
 * IN:   { (inst1, [])      → {v1, v2},
 *         (inst1, [call1]) → {v1},
 *         (inst2, [])      → {v2},
 *         (inst2, [call2]) → {} }
 *
 * OUT:  { (inst1, [])      → {v1, v2, v3},
 *         (inst1, [call1]) → {v1, v3},
 *         ... }
 * ```
 *
 * **Usage:**
 * ```cpp
 * Result->GEN(inst).insert(value);           // Add to GEN set
 * auto &in = Result->IN(inst, context);      // Get IN facts
 * Result->OUT(key) = newFacts;               // Update OUT facts
 *
 * // Iterate all contexts
 * for (auto &[key, facts] : Result->getINMap()) {
 *   processContext(key.Inst, key.Ctx, facts);
 * }
 * ```
 *
 * @tparam K Maximum call-string length
 * @tparam ContainerT Fact container type (e.g., std::set<Value*>,
 * BitVectorSet<Value*>)
 */
template <unsigned K, typename ContainerT>
class ContextSensitiveDataFlowResult {
public:
  using Context = mono::CallStringCTX<llvm::Instruction *, K>;

  /**
   * @brief Key type for context-sensitive fact storage
   *
   * Combines an instruction pointer with its calling context to uniquely
   * identify a program point in the context-sensitive analysis.
   *
   * **Ordering:** First by instruction pointer, then by context. This
   * clustering improves cache locality when iterating results.
   */
  struct ContextKey {
    llvm::Instruction *Inst;
    Context Ctx;

    bool operator<(const ContextKey &Rhs) const {
      if (Inst != Rhs.Inst) {
        return Inst < Rhs.Inst;
      }
      return Ctx < Rhs.Ctx;
    }
  };

  ContextSensitiveDataFlowResult() = default;

  ContainerT &GEN(llvm::Instruction *Inst) { return Gens[Inst]; }
  ContainerT &KILL(llvm::Instruction *Inst) { return Kills[Inst]; }

  ContainerT &IN(const ContextKey &Key) { return Ins[Key]; }
  ContainerT &OUT(const ContextKey &Key) { return Outs[Key]; }
  ContainerT &IN(llvm::Instruction *Inst, const Context &Ctx) {
    return IN(ContextKey{Inst, Ctx});
  }
  ContainerT &OUT(llvm::Instruction *Inst, const Context &Ctx) {
    return OUT(ContextKey{Inst, Ctx});
  }

  // Replace shared static empty containers with per-instance members.
  // The old code used "static ContainerT EmptySet" which is shared across all
  // instances and all calls.  For BitVectorSet this means the returned empty
  // set has no universe, causing incorrect results when callers iterate or
  // union with it.  Using per-instance members avoids the sharing problem.
  const ContainerT &IN(const ContextKey &Key) const {
    auto It = Ins.find(Key);
    if (It == Ins.end()) {
      return EmptyContainer;
    }
    return It->second;
  }

  const ContainerT &OUT(const ContextKey &Key) const {
    auto It = Outs.find(Key);
    if (It == Outs.end()) {
      return EmptyContainer;
    }
    return It->second;
  }
  const ContainerT &IN(llvm::Instruction *Inst, const Context &Ctx) const {
    return IN(ContextKey{Inst, Ctx});
  }
  const ContainerT &OUT(llvm::Instruction *Inst, const Context &Ctx) const {
    return OUT(ContextKey{Inst, Ctx});
  }

  bool hasContext(const ContextKey &Key) const {
    return Ins.find(Key) != Ins.end() || Outs.find(Key) != Outs.end();
  }

  const std::map<ContextKey, ContainerT> &getINMap() const { return Ins; }

  const std::map<ContextKey, ContainerT> &getOUTMap() const { return Outs; }

  const std::map<llvm::Instruction *, ContainerT> &getGENMap() const {
    return Gens;
  }

  const std::map<llvm::Instruction *, ContainerT> &getKILLMap() const {
    return Kills;
  }

private:
  std::map<llvm::Instruction *, ContainerT> Gens;
  std::map<llvm::Instruction *, ContainerT> Kills;
  std::map<ContextKey, ContainerT> Ins;
  std::map<ContextKey, ContainerT> Outs;
  /// Per-instance empty container returned for missing keys.
  /// This avoids the shared-static problem where all callers would share
  /// one empty container (especially problematic for BitVectorSet which
  /// needs a universe to be useful).
  ContainerT EmptyContainer{};
};

/**
 * @brief Call-string interprocedural forward dataflow engine
 *
 * This is the main fixpoint iteration engine for context-sensitive
 * interprocedural monotone dataflow analysis.
 *
 * **Responsibilities:**
 * 1. Worklist management and fixpoint iteration
 * 2. Context propagation across call/return edges
 * 3. Lazy initialization of (Instruction, Context) pairs
 * 4. Predecessor/successor computation with context transitions
 *
 * **Thread safety:** Not thread-safe. Create one instance per analysis.
 *
 * @tparam K Maximum call-string length (typical: 0-3)
 * @tparam ContainerT Fact container type (must support insert, erase, ==)
 */
template <unsigned K, typename ContainerT>
class CallStringInterProceduralDataFlowEngine {
public:
  using ResultTy = ContextSensitiveDataFlowResult<K, ContainerT>;
  using Context = typename ResultTy::Context;
  using ContextKey = typename ResultTy::ContextKey;
  using ICFG = dataflow::controlflow::InterCFG;

  CallStringInterProceduralDataFlowEngine() = default;

  /**
   * @brief Run forward dataflow analysis from a single entry function
   *
   * This is the main entry point for analysis from a single entry function.
   * The analysis starts at the entry block's first instruction with an empty
   * call-string context.
   *
   *
   * @return unique_ptr to result container (never null on success)
   */
  std::unique_ptr<ResultTy> applyForward(
      llvm::Function *Entry, const ICFG *ICF,
      std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
      std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
      std::function<void(llvm::Instruction *Inst, ContainerT &IN)> initializeIN,
      std::function<void(llvm::Instruction *Inst, ContainerT &OUT)>
          initializeOUT,
      std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                         const Context &PredCtx, const Context &CurrentCtx,
                         ContainerT &IN, ResultTy *DF)>
          computeIN,
      std::function<void(llvm::Instruction *Inst, const Context &Ctx,
                         ContainerT &OUT, ResultTy *DF)>
          computeOUT,
      std::function<bool(const ContainerT &, const ContainerT &)> equal);

  /**
   * @brief Run forward dataflow analysis from explicit seed points
   * (Phasar-compatible)
   *
   * This is a more flexible variant that supports:
   * - Multiple entry points
   * - Mid-function analysis seeds
   * - Pre-initialized fact sets at specific (instruction, context) pairs
   *
   * This API is compatible with Phasar's InterMonoSolver initialization
   * strategy.
   *
   * **Use cases:**
   * - Analyzing multiple entry points simultaneously
   * - Incremental analysis (start from previous fixpoint)
   * - Taint analysis (inject taint at specific sources)
   * - Test generation (start from specific states)
   *
   * **Seed initialization:**
   * The `SeedIns` map allows pre-populating IN facts for specific (inst, ctx)
   * pairs. These facts are preserved during fixpoint iteration:
   * ```
   * IN[seed_inst, seed_ctx] = SeedIns[{seed_inst, seed_ctx}] ∪ (merge from
   * preds)
   * ```
   *
   * **Parameters:**
   * - @param M The module containing all code to analyze
   * - @param Seeds Initial (instruction, context) pairs to start analysis
   * - @param ICF Interprocedural control flow graph
   * - @param SeedIns Pre-initialized IN facts for seeds (may be empty)
   * - (other params same as applyForward)
   *
   * @return Ownership of result container
   *
   * **Example - Multi-entry taint analysis:**
   * ```cpp
   * std::vector<ContextKey> seeds;
   * std::map<ContextKey, std::set<Value*>> seedFacts;
   *
   * // Seed 1: main() entry
   * seeds.push_back({&*main->begin()->begin(), Context()});
   *
   * // Seed 2: Taint source with initial taint
   * auto *source = findTaintSource();
   * seeds.push_back({source, Context()});
   * seedFacts[{source, Context()}].insert(source);
   *
   * auto results = engine.applyForwardFromSeeds(module, seeds, icfg,
   * seedFacts, ...);
   * ```
   */
  std::unique_ptr<ResultTy> applyForwardFromSeeds(
      llvm::Module *M, const std::vector<ContextKey> &Seeds, const ICFG *ICF,
      const std::map<ContextKey, ContainerT> &SeedIns,
      std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
      std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
      std::function<void(llvm::Instruction *Inst, ContainerT &IN)> initializeIN,
      std::function<void(llvm::Instruction *Inst, ContainerT &OUT)>
          initializeOUT,
      std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                         const Context &PredCtx, const Context &CurrentCtx,
                         ContainerT &IN, ResultTy *DF)>
          computeIN,
      std::function<void(llvm::Instruction *Inst, const Context &Ctx,
                         ContainerT &OUT, ResultTy *DF)>
          computeOUT,
      std::function<bool(const ContainerT &, const ContainerT &)> equal);

  std::unique_ptr<ResultTy> applyBackward(
      llvm::Function *Entry, const ICFG *ICF,
      std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
      std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
      std::function<void(llvm::Instruction *Inst, ContainerT &IN)> initializeIN,
      std::function<void(llvm::Instruction *Inst, ContainerT &OUT)>
          initializeOUT,
      std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                         const Context &PredCtx, const Context &CurrentCtx,
                         ContainerT &IN, ResultTy *DF)>
          computeIN,
      std::function<void(llvm::Instruction *Inst, const Context &Ctx,
                         ContainerT &OUT, ResultTy *DF)>
          computeOUT,
      std::function<bool(const ContainerT &, const ContainerT &)> equal);

  std::unique_ptr<ResultTy> applyBackwardFromSeeds(
      llvm::Module *M, const std::vector<ContextKey> &Seeds, const ICFG *ICF,
      const std::map<ContextKey, ContainerT> &SeedIns,
      std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
      std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
      std::function<void(llvm::Instruction *Inst, ContainerT &IN)> initializeIN,
      std::function<void(llvm::Instruction *Inst, ContainerT &OUT)>
          initializeOUT,
      std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                         const Context &PredCtx, const Context &CurrentCtx,
                         ContainerT &IN, ResultTy *DF)>
          computeIN,
      std::function<void(llvm::Instruction *Inst, const Context &Ctx,
                         ContainerT &OUT, ResultTy *DF)>
          computeOUT,
      std::function<bool(const ContainerT &, const ContainerT &)> equal);

  /**
   * Convenience overload with empty KILL sets.
   * Fix: returns unique_ptr<ResultTy> to match the primary overload.
   */
  std::unique_ptr<ResultTy> applyForward(
      llvm::Function *Entry, const ICFG *ICF,
      std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
      std::function<void(llvm::Instruction *Inst, ContainerT &IN)> initializeIN,
      std::function<void(llvm::Instruction *Inst, ContainerT &OUT)>
          initializeOUT,
      std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                         const Context &PredCtx, const Context &CurrentCtx,
                         ContainerT &IN, ResultTy *DF)>
          computeIN,
      std::function<void(llvm::Instruction *Inst, const Context &Ctx,
                         ContainerT &OUT, ResultTy *DF)>
          computeOUT,
      std::function<bool(const ContainerT &, const ContainerT &)> equal) {
    auto EmptyKill = [](llvm::Instruction *, ResultTy *) {};
    return applyForward(Entry, ICF, computeGEN, std::move(EmptyKill),
                        std::move(initializeIN), std::move(initializeOUT),
                        std::move(computeIN), std::move(computeOUT),
                        std::move(equal));
  }

private:
  using WorkQueue = std::deque<ContextKey>;

  static bool isCallToDefinedFunction(llvm::Instruction *Inst, const ICFG *ICF);

  static llvm::Instruction *getFirstInstruction(llvm::BasicBlock *BB) {
    return &*BB->begin();
  }

  static bool isFunctionEntry(llvm::Instruction *Inst) {
    auto *BB = Inst->getParent();
    return &BB->getParent()->getEntryBlock() == BB && Inst == &*BB->begin();
  }

  void computeGenKill(
      llvm::Module *M,
      std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
      std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
      ResultTy *DF);

  void ensureInitialized(
      const ContextKey &Key,
      std::function<void(llvm::Instruction *, ContainerT &)> initializeIN,
      std::function<void(llvm::Instruction *, ContainerT &)> initializeOUT,
      ResultTy *DF);

  std::vector<ContextKey> predecessors(
      const ContextKey &Key,
      const std::map<llvm::Instruction *, std::vector<llvm::Instruction *>>
          &CallToReturns,
      const std::map<llvm::Instruction *, std::vector<llvm::Instruction *>>
          &ContinuationToCalls,
      const ICFG *ICF, dataflow::controlflow::FlowDirection Dir);

  std::vector<ContextKey> successors(const ContextKey &Key, const ICFG *ICF,
                                     dataflow::controlflow::FlowDirection Dir);

  std::unique_ptr<ResultTy> applyFromSeeds(
      llvm::Module *M, const std::vector<ContextKey> &Seeds, const ICFG *ICF,
      const std::map<ContextKey, ContainerT> &SeedIns,
      dataflow::controlflow::FlowDirection Dir,
      std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
      std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
      std::function<void(llvm::Instruction *Inst, ContainerT &IN)> initializeIN,
      std::function<void(llvm::Instruction *Inst, ContainerT &OUT)>
          initializeOUT,
      std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                         const Context &PredCtx, const Context &CurrentCtx,
                         ContainerT &IN, ResultTy *DF)>
          computeIN,
      std::function<void(llvm::Instruction *Inst, const Context &Ctx,
                         ContainerT &OUT, ResultTy *DF)>
          computeOUT,
      std::function<bool(const ContainerT &, const ContainerT &)> equal);
};

// ---- Header-only template implementation ----

template <unsigned K, typename ContainerT>
bool CallStringInterProceduralDataFlowEngine<
    K, ContainerT>::isCallToDefinedFunction(llvm::Instruction *Inst,
                                            const ICFG *ICF) {
  if (ICF == nullptr || Inst == nullptr || !ICF->isCallSite(Inst)) {
    return false;
  }
  for (auto *Callee : ICF->getCalleesOfCallAt(Inst)) {
    if (Callee != nullptr && !Callee->isDeclaration() && !Callee->empty()) {
      return true;
    }
  }
  return false;
}

template <unsigned K, typename ContainerT>
void CallStringInterProceduralDataFlowEngine<K, ContainerT>::computeGenKill(
    llvm::Module *M,
    std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
    std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
    ResultTy *DF) {
  for (auto &F : *M) {
    if (F.isDeclaration()) {
      continue;
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        computeGEN(&I, DF);
        computeKILL(&I, DF);
      }
    }
  }
}

template <unsigned K, typename ContainerT>
void CallStringInterProceduralDataFlowEngine<K, ContainerT>::ensureInitialized(
    const ContextKey &Key,
    std::function<void(llvm::Instruction *, ContainerT &)> initializeIN,
    std::function<void(llvm::Instruction *, ContainerT &)> initializeOUT,
    ResultTy *DF) {
  if (DF->hasContext(Key)) {
    return;
  }
  auto &INSet = DF->IN(Key);
  auto &OUTSet = DF->OUT(Key);
  initializeIN(Key.Inst, INSet);
  initializeOUT(Key.Inst, OUTSet);
}

template <unsigned K, typename ContainerT>
std::vector<
    typename CallStringInterProceduralDataFlowEngine<K, ContainerT>::ContextKey>
CallStringInterProceduralDataFlowEngine<K, ContainerT>::successors(
    const ContextKey &Key, const ICFG *ICF,
    dataflow::controlflow::FlowDirection Dir) {
  std::vector<ContextKey> Result;
  auto *Inst = Key.Inst;
  auto Ctx = Key.Ctx;

  if (Dir == dataflow::controlflow::FlowDirection::Backward) {
    if (ICF->isStartPoint(Inst,
                          dataflow::controlflow::FlowDirection::Forward)) {
      if (!Ctx.empty()) {
        auto CallerCtx = Ctx;
        auto *CallInst = CallerCtx.pop_back();
        if (CallInst != nullptr) {
          Result.push_back({CallInst, CallerCtx});
        }
      } else if (K == 0 && ICF != nullptr) {
        // K=0 is the only context-insensitive mode: entry facts may come from
        // any caller because the empty call string intentionally identifies the
        // collapsed caller summary.
        for (auto *CallInst : ICF->getCallersOf(Inst->getFunction())) {
          if (CallInst != nullptr) {
            Result.push_back({CallInst, Ctx});
          }
        }
      }
      return Result;
    }

    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      for (auto *Cont : ICF->getReturnSitesOfCallAt(Call)) {
        if (Cont != nullptr) {
          Result.push_back({Cont, Ctx});
        }
      }
      return Result;
    }

    auto ContCtx = Ctx;
    llvm::Instruction *CallInstFromCtx = nullptr;
    if (!ContCtx.empty()) {
      CallInstFromCtx = ContCtx.pop_back();
    }

    std::vector<llvm::Instruction *> CallSites;
    if (CallInstFromCtx != nullptr) {
      CallSites.push_back(CallInstFromCtx);
    } else {
      for (auto *PredInst : ICF->getPredsOf(
               Inst, dataflow::controlflow::FlowDirection::Forward)) {
        if (PredInst != nullptr && llvm::isa<llvm::CallBase>(PredInst)) {
          for (auto *RetSite : ICF->getReturnSitesOfCallAt(PredInst)) {
            if (RetSite == Inst) {
              CallSites.push_back(PredInst);
              break;
            }
          }
        }
      }
    }

    for (auto *CallSite : CallSites) {
      Context ExitCtx = Ctx;
      if (CallInstFromCtx == nullptr) {
        ExitCtx.push_back(CallSite);
      }
      for (auto *Callee : ICF->getCalleesOfCallAt(CallSite)) {
        if (Callee == nullptr || Callee->isDeclaration() || Callee->empty()) {
          continue;
        }
        for (auto *Exit : ICF->getExitPointsOf(Callee)) {
          if (Exit != nullptr) {
            Result.push_back({Exit, ExitCtx});
          }
        }
      }
    }

    if (CallInstFromCtx != nullptr) {
      for (auto *PredInst : ICF->getPredsOf(
               Inst, dataflow::controlflow::FlowDirection::Forward)) {
        if (PredInst != nullptr && PredInst != CallInstFromCtx) {
          // Stay in the current callee context while moving backward within the
          // function. The caller frame is only popped when we cross the callee
          // entry/start-point boundary.
          Result.push_back({PredInst, Ctx});
        }
      }
      return Result;
    }

    for (auto *PredInst :
         ICF->getPredsOf(Inst, dataflow::controlflow::FlowDirection::Forward)) {
      if (PredInst != nullptr) {
        Result.push_back({PredInst, Ctx});
      }
    }
    return Result;
  }

  if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Inst)) {
    if (!Ctx.empty()) {
      auto CallerCtx = Ctx;
      auto *CallInst = CallerCtx.pop_back();
      for (auto *Cont : ICF->getReturnSitesOfCallAt(CallInst)) {
        if (Cont != nullptr) {
          Result.push_back({Cont, CallerCtx});
        }
      }
    } else if (K == 0 && ICF != nullptr) {
      // K=0 is context-insensitive, so a callee exit flows to all callers.
      for (auto *CallInst : ICF->getCallersOf(Ret->getFunction())) {
        for (auto *Cont : ICF->getReturnSitesOfCallAt(CallInst)) {
          if (Cont != nullptr) {
            Result.push_back({Cont, Ctx});
          }
        }
      }
    }
    return Result;
  }

  if (isCallToDefinedFunction(Inst, ICF)) {
    // Call-to-return successors always exist (even for defined callees).
    for (auto *SuccInst : ICF->getReturnSitesOfCallAt(Inst)) {
      if (SuccInst != nullptr) {
        Result.push_back({SuccInst, Ctx});
      }
    }

    // Call edges to all defined callees.
    for (auto *Callee : ICF->getCalleesOfCallAt(Inst)) {
      if (Callee == nullptr || Callee->isDeclaration() || Callee->empty()) {
        continue;
      }
      Context CalleeCtx = Ctx;
      CalleeCtx.push_back(Inst);
      for (auto *Start : ICF->getStartPointsOf(Callee)) {
        if (Start != nullptr) {
          Result.push_back({Start, CalleeCtx});
        }
      }
    }
    return Result;
  }

  for (auto *SuccInst :
       ICF->getSuccsOf(Inst, dataflow::controlflow::FlowDirection::Forward)) {
    if (SuccInst != nullptr) {
      Result.push_back({SuccInst, Ctx});
    }
  }
  return Result;
}

template <unsigned K, typename ContainerT>
std::vector<
    typename CallStringInterProceduralDataFlowEngine<K, ContainerT>::ContextKey>
CallStringInterProceduralDataFlowEngine<K, ContainerT>::predecessors(
    const ContextKey &Key,
    const std::map<llvm::Instruction *, std::vector<llvm::Instruction *>>
        &CallToReturns,
    const std::map<llvm::Instruction *, std::vector<llvm::Instruction *>>
        &ContinuationToCalls,
    const ICFG *ICF, dataflow::controlflow::FlowDirection Dir) {
  std::vector<ContextKey> Result;
  auto *Inst = Key.Inst;
  auto Ctx = Key.Ctx;

  if (Dir == dataflow::controlflow::FlowDirection::Backward) {
    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      Context CalleeCtx = Ctx;
      CalleeCtx.push_back(Call);
      for (auto *Callee : ICF->getCalleesOfCallAt(Call)) {
        if (Callee == nullptr || Callee->isDeclaration() || Callee->empty()) {
          continue;
        }
        for (auto *Start : ICF->getStartPointsOf(Callee)) {
          if (Start != nullptr) {
            Result.push_back({Start, CalleeCtx});
          }
        }
      }
      for (auto *Cont : ICF->getReturnSitesOfCallAt(Call)) {
        if (Cont != nullptr) {
          Result.push_back({Cont, Ctx});
        }
      }
      return Result;
    }

    if (ICF->isExitInst(Inst, dataflow::controlflow::FlowDirection::Forward)) {
      if (!Ctx.empty()) {
        auto CallerCtx = Ctx;
        auto *CallInst = CallerCtx.pop_back();
        for (auto *Cont : ICF->getReturnSitesOfCallAt(CallInst)) {
          if (Cont != nullptr) {
            Result.push_back({Cont, CallerCtx});
          }
        }
      } else if (K == 0 && ICF != nullptr) {
        // Only K=0 collapses caller histories into the empty context. For
        // positive K, [] is the ordinary root call string and does not
        // enumerate caller continuations at callee exits.
        for (auto *CallInst : ICF->getCallersOf(Inst->getFunction())) {
          for (auto *Cont : ICF->getReturnSitesOfCallAt(CallInst)) {
            if (Cont != nullptr) {
              Result.push_back({Cont, Ctx});
            }
          }
        }
      }
      return Result;
    }

    for (auto *PredInst :
         ICF->getSuccsOf(Inst, dataflow::controlflow::FlowDirection::Forward)) {
      if (PredInst != nullptr) {
        Result.push_back({PredInst, Ctx});
      }
    }
    return Result;
  }

  if (isFunctionEntry(Inst)) {
    if (!Ctx.empty()) {
      auto CallerCtx = Ctx;
      auto *CallInst = CallerCtx.pop_back();
      if (CallInst != nullptr) {
        Result.push_back({CallInst, CallerCtx});
      }
    } else if (K == 0 && ICF != nullptr) {
      // K=0 is context-insensitive, so the empty call string summarizes all
      // callers at the callee entry.
      for (auto *CallInst : ICF->getCallersOf(Inst->getFunction())) {
        if (CallInst != nullptr) {
          Result.push_back({CallInst, Ctx});
        }
      }
    }
    return Result;
  }

  auto ContIt = ContinuationToCalls.find(Inst);
  if (ContIt != ContinuationToCalls.end()) {
    for (auto *CallInst : ContIt->second) {
      auto CallToRetIt = CallToReturns.find(CallInst);
      if (CallToRetIt == CallToReturns.end()) {
        continue;
      }
      Context RetCtx = Ctx;
      RetCtx.push_back(CallInst);
      for (auto *RetInst : CallToRetIt->second) {
        if (RetInst == nullptr) {
          continue;
        }
        Result.push_back({RetInst, RetCtx});
      }
    }
  }

  for (auto *PredInst :
       ICF->getPredsOf(Inst, dataflow::controlflow::FlowDirection::Forward)) {
    if (PredInst != nullptr) {
      Result.push_back({PredInst, Ctx});
    }
  }
  return Result;
}

template <unsigned K, typename ContainerT>
std::unique_ptr<
    typename CallStringInterProceduralDataFlowEngine<K, ContainerT>::ResultTy>
CallStringInterProceduralDataFlowEngine<K, ContainerT>::applyForward(
    llvm::Function *Entry, const ICFG *ICF,
    std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
    std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
    std::function<void(llvm::Instruction *Inst, ContainerT &IN)> initializeIN,
    std::function<void(llvm::Instruction *Inst, ContainerT &OUT)> initializeOUT,
    std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                       const Context &PredCtx, const Context &CurrentCtx,
                       ContainerT &IN, ResultTy *DF)>
        computeIN,
    std::function<void(llvm::Instruction *Inst, const Context &Ctx,
                       ContainerT &OUT, ResultTy *DF)>
        computeOUT,
    std::function<bool(const ContainerT &, const ContainerT &)> equal) {
  // Assert ICF is non-null rather than silently returning nullptr.
  assert(ICF != nullptr &&
         "CallStringInterProceduralDataFlowEngine::applyForward: "
         "ICF must not be null");
  if (Entry == nullptr || Entry->isDeclaration()) {
    return nullptr;
  }

  auto *Module = Entry->getParent();
  Context EmptyCtx;
  ContextKey EntryKey{&*Entry->getEntryBlock().begin(), EmptyCtx};
  std::vector<ContextKey> Seeds{EntryKey};
  std::map<ContextKey, ContainerT> SeedIns;
  return applyForwardFromSeeds(
      Module, Seeds, ICF, SeedIns, std::move(computeGEN),
      std::move(computeKILL), std::move(initializeIN), std::move(initializeOUT),
      std::move(computeIN), std::move(computeOUT), std::move(equal));
}

template <unsigned K, typename ContainerT>
std::unique_ptr<
    typename CallStringInterProceduralDataFlowEngine<K, ContainerT>::ResultTy>
CallStringInterProceduralDataFlowEngine<K, ContainerT>::applyBackward(
    llvm::Function *Entry, const ICFG *ICF,
    std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
    std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
    std::function<void(llvm::Instruction *Inst, ContainerT &IN)> initializeIN,
    std::function<void(llvm::Instruction *Inst, ContainerT &OUT)> initializeOUT,
    std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                       const Context &PredCtx, const Context &CurrentCtx,
                       ContainerT &IN, ResultTy *DF)>
        computeIN,
    std::function<void(llvm::Instruction *Inst, const Context &Ctx,
                       ContainerT &OUT, ResultTy *DF)>
        computeOUT,
    std::function<bool(const ContainerT &, const ContainerT &)> equal) {
  assert(ICF != nullptr &&
         "CallStringInterProceduralDataFlowEngine::applyBackward: "
         "ICF must not be null");
  if (Entry == nullptr || Entry->isDeclaration()) {
    return nullptr;
  }

  auto *Module = Entry->getParent();
  Context EmptyCtx;
  std::vector<ContextKey> Seeds;
  for (auto *Exit : ICF->getExitPointsOf(Entry)) {
    if (Exit != nullptr) {
      Seeds.push_back(ContextKey{Exit, EmptyCtx});
    }
  }
  std::map<ContextKey, ContainerT> SeedIns;
  return applyBackwardFromSeeds(
      Module, Seeds, ICF, SeedIns, std::move(computeGEN),
      std::move(computeKILL), std::move(initializeIN), std::move(initializeOUT),
      std::move(computeIN), std::move(computeOUT), std::move(equal));
}

template <unsigned K, typename ContainerT>
std::unique_ptr<
    typename CallStringInterProceduralDataFlowEngine<K, ContainerT>::ResultTy>
CallStringInterProceduralDataFlowEngine<K, ContainerT>::applyForwardFromSeeds(
    llvm::Module *M, const std::vector<ContextKey> &Seeds, const ICFG *ICF,
    const std::map<ContextKey, ContainerT> &SeedIns,
    std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
    std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
    std::function<void(llvm::Instruction *Inst, ContainerT &IN)> initializeIN,
    std::function<void(llvm::Instruction *Inst, ContainerT &OUT)> initializeOUT,
    std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                       const Context &PredCtx, const Context &CurrentCtx,
                       ContainerT &IN, ResultTy *DF)>
        computeIN,
    std::function<void(llvm::Instruction *Inst, const Context &Ctx,
                       ContainerT &OUT, ResultTy *DF)>
        computeOUT,
    std::function<bool(const ContainerT &, const ContainerT &)> equal) {
  // Assert ICF is non-null.
  assert(ICF != nullptr && "CallStringInterProceduralDataFlowEngine::"
                           "applyForwardFromSeeds: ICF must not be null");
  if (M == nullptr || (Seeds.empty() && SeedIns.empty())) {
    return {};
  }

  auto DF = std::make_unique<ResultTy>();
  computeGenKill(M, computeGEN, computeKILL, DF.get());

  std::map<llvm::Instruction *, std::vector<llvm::Instruction *>> CallToReturns;
  std::map<llvm::Instruction *, std::vector<llvm::Instruction *>>
      ContinuationToCalls;
  for (auto &F : *M) {
    if (F.isDeclaration()) {
      continue;
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (ICF->isCallSite(&I)) {
          for (auto *Cont : ICF->getReturnSitesOfCallAt(&I)) {
            if (Cont != nullptr) {
              ContinuationToCalls[Cont].push_back(&I);
            }
          }
        }

        if (isCallToDefinedFunction(&I, ICF)) {
          std::vector<llvm::Instruction *> Returns;
          for (auto *Callee : ICF->getCalleesOfCallAt(&I)) {
            if (Callee == nullptr) {
              continue;
            }
            auto CalleeReturns = ICF->getExitPointsOf(Callee);
            for (auto *RetInst : CalleeReturns) {
              if (RetInst != nullptr) {
                Returns.push_back(RetInst);
              }
            }
          }
          CallToReturns[&I] = std::move(Returns);
        }
      }
    }
  }

  WorkQueue Queue;
  std::set<ContextKey> InQueue;
  std::set<ContextKey> Processed;

  auto Enqueue = [&](const ContextKey &Key) {
    if (InQueue.insert(Key).second) {
      Queue.push_back(Key);
    }
  };

  for (const auto &Seed : Seeds) {
    Enqueue(Seed);
  }
  for (const auto &Seed : SeedIns) {
    Enqueue(Seed.first);
  }

  // Inject explicit boundary facts for the exact seeded (instruction, context)
  // pairs before fixpoint iteration starts.
  for (const auto &Seed : SeedIns) {
    ensureInitialized(Seed.first, initializeIN, initializeOUT, DF.get());
    DF->IN(Seed.first) = Seed.second;
  }

  while (!Queue.empty()) {
    ContextKey Current = Queue.front();
    Queue.pop_front();
    InQueue.erase(Current);

    const bool FirstProcess = Processed.insert(Current).second;
    ensureInitialized(Current, initializeIN, initializeOUT, DF.get());

    auto &InSet = DF->IN(Current);
    ContainerT OldIn = InSet;
    ContainerT NewIn;
    initializeIN(Current.Inst, NewIn);
    {
      auto SeedIt = SeedIns.find(Current);
      if (SeedIt != SeedIns.end()) {
        // Preserve explicit boundary facts while still recomputing predecessor
        // contributions from scratch.
        NewIn = SeedIt->second;
      }
    }

    for (const auto &PredKey :
         predecessors(Current, CallToReturns, ContinuationToCalls, ICF,
                      dataflow::controlflow::FlowDirection::Forward)) {
      ensureInitialized(PredKey, initializeIN, initializeOUT, DF.get());
      computeIN(Current.Inst, PredKey.Inst, PredKey.Ctx, Current.Ctx, NewIn,
                DF.get());
    }
    InSet = std::move(NewIn);

    auto &OutSet = DF->OUT(Current);
    ContainerT OldOut = OutSet;
    computeOUT(Current.Inst, Current.Ctx, OutSet, DF.get());

    if (FirstProcess || !equal(OutSet, OldOut) || !equal(InSet, OldIn)) {
      for (const auto &SuccKey : successors(
               Current, ICF, dataflow::controlflow::FlowDirection::Forward)) {
        Enqueue(SuccKey);
      }
    }
  }

  return DF;
}

template <unsigned K, typename ContainerT>
std::unique_ptr<
    typename CallStringInterProceduralDataFlowEngine<K, ContainerT>::ResultTy>
CallStringInterProceduralDataFlowEngine<K, ContainerT>::applyBackwardFromSeeds(
    llvm::Module *M, const std::vector<ContextKey> &Seeds, const ICFG *ICF,
    const std::map<ContextKey, ContainerT> &SeedIns,
    std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
    std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
    std::function<void(llvm::Instruction *Inst, ContainerT &IN)> initializeIN,
    std::function<void(llvm::Instruction *Inst, ContainerT &OUT)> initializeOUT,
    std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                       const Context &PredCtx, const Context &CurrentCtx,
                       ContainerT &IN, ResultTy *DF)>
        computeIN,
    std::function<void(llvm::Instruction *Inst, const Context &Ctx,
                       ContainerT &OUT, ResultTy *DF)>
        computeOUT,
    std::function<bool(const ContainerT &, const ContainerT &)> equal) {
  assert(ICF != nullptr && "CallStringInterProceduralDataFlowEngine::"
                           "applyBackwardFromSeeds: ICF must not be null");
  if (M == nullptr || (Seeds.empty() && SeedIns.empty())) {
    return {};
  }

  auto DF = std::make_unique<ResultTy>();
  computeGenKill(M, computeGEN, computeKILL, DF.get());

  std::map<llvm::Instruction *, std::vector<llvm::Instruction *>> CallToReturns;
  std::map<llvm::Instruction *, std::vector<llvm::Instruction *>>
      ContinuationToCalls;
  for (auto &F : *M) {
    if (F.isDeclaration()) {
      continue;
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (ICF->isCallSite(&I)) {
          for (auto *Cont : ICF->getReturnSitesOfCallAt(&I)) {
            if (Cont != nullptr) {
              ContinuationToCalls[Cont].push_back(&I);
            }
          }
        }

        if (isCallToDefinedFunction(&I, ICF)) {
          std::vector<llvm::Instruction *> Returns;
          for (auto *Callee : ICF->getCalleesOfCallAt(&I)) {
            if (Callee == nullptr) {
              continue;
            }
            auto CalleeReturns = ICF->getExitPointsOf(Callee);
            for (auto *RetInst : CalleeReturns) {
              if (RetInst != nullptr) {
                Returns.push_back(RetInst);
              }
            }
          }
          CallToReturns[&I] = std::move(Returns);
        }
      }
    }
  }

  WorkQueue Queue;
  std::set<ContextKey> InQueue;
  std::set<ContextKey> Processed;

  auto Enqueue = [&](const ContextKey &Key) {
    if (InQueue.insert(Key).second) {
      Queue.push_back(Key);
    }
  };

  for (const auto &Seed : Seeds) {
    Enqueue(Seed);
  }
  for (const auto &Seed : SeedIns) {
    Enqueue(Seed.first);
  }

  for (const auto &Seed : SeedIns) {
    ensureInitialized(Seed.first, initializeIN, initializeOUT, DF.get());
    DF->IN(Seed.first) = Seed.second;
  }

  while (!Queue.empty()) {
    ContextKey Current = Queue.front();
    Queue.pop_front();
    InQueue.erase(Current);

    const bool FirstProcess = Processed.insert(Current).second;
    ensureInitialized(Current, initializeIN, initializeOUT, DF.get());

    auto &InSet = DF->IN(Current);
    ContainerT OldIn = InSet;
    ContainerT NewIn;
    initializeIN(Current.Inst, NewIn);
    {
      auto SeedIt = SeedIns.find(Current);
      if (SeedIt != SeedIns.end()) {
        NewIn = SeedIt->second;
      }
    }

    for (const auto &PredKey :
         predecessors(Current, CallToReturns, ContinuationToCalls, ICF,
                      dataflow::controlflow::FlowDirection::Backward)) {
      ensureInitialized(PredKey, initializeIN, initializeOUT, DF.get());
      computeIN(Current.Inst, PredKey.Inst, PredKey.Ctx, Current.Ctx, NewIn,
                DF.get());
    }
    InSet = std::move(NewIn);

    auto &OutSet = DF->OUT(Current);
    ContainerT OldOut = OutSet;
    computeOUT(Current.Inst, Current.Ctx, OutSet, DF.get());

    if (FirstProcess || !equal(OutSet, OldOut) || !equal(InSet, OldIn)) {
      for (const auto &SuccKey : successors(
               Current, ICF, dataflow::controlflow::FlowDirection::Backward)) {
        Enqueue(SuccKey);
      }
    }
  }

  return DF;
}

} // namespace dataflow

#endif // LOTUS_DATAFLOW_MONO_CORE_CALLSTRINGSOLVER_H_
