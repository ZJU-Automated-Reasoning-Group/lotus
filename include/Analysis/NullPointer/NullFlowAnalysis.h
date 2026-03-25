/**
 * @file NullFlowAnalysis.h
 * @brief Null Pointer Flow Analysis for LLVM
 *
 * This file provides a flow-sensitive null pointer analysis that tracks
 * null/non-null states of pointers through the program. It uses a value-flow
 * graph (VFG) and a unification-based alias analysis to compute precise
 * nullness information.
 *
 * Key Features:
 * - Flow-sensitive pointer analysis
 * - Context-aware null propagation
 * - Integration with Dyck alias analysis
 * - Support for function summaries and call edges
 *
 * @author Lotus Analysis Framework
 * @date 2025
 * @ingroup NullPointer
 */

#ifndef NULLPOINTER_NULLFLOWANALYSIS_H
#define NULLPOINTER_NULLFLOWANALYSIS_H

#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckVFG.h"

#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

/**
 * @class NullFlowAnalysis
 * @brief Flow-sensitive null pointer analysis using VFG and alias analysis
 *
 * This analysis computes which pointers may be null at each program point.
 * It performs a fixpoint iteration over the value-flow graph, propagating
 * null/non-null information through assignments, phi nodes, and function calls.
 *
 * The analysis distinguishes between:
 * - Definitely non-null pointers (safe to dereference)
 * - Possibly null pointers (potential null dereference)
 * - Points-to sets for heap allocations and function parameters
 *
 * @note This is a ModulePass that analyzes the entire program
 * @note Uses DyckAliasAnalysis for alias information
 * @see DyckAliasAnalysis, DyckVFG
 */
class NullFlowAnalysis : public ModulePass {
private:
  DyckAliasAnalysis *DAA; ///< Pointer to the alias analysis instance

  DyckVFG *VFG; ///< Pointer to the value-flow graph

  /// Set of edges that are known to be non-null (from source to target)
  std::set<std::pair<DyckVFGNode *, DyckVFGNode *>> NonNullEdges;

  /// Per-function non-null edges added during analysis
  std::map<Function *, std::set<std::pair<DyckVFGNode *, DyckVFGNode *>>>
      NewNonNullEdges;

  /// Set of nodes that are known to be non-null
  std::set<DyckVFGNode *> NonNullNodes;

public:
  static char ID; ///< Pass identifier for LLVM pass registry

  /**
   * @brief Construct a new NullFlowAnalysis pass
   */
  NullFlowAnalysis();

  /**
   * @brief Destroy the NullFlowAnalysis pass
   */
  ~NullFlowAnalysis() override;

  /**
   * @brief Main analysis entry point
   * @param M The LLVM module to analyze
   * @return true if the module was modified
   */
  bool runOnModule(Module &M) override;

  /**
   * @brief Declare analysis dependencies
   * @param AU Analysis usage to register required passes
   */
  void getAnalysisUsage(AnalysisUsage &AU) const override;

public:
  /**
   * @brief Recompute null flow analysis for specific functions
   *
   * Allows incremental recomputation when new null information
   * becomes available for certain functions.
   *
   * @param functions Set of functions to recompute analysis for
   * @return true if any changes were made to the analysis results
   */
  bool recompute(std::set<Function *> &);

  /**
   * @brief Add null information for a function parameter
   * @param F The function containing the parameter
   * @param funcArg The function argument (must be a pointer type)
   * @param ptr The pointer value being added as non-null
   */
  void add(Function *, Value *, Value *);

  /**
   * @brief Add null information for a call site return value
   * @param F The function containing the call
   * @param callInst The call instruction
   * @param K Parameter index for indirect calls
   */
  void add(Function *, CallInst *, unsigned K);

  /**
   * @brief Add null information for a function return value
   * @param F The function
   * @param val The return value being added as non-null
   */
  void add(Function *, Value *);

  /**
   * @brief Query if a value is known to be non-null
   * @param V The value to query
   * @return true if V is known to be non-null at this point
   */
  bool notNull(Value *) const;
};

namespace lotus {
namespace nullpointer {
namespace testing {

void setNullFlowIncrementalLimitOverrideForTesting(int Limit);
unsigned getNullFlowIncrementalLimitForTesting();
bool isContextInsensitiveGuaranteedNonNullValueForTesting(llvm::Value *V);

} // namespace testing
} // namespace nullpointer
} // namespace lotus

#endif // NULLPOINTER_NULLFLOWANALYSIS_H
