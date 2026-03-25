#ifndef ATOMICITY_CHECKER_H
#define ATOMICITY_CHECKER_H

#include "Analysis/Concurrency/LockSet/LockSetAnalysis.h"
#include "Analysis/Concurrency/MHP/IMHPAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include "Checker/Concurrency/ConcurrencyBugReport.h"

#include <string>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace concurrency {

/**
 * @brief Represents a critical section (acquire-release pair)
 */
struct CriticalSection {
  const llvm::Instruction *Acquire;
  const llvm::Instruction *Release;
  llvm::SmallVector<const llvm::Instruction *, 16> Body;
};

/**
 * @brief Specialized checker for atomicity violation detection
 *
 * This class handles the logic for detecting potential atomicity violations
 * where operations that should be atomic may be interleaved with other
 * operations from different threads.
 */
class AtomicityChecker {
public:
  explicit AtomicityChecker(
      llvm::Module &module, mhp::IMHPAnalysis *mhpAnalysis,
      mhp::LockSetAnalysis *locksetAnalysis, ThreadAPI *threadAPI,
      lotus::AliasAnalysisWrapper *aliasAnalysis = nullptr);

  /**
   * @brief Check for atomicity violations in the module
   * @return Vector of atomicity violation reports
   */
  std::vector<ConcurrencyBugReport> checkAtomicityViolations();

private:
  // Analysis components
  llvm::Module &m_module;
  mhp::IMHPAnalysis *m_mhpAnalysis;
  mhp::LockSetAnalysis *m_locksetAnalysis;
  ThreadAPI *m_threadAPI;
  lotus::AliasAnalysisWrapper *m_aliasAnalysis;

  // Cache of critical sections per function
  llvm::DenseMap<llvm::Function *, llvm::SmallVector<CriticalSection, 4>>
      m_csPerFunc;

  // Collect all critical sections in the module
  void collectCriticalSections();

  // Helper methods for atomicity violation detection
  bool isAcquire(const llvm::Instruction *inst) const;
  bool isRelease(const llvm::Instruction *inst) const;
  mhp::LockID getLockID(const llvm::Instruction *inst) const;
  std::string getInstructionLocation(const llvm::Instruction *inst) const;
  const llvm::Instruction *
  findMatchingUnlock(const llvm::Instruction *lockInst) const;
  bool isMemoryAccess(const llvm::Instruction *inst) const;
  bool isAtomicOperation(const llvm::Instruction *inst) const;
  const llvm::Value *getMemoryLocation(const llvm::Instruction *inst) const;
  bool mayAlias(const llvm::Value *v1, const llvm::Value *v2) const;

  // Atomicity violation detection logic
  void checkCriticalSectionForAtomicityViolations(
      const llvm::Instruction *lockInst, const llvm::Instruction *unlockInst,
      std::vector<ConcurrencyBugReport> &reports);
  bool isAtomicSequence(const llvm::Instruction *start,
                        const llvm::Instruction *end) const;
  bool mayBeInterleaved(const llvm::Instruction *inst1,
                        const llvm::Instruction *inst2) const;
};

} // namespace concurrency

#endif // ATOMICITY_CHECKER_H
