/*
 * LotusAA - Function Pointer Resolution Results
 * 
 * Manages indirect call resolution results, tracking which functions
 * might be called at each call site.
 */

#pragma once

#include <map>
#include <set>

#include "Alias/LotusAA/MemoryModel/Types.h"
#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>

namespace llvm {

/// Map of function targets to the condition under which the edge is feasible.
using CallTargetSet = std::map<Function *, path_cond_t, llvm_cmp>;

/// Map from call site (Value*) to possible targets
using CallSiteTargetMap = std::map<Value *, CallTargetSet, llvm_cmp>;

/**
 * FunctionPointerResults - Indirect call resolution database
 * 
 * Stores the results of pointer analysis on function pointers,
 * mapping each indirect call site to its possible target functions.
 * Supports incremental updates and change detection.
 */
class FunctionPointerResults {
public:
  FunctionPointerResults() = default;
  ~FunctionPointerResults() = default;

  /// Get all targets for a specific call site in a function
  /// Returns nullptr if no targets found
  CallTargetSet *getTargets(Function *caller, Value *callsite);
  const CallTargetSet *getTargets(Function *caller, Value *callsite) const;

  /// Add a target function for a call site
  void addTarget(Function *caller, Value *callsite, Function *target);

  /// Set all targets for a call site (replaces existing)
  void setTargets(Function *caller, Value *callsite, const CallTargetSet &targets);

  /// Get all call sites for a function
  CallSiteTargetMap *getCallSites(Function *caller);
  const CallSiteTargetMap *getCallSites(Function *caller) const;

  /// Check if targets have changed for a function
  /// Compares old targets with new results and returns true if different
  bool hasChanged(Function *caller, 
                  const CallSiteTargetMap &newResults,
                  std::set<Function *> &outChangedCallers) const;

  /// Update targets for a function and detect changes
  /// Returns true if any targets changed
  bool updateAndDetectChanges(Function *caller,
                              const CallSiteTargetMap &newResults);

  /// Clear all results
  void clear();

  /// Get total number of indirect call sites tracked
  size_t getCallSiteCount() const;

  /// Access to underlying map (for iteration/compatibility)
  using ResultsMap = std::map<Function *, CallSiteTargetMap, llvm_cmp>;
  ResultsMap &getResultsMap() { return results_; }
  const ResultsMap &getResultsMap() const { return results_; }

private:
  /// Main storage: Function -> CallSite -> Targets
  ResultsMap results_;
};

} // namespace llvm
