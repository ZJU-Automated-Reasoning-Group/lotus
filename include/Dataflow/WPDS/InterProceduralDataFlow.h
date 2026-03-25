#ifndef ANALYSIS_INTERPROCEDURALDATAFLOW_H_
#define ANALYSIS_INTERPROCEDURALDATAFLOW_H_

/**
 * Umbrella header for WPDS-based interprocedural dataflow.
 *
 * This file includes the split components for clarity and single-include
 * convenience. The structure follows the paper:
 *
 *   - DataFlowFacts: fact domain (set of facts / environment)
 *   - GenKillTransformer: semiring weight (gen/kill + relational flow)
 *   - InterProceduralDataFlowEngine: encoding supergraph as WPDS, running
 *     GPR (Algorithm 1), and extracting results
 *
 * @see Reps, Schwoon, Jha: "Weighted Pushdown Systems and their Application
 *      to Interprocedural Dataflow Analysis"
 */

#include "Dataflow/WPDS/Core/DataFlowFacts.h"
#include "Dataflow/WPDS/Core/ExplodedWPDSBuilder.h"
#include "Dataflow/WPDS/Core/GenKillTransformer.h"
#include "Dataflow/WPDS/Core/MemoryObjectFact.h"
#include "Dataflow/WPDS/Solver/InterProceduralDataFlowEngine.h"

#endif // ANALYSIS_INTERPROCEDURALDATAFLOW_H_
