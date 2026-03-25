/**
 * @file Andersen.h
 * @brief Andersen-style interprocedural points-to analysis (SparrowAA).
 *
 * ## Algorithm Overview
 *
 * This implements **Andersen's subset-based, flow-insensitive** pointer
 * analysis.  Context sensitivity is configurable at construction time:
 * the default is context-insensitive (k=0), but k-call-site (k-CFA)
 * contexts can be enabled via `makeContextPolicy(k)`.
 *
 * The analysis runs in four phases:
 *
 *  1. **Object identification** (`collectConstraints`): Identifies all
 *     memory objects — globals, heap allocations, and stack allocations.
 *  2. **Constraint identification** (`collectConstraints`): Scans every
 *     instruction and emits inclusion constraints of the form `A ⊇ B`
 *     (A can point to everything B can point to).  Handles copies, loads,
 *     stores, and address-taking.
 *  3. **Offline constraint optimisation** (`optimizeConstraints`): Computes
 *     pointer equivalences (nodes with identical points-to sets) and location
 *     equivalences (nodes that always appear together in points-to sets),
 *     then collapses cycles offline to reduce the constraint graph size.
 *  4. **Constraint solving** (`solveConstraints`): Iteratively propagates
 *     inclusion constraints until a fixed point is reached.  Worst-case
 *     complexity is O(N³).
 *
 * ## Function Modelling
 *
 * Functions are modelled as structs with one field per parameter plus a
 * return slot.  Argument `i` of function `F` maps to node index
 * `getNode(F) + CallArgPos + i`; the return value maps to
 * `getNode(F) + CallReturnPos`.  This uniform representation handles
 * indirect calls naturally: an indirect call `(*fp)(a, b)` is modelled as
 * `*(fp + 1) = a`, `*(fp + 2) = b`.
 *
 * ## Context Sensitivity
 *
 * The `ContextPolicy` struct encapsulates the context abstraction:
 *  - `initialCtx` / `globalCtx`: the contexts used for the entry point and
 *    for global initialisers.
 *  - `evolve(ctx, callInst)`: computes the callee context from the caller
 *    context and the call instruction.
 *  - `k`: the call-string depth (0 = context-insensitive).
 *
 * ## Usage
 *
 * ```cpp
 * Andersen aa(M, makeContextPolicy(0));  // context-insensitive
 * std::vector<const llvm::Value *> pts;
 * if (aa.getPointsToSet(ptr, pts)) {
 *   for (auto *obj : pts) { ... }
 * }
 * ```
 */

//
// This file defines an implementation of Andersen's interprocedural alias
// analysis
//
// In pointer analysis terms, this is a subset-based, flow-insensitive
// algorithm. Context sensitivity is configurable: the default is
// context-insensitive, but k-call-site (k-CFA) contexts can be enabled at
// runtime.
//

#ifndef TCFS_ANDERSEN_H
#define TCFS_ANDERSEN_H

#include "Alias/SparrowAA/Constraint.h"
#include "Alias/SparrowAA/NodeFactory.h"
#include "Alias/SparrowAA/TemplatePtsSet.h"
#include "Alias/Spec/AliasSpecManager.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/InstrTypes.h> // For CallBase

/**
 * @struct ContextPolicy
 * @brief Encapsulates the context abstraction used by the Andersen analysis.
 *
 * A `ContextPolicy` is a vtable-like struct of function pointers that
 * defines how contexts are created, evolved at call sites, and printed.
 * This design allows the context sensitivity to be selected at runtime
 * without virtual dispatch overhead in the hot constraint-solving loop.
 *
 * - `initialCtx()` — returns the context for the program entry point.
 * - `globalCtx()` — returns the context used for global initialisers.
 * - `evolve(ctx, callInst)` — computes the callee context from the caller
 *   context and the call instruction (implements the k-CFA rule).
 * - `toString(ctx, detailed)` — converts a context to a human-readable string.
 * - `release()` — frees any heap memory owned by the policy (e.g., call-string
 *   interning tables).
 * - `k` — the call-string depth (0 = context-insensitive).
 * - `name` — a short human-readable name for the policy (e.g., "0-CFA").
 */
struct ContextPolicy {
  using Context = AndersNodeFactory::CtxKey;
  using ToStringFn = std::string (*)(Context, bool);
  using EvolveFn = Context (*)(Context, const llvm::Instruction *);

  Context (*initialCtx)(); ///< Returns the initial (entry-point) context.
  Context (*globalCtx)();  ///< Returns the context for global initialisers.
  EvolveFn evolve; ///< Computes callee context from caller context + call site.
  ToStringFn toString; ///< Converts a context to a string for debugging.
  void (*release)();   ///< Releases any resources owned by the policy.
  unsigned k;          ///< Call-string depth (0 = context-insensitive).
  const char *name;    ///< Short name of the policy (e.g., "0-CFA").
};

/**
 * @brief Create a k-call-site context policy.
 * @param kCallSite  The call-string depth.  0 = context-insensitive.
 */
ContextPolicy makeContextPolicy(unsigned kCallSite);

/**
 * @brief Return the context policy selected by the current command-line flags.
 */
ContextPolicy getSelectedAndersenContextPolicy();

/**
 * @class Andersen
 * @brief Core Andersen points-to analysis engine.
 *
 * Clients construct an `Andersen` object with a module and a context policy,
 * then query it via `getPointsToSet` / `getPointsToSetInContext`.
 *
 * The analysis is **whole-program**: it processes the entire module in the
 * constructor and stores the resulting points-to graph.  Queries are then
 * answered in O(1) by looking up the pre-computed graph.
 *
 * @note `Andersen` is not an LLVM pass; it is a plain C++ object.  The
 *       `AndersenAAResult` wrapper (in `AndersenAA.h`) adapts it to the
 *       LLVM `AAResultBase` interface.
 */
