/**
 * @file WPDSLivenessAnalysis.h
 * @brief Header for liveness analysis using WPDS-based dataflow engine
 *
 * Author: rainoftime
 */

#ifndef ANALYSIS_WPDS_LIVENESS_ANALYSIS_H_
#define ANALYSIS_WPDS_LIVENESS_ANALYSIS_H_

#include "Dataflow/Mono/Support/Result.h"

#include <memory>

#include <llvm/IR/Module.h>

/**
 * @brief Runs live variable analysis and returns detailed results
 *
 * @param module The LLVM module to analyze
 * @return Analysis result containing IN/OUT/GEN/KILL sets for each instruction
 */
std::unique_ptr<mono::DataFlowResult> runLivenessAnalysis(llvm::Module &module);

/**
 * @brief Demo function showing how to use the liveness analysis
 *
 * @param module The LLVM module to analyze
 */
void demoLivenessAnalysis(llvm::Module &module);

#endif // ANALYSIS_WPDS_LIVENESS_ANALYSIS_H_
