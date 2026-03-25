// Implementation of the SemiSparsePointerAnalysis class.
//
// This file implements the high-level driver for the semi-sparse pointer
// analysis. It orchestrates the two main phases of the analysis:
// 1. Global Initialization: Processing global variables and their initializers.
// 2. Data-Flow Analysis: Running the fixpoint algorithm on the semi-sparse CFG.

#include "Alias/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"

#include "Alias/TPA/PointerAnalysis/Analysis/GlobalPointerAnalysis.h"
#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/Engine/Initializer.h"
#include "Alias/TPA/PointerAnalysis/Engine/SemiSparsePropagator.h"
#include "Alias/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/TPA/PointerAnalysis/Program/SemiSparseProgram.h"
#include "Alias/TPA/Util/AnalysisEngine/DataFlowAnalysis.h"
#include "Alias/TPA/Util/Log.h"

namespace tpa {

// Main entry point for the analysis on a semi-sparse program.
//
// The analysis proceeds in two distinct phases:
//
// Phase 1: Global Initialization
// Uses GlobalPointerAnalysis to scan the module for global variables and
// functions. It populates the initial Environment (Env) with mappings for
// globals and the initial Store with the effects of global initializers.
//
// Phase 2: Data-Flow Analysis
// Sets up the GlobalState (which holds immutable context for the analysis) and
// the DataFlowAnalysis engine. The engine uses:
// - GlobalState: Shared state (program, type maps, managers)
// - Memo: Memoization table for avoiding redundant work
// - TransferFunction: Evaluator for individual program points
// - SemiSparsePropagator: Strategy for traversing the CFG and updating state
//
// The analysis runs until a fixpoint is reached.
void SemiSparsePointerAnalysis::runOnProgram(const SemiSparseProgram &ssProg) {
  LOG_INFO("Phase 1: Analyzing global pointers and initializers...");
  auto initStore = Store();
  // Initialize globals and get the starting environment and store.
  std::tie(env, initStore) =
      GlobalPointerAnalysis(ptrManager, memManager, ssProg.getTypeMap())
          .runOnModule(ssProg.getModule());
  LOG_INFO("Global pointer analysis completed");

  LOG_INFO("Phase 2: Running data-flow analysis...");
  // Construct the global state that will be passed to transfer functions.
  // Note that 'env' is passed by reference and will be updated during analysis.
  auto globalState = GlobalState(ptrManager, memManager, ssProg, extTable, env);

  // Configure the generic DataFlowAnalysis engine with our specific components.
  auto dfa = util::DataFlowAnalysis<GlobalState, Memo, TransferFunction,
                                    SemiSparsePropagator>(globalState, memo);

  // Run the fixpoint iteration starting with the initial store derived from
  // globals.
  dfa.runOnInitialState<Initializer>(std::move(initStore));
  LOG_INFO("Data-flow analysis completed");
}

// Internal implementation of getPtsSet required by the CRTP base class.
// Looks up the points-to set in the computed environment.
PtsSet SemiSparsePointerAnalysis::getPtsSetImpl(const Pointer *ptr) const {
  return env.lookup(ptr);
}

} // namespace tpa