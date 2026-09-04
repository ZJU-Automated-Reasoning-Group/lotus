/** @file CommandlineArgs.h @brief Command-line argument definitions for FiTx
 * analysis. */
#pragma once
#include "llvm/Support/CommandLine.h"

#include "Checker/Framework/Subcommands.h"

namespace fitx {
namespace CommandLineArgs {
llvm::cl::opt<bool> ReportAllCandidates(
    "fitx.report-all-candidates", llvm::cl::desc("Report all possible errors"),
    llvm::cl::sub(lotus::checker::tooling::fitxSubCommand()));
} // namespace CommandLineArgs
} // namespace fitx
