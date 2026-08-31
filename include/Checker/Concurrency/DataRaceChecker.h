/** @file DataRaceChecker.h @brief Data race detection checker for concurrent programs. */
#ifndef DATA_RACE_CHECKER_H
#define DATA_RACE_CHECKER_H

#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "Checker/Concurrency/ConcurrencyBugReport.h"
#include "Concurrency/MHP/IMHPAnalysis.h"
#include "Concurrency/Memory/EscapeAnalysis.h"
#include "Concurrency/Memory/StaticThreadSharingAnalysis.h"
#include "Concurrency/Utils/ThreadAPI.h"
#include "Concurrency/Utils/ThreadLocalAnalysis.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace lotus {
class AliasAnalysisWrapper;
class HappensBeforeAnalysis;
namespace analysis {
class SparseFlowSensitivePTA;
}
} // namespace lotus

namespace concurrency {

/**
 * @brief Specialized checker for data race detection
 *
 * This class handles the logic for detecting data races between concurrent
 * memory accesses that may happen in parallel without proper synchronization.
 * Uses optional HappensBeforeAnalysis (C11 synchronizes-with) and explicit
 * independence (non-aliasing) to reduce false positives.
 *
 * Shared-memory pruning composes thread-local, escape, and static sharing
 * filters conservatively before expensive pairwise race checks.
 */
class DataRaceChecker {
public:
  explicit DataRaceChecker(
      llvm::Module &module, mhp::IMHPAnalysis *mhpAnalysis,
      mhp::LockSetAnalysis *locksetAnalysis = nullptr,
      lotus::EscapeAnalysis *escapeAnalysis = nullptr,
      ThreadLocal::ThreadLocalAnalysis *threadLocalAnalysis = nullptr,
      lotus::StaticThreadSharingAnalysis *staticThreadSharingAnalysis = nullptr,
      lotus::AliasAnalysisWrapper *aliasAnalysis = nullptr,
      lotus::HappensBeforeAnalysis *happensBeforeAnalysis = nullptr,
      const lotus::analysis::SparseFlowSensitivePTA *sparsePTA = nullptr);

  /**
   * @brief Check for data races in the module
   * @return Vector of data race reports
   */
  std::vector<ConcurrencyBugReport> checkDataRaces();

  /**
   * @brief Check if two instructions are independent (do not access same
   * location)
   */
  bool areIndependent(const llvm::Instruction *inst1,
                      const llvm::Instruction *inst2) const;

  /**
   * @brief Single predicate: would we report a data race for this pair?
   * Encapsulates MHP + happens-before + lock + alias so the definition lives in
   * one place.
   */
  bool wouldReportDataRace(const llvm::Instruction *inst1,
                           const llvm::Instruction *inst2) const;

private:
  struct InstPairHash {
    size_t operator()(const std::pair<const llvm::Instruction *,
                                      const llvm::Instruction *> &p) const {
      return std::hash<const llvm::Instruction *>()(p.first) ^
             (std::hash<const llvm::Instruction *>()(p.second) << 1U);
    }
  };

  llvm::Module &m_module;
  mhp::IMHPAnalysis *m_mhpAnalysis;
  mhp::LockSetAnalysis *m_locksetAnalysis;
  lotus::EscapeAnalysis *m_escapeAnalysis;
  ThreadLocal::ThreadLocalAnalysis *m_threadLocalAnalysis;
  lotus::StaticThreadSharingAnalysis *m_staticThreadSharingAnalysis;
  lotus::AliasAnalysisWrapper *m_aliasAnalysis;
  lotus::HappensBeforeAnalysis *m_happensBeforeAnalysis;
  const lotus::analysis::SparseFlowSensitivePTA *m_sparsePTA;

  bool mayAlias(const llvm::Value *v1, const llvm::Value *v2) const;
  bool isMemoryAccess(const llvm::Instruction *inst) const;
  bool isWriteAccess(const llvm::Instruction *inst) const;
  bool isAtomicOperation(const llvm::Instruction *inst) const;
  const llvm::Value *getMemoryLocation(const llvm::Instruction *inst) const;
  std::string getInstructionLocation(const llvm::Instruction *inst) const;

  void
  collectVariableAccesses(std::vector<const llvm::Instruction *> &accesses);
  bool mayAccessSameLocation(const llvm::Instruction *inst1,
                             const llvm::Instruction *inst2) const;
  bool isOpenMPPrivateLikeAccess(const llvm::Instruction *inst,
                                 const llvm::Value *loc) const;

  void buildSyncObjectSet();
  bool isSyncObjectAccess(const llvm::Value *loc) const;
  std::string getAccessPath(const llvm::Instruction *inst) const;

  ThreadAPI *m_threadAPI;
  std::unordered_set<const llvm::Value *> m_syncObjects;
  mutable std::unordered_map<
      std::pair<const llvm::Instruction *, const llvm::Instruction *>, bool,
      InstPairHash>
      m_location_overlap_cache;
};

} // namespace concurrency

#endif // DATA_RACE_CHECKER_H
