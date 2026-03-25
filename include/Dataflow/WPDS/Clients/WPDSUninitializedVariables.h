/**
 * @file WPDSUninitializedVariables.h
 * @brief Header for uninitialized variables analysis using WPDS-based dataflow
 * engine
 *
 * This header provides the public interface for the uninitialized variables
 * analysis demonstration using the InterProceduralDataFlowEngine.
 *
 * Author: rainoftime
 */

#ifndef ANALYSIS_WPDS_UNINITIALIZED_VARIABLES_H_
#define ANALYSIS_WPDS_UNINITIALIZED_VARIABLES_H_

#include "Dataflow/Mono/Support/Result.h"

#include <memory>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace llvm {
class Module;
class Instruction;
} // namespace llvm

/**
 * @brief Demo function showing how to use the uninitialized variables analysis
 *
 * This function demonstrates the basic usage of the WPDS-based dataflow engine
 * for uninitialized variables analysis. It runs the analysis and reports
 * warnings to stderr.
 *
 * @param module The LLVM module to analyze
 */
void demoUninitializedVariablesAnalysis(llvm::Module &module);

/**
 * @brief Runs uninitialized variables analysis and returns detailed results
 *
 * This function provides a more advanced interface that returns the complete
 * dataflow analysis results for external processing.
 *
 * @param module The LLVM module to analyze
 * @return Analysis result containing IN/OUT/GEN/KILL sets for each instruction,
 *         or nullptr if analysis failed
 */
std::unique_ptr<mono::DataFlowResult>
runUninitializedVariablesAnalysis(llvm::Module &module);

/**
 * @brief Queries and displays analysis results for a specific instruction
 *
 * This function demonstrates how to query the dataflow analysis results
 * for a specific instruction and display detailed information.
 *
 * @param module The analyzed module
 * @param result The analysis result from runUninitializedVariablesAnalysis
 * @param targetInst The instruction to query
 */
void queryAnalysisResults(llvm::Module &module,
                          const mono::DataFlowResult &result,
                          llvm::Instruction *targetInst);

#endif // ANALYSIS_WPDS_UNINITIALIZED_VARIABLES_H_
