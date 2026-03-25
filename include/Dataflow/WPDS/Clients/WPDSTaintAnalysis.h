/**
 * @file WPDSTaintAnalysis.h
 * @brief Header for taint analysis using WPDS-based dataflow engine
 *
 * Author: rainoftime
 */

#ifndef ANALYSIS_WPDS_TAINT_ANALYSIS_H_
#define ANALYSIS_WPDS_TAINT_ANALYSIS_H_

#include "Dataflow/Mono/Support/Result.h"

#include <memory>

#include <llvm/IR/Module.h>

/**
 * @brief Runs taint analysis and returns detailed results
 *
 * This analysis tracks the flow of tainted (untrusted) data through the program
 * and can identify potential security vulnerabilities when tainted data reaches
 * dangerous sinks.
 *
 * @param module The LLVM module to analyze
 * @return Analysis result containing IN/OUT/GEN/KILL sets for each instruction
 */
std::unique_ptr<mono::DataFlowResult> runTaintAnalysis(llvm::Module &module);

/**
 * @brief Demo function showing how to use the taint analysis
 *
 * This function runs the taint analysis and reports potential security issues.
 *
 * @param module The LLVM module to analyze
 */
void demoTaintAnalysis(llvm::Module &module);

#endif // ANALYSIS_WPDS_TAINT_ANALYSIS_H_