class Andersen {
private:
  /// Factory that allocates and manages all `AndersNode` objects.
  AndersNodeFactory nodeFactory;

  /// All inclusion constraints collected from the module.
  std::vector<AndersConstraint> constraints;

  /// The points-to graph: maps each node index to its points-to set.
  /// Populated by `solveConstraints()` and queried by `getPointsToSet()`.
  std::map<NodeIndex, DefaultPtsSet, std::less<NodeIndex>,
           std::allocator<std::pair<const NodeIndex, DefaultPtsSet>>>
      ptsGraph;

  /// Library-function spec manager used during constraint collection.
  lotus::alias::AliasSpecManager specManager;

  ContextPolicy ctxPolicy;              ///< The active context policy.
  AndersNodeFactory::CtxKey initialCtx; ///< Context for the entry point.
  AndersNodeFactory::CtxKey globalCtx;  ///< Context for global initialisers.

  /// Key type for the visited-functions set: (function, context) pair.
  struct FunctionContextKey {
    const llvm::Function *func;
    AndersNodeFactory::CtxKey ctx;
  };
  /// DenseSet info for `FunctionContextKey`.
  struct FunctionContextInfo {
    static FunctionContextKey getEmptyKey() {
      return {reinterpret_cast<const llvm::Function *>(-1),
              reinterpret_cast<void *>(0x1)};
    }
    static FunctionContextKey getTombstoneKey() {
      return {reinterpret_cast<const llvm::Function *>(-2),
              reinterpret_cast<void *>(0x2)};
    }
    static unsigned getHashValue(const FunctionContextKey &k) {
      return llvm::hash_combine(k.func, k.ctx);
    }
    static bool isEqual(const FunctionContextKey &lhs,
                        const FunctionContextKey &rhs) {
      return lhs.func == rhs.func && lhs.ctx == rhs.ctx;
    }
  };
  llvm::DenseSet<FunctionContextKey, FunctionContextInfo> visitedFunctions;

  // Three main phases
  void collectConstraints(const llvm::Module &);
  void collectConstraintsForFunction(const llvm::Function *,
                                     AndersNodeFactory::CtxKey);
  void optimizeConstraints();
  void solveConstraints();

  // Helper functions for constraint collection
  void collectConstraintsForGlobals(const llvm::Module &,
                                    AndersNodeFactory::CtxKey);
  void collectConstraintsForInstruction(const llvm::Instruction *,
                                        AndersNodeFactory::CtxKey);
  void addGlobalInitializerConstraints(NodeIndex, const llvm::Constant *,
                                       AndersNodeFactory::CtxKey);
  void addConstraintForCall(const llvm::CallBase *cs,
                            AndersNodeFactory::CtxKey callerCtx);
  bool addConstraintForExternalLibrary(const llvm::CallBase *cs,
                                       const llvm::Function *f,
                                       AndersNodeFactory::CtxKey callerCtx);
  void addArgumentConstraintForCall(const llvm::CallBase *cs,
                                    const llvm::Function *f,
                                    AndersNodeFactory::CtxKey callerCtx,
                                    AndersNodeFactory::CtxKey calleeCtx);

  // Helper functions for constraint optimization
  NodeIndex getRefNodeIndex(NodeIndex n) const;
  NodeIndex getAdrNodeIndex(NodeIndex n) const;

  // For debugging
  void dumpConstraint(const AndersConstraint &) const;
  void dumpConstraints() const;
  void dumpConstraintsPlainVanilla() const;
  void dumpPtsGraphPlainVanilla() const;

public:
  static char ID;

  explicit Andersen(const llvm::Module &,
                    ContextPolicy policy = makeContextPolicy(0));
  ~Andersen();
  bool runOnModule(const llvm::Module &M);

  // Given a llvm pointer v,
  // - Return false if the analysis doesn't know where v points to. In other
  // words, the client must conservatively assume v can points to everything.
  // - Return true otherwise, and the points-to set of v is put into the second
  // argument.
  bool getPointsToSet(const llvm::Value *v,
                      std::vector<const llvm::Value *> &ptsSet) const;
  bool getPointsToSet(const llvm::Value *v, AndersPtsSet &ptsSet) const;
  // Context-sensitive queries (no cross-context union). Return false if the
  // value has no node in the given context; otherwise fill the supplied set.
  bool getPointsToSetInContext(const llvm::Value *v,
                               AndersNodeFactory::CtxKey ctx,
                               std::vector<const llvm::Value *> &ptsSet) const;
  bool getPointsToSetInContext(const llvm::Value *v,
                               AndersNodeFactory::CtxKey ctx,
                               AndersPtsSet &ptsSet) const;

  // Context utilities for clients that want per-context answers.
  AndersNodeFactory::CtxKey getInitialContext() const { return initialCtx; }
  AndersNodeFactory::CtxKey getGlobalContext() const { return globalCtx; }
  AndersNodeFactory::CtxKey evolveContext(AndersNodeFactory::CtxKey prev,
                                          const llvm::Instruction *I) const {
    return ctxPolicy.evolve(prev, I);
  }
  std::string contextToString(AndersNodeFactory::CtxKey ctx,
                              bool detailed = false) const {
    return ctxPolicy.toString(ctx, detailed);
  }
  // Put all allocation sites (i.e. all memory objects identified by the
  // analysis) into the first arugment
  void
  getAllAllocationSites(std::vector<const llvm::Value *> &allocSites) const;

  friend class AndersenAAResult;
};

#endif
