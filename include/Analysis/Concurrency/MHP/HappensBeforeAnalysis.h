#ifndef HAPPENS_BEFORE_ANALYSIS_H
#define HAPPENS_BEFORE_ANALYSIS_H

#include "Analysis/Concurrency/MHP/MHPAnalysis.h"
#include "Analysis/Concurrency/Utils/CppAtomics.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>

namespace lotus {

class AliasAnalysisWrapper;

/**
 * Happens-before relation for race detection. HB is the union of:
 * - Program order (TFG): intra-thread and fork/join/lock/barrier edges.
 * - Synchronizes-with (m_sync_with): promise/future and selected modeled
 *   library/runtime edges.
 *
 * Atomic and fence synchronization edges are emitted only when the analysis can
 * prove a witness strong enough to avoid inventing reads-from relationships.
 * Remaining candidates are left in deferred counters with explicit reasons.
 */
class HappensBeforeAnalysis {
public:
  explicit HappensBeforeAnalysis(llvm::Module &module, mhp::MHPAnalysis &mhp);

  void analyze();

  /**
   * @brief Set optional alias analysis for synchronizes-with same-location
   * check
   */
  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) {
    m_alias_analysis = aa;
  }

  const std::unordered_map<std::string, size_t> &getDeferredSyncCounts() const {
    return m_deferred_sync_counts;
  }
  const std::vector<
      std::pair<const llvm::Instruction *, const llvm::Instruction *>> &
  getSynchronizesWithEdges() const {
    return m_sync_with;
  }

  /**
   * @brief Check if instruction A happens-before instruction B
   * Includes program order (TFG) and modeled synchronizes-with edges.
   * @param A The first instruction
   * @param B The second instruction
   * @return true if A happens-before B
   */
  bool happensBefore(const llvm::Instruction *A,
                     const llvm::Instruction *B) const;

  bool mustPrecede(const llvm::Instruction *A,
                   const llvm::Instruction *B) const {
    return happensBefore(A, B);
  }

private:
  struct InstPairHash {
    size_t operator()(const std::pair<const llvm::Instruction *,
                                      const llvm::Instruction *> &p) const {
      return std::hash<const llvm::Instruction *>()(p.first) ^
             std::hash<const llvm::Instruction *>()(p.second);
    }
  };

  void buildSynchronizesWith();
  void computeAtomicHappensBefore();
  bool hasProgramOrder(const llvm::Instruction *A,
                       const llvm::Instruction *B) const;
  bool canReach(const mhp::SyncNode *start, const mhp::SyncNode *end) const;
  bool canReachWithHB(const mhp::SyncNode *start,
                      const mhp::SyncNode *end) const;
  bool canReachExplicitHB(const llvm::Instruction *from,
                          const llvm::Instruction *to) const;
  bool isInstructionThreadAmbiguous(const llvm::Instruction *inst) const;
  void addExtraHBEdge(const llvm::Instruction *from,
                      const llvm::Instruction *to);
  void addExtraHBEdge(const mhp::SyncNode *from, const mhp::SyncNode *to);
  void addExplicitHBClosure(const llvm::Instruction *from,
                            const llvm::Instruction *to);
  void addExplicitHBPair(const llvm::Instruction *from,
                         const llvm::Instruction *to);
  std::vector<const llvm::Instruction *>
  collectThreadPrefixInstructions(const llvm::Instruction *inst) const;
  std::vector<const llvm::Instruction *>
  collectThreadSuffixInstructions(const llvm::Instruction *inst) const;
  std::vector<const llvm::Instruction *>
  collectHBRelevantSuffixInstructions(const llvm::Instruction *inst) const;
  bool isPostSyncInstruction(const llvm::Instruction *sync_inst,
                             const llvm::Instruction *candidate) const;
  const llvm::Instruction *
  findNearestAtomicInBlock(const llvm::Instruction *inst, bool search_backward,
                           bool require_load_like,
                           bool require_store_like) const;
  bool isFenceAnchorCompatibleInstruction(const llvm::Instruction *inst) const;
  const llvm::PostDominatorTree &
  getPostDominatorTree(const llvm::Function *func) const;

  bool sameAtomicLocation(const llvm::Instruction *store_inst,
                          const llvm::Instruction *load_inst) const;
  const llvm::Instruction *
  getSinglePrecedingAtomicLoad(const llvm::Instruction *inst) const;
  const llvm::Instruction *
  getSinglePrecedingReleaseFence(const llvm::Instruction *inst) const;
  const llvm::Instruction *
  getSingleFollowingAcquireFence(const llvm::Instruction *inst) const;
  bool hasSupportedAtomicWitness(const llvm::Instruction *inst) const;

  /**
   * @brief Check if promise and future operate on the same shared state
   */
  bool samePromiseFuturePair(const llvm::Instruction *promise,
                             const llvm::Instruction *future) const;

  /**
   * @brief Check if two call_once calls use the same once_flag
   */
  bool sameOnceFlag(const llvm::Instruction *call1,
                    const llvm::Instruction *call2) const;

  /**
   * @brief Check if two latch operations use the same latch object
   */
  bool sameLatch(const llvm::Instruction *inst1,
                 const llvm::Instruction *inst2) const;

  /**
   * @brief Check if two barrier operations use the same barrier object
   */
  bool sameBarrier(const llvm::Instruction *inst1,
                   const llvm::Instruction *inst2) const;

  const llvm::Value *traceSharedState(const llvm::Value *value) const;

  llvm::Module &m_module;
  mhp::MHPAnalysis &m_mhp;
  lotus::AliasAnalysisWrapper *m_alias_analysis = nullptr;
  std::unordered_map<const llvm::Value *, const llvm::Value *>
      m_future_shared_state;
  mutable std::unordered_map<const llvm::Value *, const llvm::Value *>
      m_shared_state_trace_cache;
  std::unordered_map<std::string, size_t> m_deferred_sync_counts;
  std::vector<const llvm::Instruction *> m_atomic_instructions;
  std::unordered_map<const mhp::SyncNode *, std::vector<const mhp::SyncNode *>>
      m_extra_hb_successors;
  mutable std::unordered_map<const llvm::Function *,
                             std::unique_ptr<llvm::PostDominatorTree>>
      m_post_dom_cache;

  /// Synchronizes-with pairs proven by non-atomic witness mechanisms.
  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
      m_sync_with;
  std::unordered_set<
      std::pair<const llvm::Instruction *, const llvm::Instruction *>,
      InstPairHash>
      m_explicit_hb_pairs;
  mutable std::unordered_map<
      std::pair<const llvm::Instruction *, const llvm::Instruction *>, bool,
      InstPairHash>
      m_hb_cache;
};

} // namespace lotus

#endif // HAPPENS_BEFORE_ANALYSIS_H
