/**
 * @file WPDSConstantPropagation.h
 * @brief Header for constant propagation analysis using WPDS-based dataflow
 * engine
 *
 * Author: rainoftime
 */

#ifndef ANALYSIS_WPDS_CONSTANT_PROPAGATION_H_
#define ANALYSIS_WPDS_CONSTANT_PROPAGATION_H_

#include "Dataflow/Mono/Support/Result.h"

#include <memory>

#include <llvm/IR/Module.h>

/**
 * @brief Runs constant propagation analysis and returns detailed results
 *
 * @param module The LLVM module to analyze
 * @return Analysis result containing IN/OUT/GEN/KILL sets for each instruction
 */
std::unique_ptr<mono::DataFlowResult>
runConstantPropagationAnalysis(llvm::Module &module);

/**
 * @brief Demo function showing how to use the constant propagation analysis
 *
 * @param module The LLVM module to analyze
 */
void demoConstantPropagationAnalysis(llvm::Module &module);

#endif // ANALYSIS_WPDS_CONSTANT_PROPAGATION_H_
