#ifndef LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_AVAILABLEEXPRESSIONS_H_
#define LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_AVAILABLEEXPRESSIONS_H_

#include "Dataflow/Mono/Domains/AvailableExpressionsDomain.h"
#include "Dataflow/Mono/Support/Result.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

/**
 * @brief Run available expressions analysis on a function (backward variant)
 *
 * This is a **backward** dataflow analysis that computes which expressions are
 * "available" (will be computed on all paths to the end of the function without
 * being invalidated) at each program point.
 *
 * **Key insight:** This is the backward dual of "very busy expressions."
 * An expression is available-backward at point P if it will definitely be
 * computed on all paths from P to any exit point.
 *
 * **Dataflow equations (backward):**
 *   - GEN[n] = { e | n computes expression e }
 *   - KILL[n] = { e | n modifies an operand of e }
 *   - IN[n]  = (OUT[n] - KILL[n]) ∪ GEN[n]
 *   - OUT[n] = ⋂ IN[s] for all CFG successors s of n
 *
 * **Direction:** Backward (data flows from exits to entry)
 *
 * **Meet operator:** Intersection (∩) - an expression is available only if
 * it's available on ALL successor paths.
 *
 * **Applications:**
 * - Code hoisting (move computations earlier)
 * - Loop-invariant code motion preparation
 * - Redundancy elimination
 * - Partial redundancy elimination (PRE) in compilers
 *
 * **Example:**
 * ```
 * int foo(int a, int b) {
 *   // Point 1: a+b not available (might not be computed)
 *   if (condition) {
 *     x = a + b;  // Computes a+b
 *   } else {
 *     y = a + b;  // Computes a+b
 *   }
 *   // Point 2: a+b IS available (computed on both paths)
 *   return 0;
 * }
 * ```
 *
 * **Note on SSA:** In SSA form, KILL sets are easier to compute because
 * we can track which expressions use a particular SSA value.
 *
 * @param F The function to analyze
 * @return DataFlowResult containing available expression sets for each
 * instruction
 */
std::unique_ptr<DataFlowResult>
runAvailableExpressionsAnalysis(llvm::Function *F);

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_AVAILABLEEXPRESSIONS_H_
