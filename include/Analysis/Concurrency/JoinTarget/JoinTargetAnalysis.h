/**
 * @file JoinTargetAnalysis.h
 * @brief Join-target set: which fork(s) may be joined by each pthread_join
 *
 * For each join site, computes the set of fork (pthread_create) instructions
 * whose thread ID may be the one waited on (join's arg0 may alias fork's arg0).
 * Used to improve MHP precision (unambiguous join) and thread-flow reasoning.
 *
 * @author rainoftime
 * @date 2026
 */

#ifndef JOIN_TARGET_ANALYSIS_H
#define JOIN_TARGET_ANALYSIS_H

#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include "Analysis/Concurrency/Utils/ThreadMultiplicity.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace mhp {

/**
 * @brief For each pthread_join, the set of pthread_create calls that may be joined
 */
class JoinTargetAnalysis {
public:
  explicit JoinTargetAnalysis(llvm::Module &module,
                              lotus::AliasAnalysisWrapper *aliasAnalysis = nullptr);

  void analyze();

  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) { m_aliasAnalysis = aa; }

  /**
   * @brief Fork instructions that may be joined by this join (join's arg0 may alias fork's arg0)
   */
  std::vector<const llvm::Instruction *>
  getPossibleJoinedForks(const llvm::Instruction *joinInst) const;

  /**
   * @brief Fork instructions that remain temporally feasible for this join.
   *
   * This refines alias/root matching with intra-procedural CFG feasibility to
   * avoid binding a join to creates that only reuse the same storage later.
   */
  std::vector<const llvm::Instruction *>
  getFeasibleJoinedForks(const llvm::Instruction *joinInst) const;

  /**
   * @brief True if this join has exactly one possible target fork (unambiguous join)
   */
  bool isUnambiguousJoin(const llvm::Instruction *joinInst) const;

  /**
   * @brief Trace an SSA pthread_t handle back to a stable origin when possible.
   *
   * Supports load/phi/select/bitcast/gep forwarding and, when a module is
   * provided, walks direct callers to map formal arguments back to actuals.
   */
  static const llvm::Value *traceThreadHandleRoot(const llvm::Value *value,
                                                  const llvm::Module *module = nullptr);

  /// Collect all allocas/globals reachable from value (for phi/select gives multiple roots).
  static void traceThreadHandleRoots(const llvm::Value *value,
                                     const llvm::Module *module,
                                     std::unordered_set<const llvm::Value *> &roots);

private:
  enum class CandidateCountKind { Zero, One, Many };

  void collectForksAndJoins();
  CandidateCountKind
  classifyJoinForks(const std::vector<const llvm::Instruction *> &forks) const;
  std::vector<const llvm::Instruction *>
  filterTemporallyFeasibleForks(const llvm::Instruction *joinInst,
                                const std::vector<const llvm::Instruction *> &forks) const;
  bool forkMayReachJoinInFunction(const llvm::Instruction *forkInst,
                                  const llvm::Instruction *joinInst) const;
  bool joinMayReachForkInFunction(const llvm::Instruction *joinInst,
                                  const llvm::Instruction *forkInst) const;
  const llvm::PostDominatorTree &
  getPostDominatorTree(const llvm::Function *func) const;

  llvm::Module &m_module;
  ThreadAPI *m_threadAPI;
  lotus::AliasAnalysisWrapper *m_aliasAnalysis;

  std::vector<const llvm::Instruction *> m_forkInsts;
  std::vector<const llvm::Instruction *> m_joinInsts;
  std::unordered_map<const llvm::Instruction *, const llvm::Value *>
      m_forkToRoot;
  std::unordered_map<const llvm::Instruction *, std::vector<const llvm::Instruction *>>
      m_joinToForks;
  std::unordered_map<const llvm::Instruction *,
                     std::vector<const llvm::Instruction *>>
      m_joinToFeasibleForks;
  std::unordered_set<const llvm::Instruction *> m_unambiguousJoins;
  mutable std::unordered_map<const llvm::Function *,
                             std::unique_ptr<llvm::PostDominatorTree>>
      m_postDomCache;
  mutable std::unique_ptr<concurrency::ThreadMultiplicityAnalysis>
      m_threadMultiplicity;
};

} // namespace mhp

#endif // JOIN_TARGET_ANALYSIS_H
