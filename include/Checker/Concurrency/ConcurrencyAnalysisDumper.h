/**
 * @file ConcurrencyAnalysisDumper.h
 * @brief Dumps concurrency analysis results in human-readable and JSON formats
 *
 * This module provides functionality to dump the results of fundamental
 * concurrency analyses (MHP, LockSet, Escape) with source-level debug
 * information. Used in analysis-only mode to report facts without performing
 * bug checking.
 */

#ifndef CONCURRENCY_ANALYSIS_DUMPER_H
#define CONCURRENCY_ANALYSIS_DUMPER_H

#include "Analysis/Concurrency/LockSet/LockSetAnalysis.h"
#include "Analysis/Concurrency/MHP/IMHPAnalysis.h"
#include "Analysis/Concurrency/Memory/EscapeAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

// Forward declaration
class DebugInfoAnalysis;

namespace concurrency {

/**
 * @brief Dumps concurrency analysis results with source-level information
 *
 * This class provides methods to dump analysis results in both human-readable
 * text format and JSON format, using debug information to report facts at the
 * source-code level rather than LLVM IR level.
 */
class ConcurrencyAnalysisDumper {
public:
  /**
   * @brief Construct the dumper with analysis components
   *
   * @param module The LLVM module being analyzed
   * @param mhpAnalysis MHP analysis results
   * @param locksetAnalysis Lock set analysis results
   * @param escapeAnalysis Escape analysis results
   * @param threadAPI Thread API for identifying lock operations
   */
  ConcurrencyAnalysisDumper(llvm::Module &module,
                            mhp::IMHPAnalysis *mhpAnalysis,
                            mhp::LockSetAnalysis *locksetAnalysis,
                            lotus::EscapeAnalysis *escapeAnalysis,
                            ThreadAPI *threadAPI);

  /**
   * @brief Dump analysis results to output stream
   *
   * @param os Output stream
   * @param jsonFormat If true, output JSON; otherwise, human-readable text
   */
  void dump(llvm::raw_ostream &os, bool jsonFormat = false) const;

private:
  /**
   * @brief Dump results in human-readable text format
   */
  void dumpText(llvm::raw_ostream &os, DebugInfoAnalysis &debugInfo) const;

  /**
   * @brief Dump results in JSON format
   */
  void dumpJSON(llvm::raw_ostream &os, DebugInfoAnalysis &debugInfo) const;

  /**
   * @brief Improve location string with fallbacks when debug info is missing
   */
  std::string improveLocation(const llvm::Instruction *inst,
                              DebugInfoAnalysis &debugInfo,
                              const std::string &funcName) const;

  llvm::Module &m_module;
  mhp::IMHPAnalysis *m_mhpAnalysis;
  mhp::LockSetAnalysis *m_locksetAnalysis;
  lotus::EscapeAnalysis *m_escapeAnalysis;
  ThreadAPI *m_threadAPI;
};

} // namespace concurrency

#endif // CONCURRENCY_ANALYSIS_DUMPER_H
