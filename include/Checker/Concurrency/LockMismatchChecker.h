#ifndef LOCK_MISMATCH_CHECKER_H
#define LOCK_MISMATCH_CHECKER_H

#include "Analysis/Concurrency/LockSet/LockSetAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include "Checker/Concurrency/ConcurrencyBugReport.h"

#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace concurrency {

/**
 * @brief Specialized checker for lock usage errors
 *
 * Detects:
 * - Double lock (for non-recursive locks)
 * - Double unlock
 * - Unlock without lock
 * - Lock leaks (returning with lock held)
 * - RAII lock misuse patterns (C++11/17/20)
 */
class LockMismatchChecker {
public:
  explicit LockMismatchChecker(llvm::Module &module,
                               mhp::LockSetAnalysis *locksetAnalysis,
                               ThreadAPI *threadAPI);

  /**
   * @brief Check for lock misuse
   * @return Vector of bug reports
   */
  std::vector<ConcurrencyBugReport> checkLockMisuse();

private:
  llvm::Module &m_module;
  mhp::LockSetAnalysis *m_locksetAnalysis;
  ThreadAPI *m_threadAPI;

  // Helper methods
  std::string getInstructionLocation(const llvm::Instruction *inst) const;

  /**
   * @brief Check for RAII lock misuse patterns
   * @param func Function to analyze
   * @param reports Vector to append reports to
   */
  void checkRAIILockMisuse(llvm::Function &func,
                           std::vector<ConcurrencyBugReport> &reports);
};

} // namespace concurrency

#endif // LOCK_MISMATCH_CHECKER_H
