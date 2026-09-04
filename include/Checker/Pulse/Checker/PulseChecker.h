/** @file PulseChecker.h @brief Main Pulse checker driver for abstract-interpretation-based bug detection. */
#ifndef CHECKER_PULSE_PULSECHECKER_H
#define CHECKER_PULSE_PULSECHECKER_H

#include "Checker/Pulse/Core/PulseValueHistory.h"
#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Pulse/Domain/PulseDisjunctiveDomain.h"
#include "Checker/Pulse/Domain/PulseDomain.h"
#include "Checker/Pulse/Domain/PulseLoopAbstraction.h"
#include "Checker/Pulse/Domain/PulseNonDisjunctiveDomain.h"
#include "Checker/Pulse/Domain/PulseOperations.h"
#include "Checker/Pulse/Interproc/PulseSpecialization.h"
#include "Checker/Pulse/Interproc/PulseSummary.h"
#include "Checker/Pulse/Interproc/PulseTransitiveInfo.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseLatentIssue.h"

#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace llvm {
class BasicBlock;
class Module;
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace pulse {

class PulseModels;

/**
 * PulseChecker: main bug finder using biabductive analysis.
 *
 * High-level model (Infer Pulse-inspired, "incorrectness logic"):
 * - The analysis is geared toward producing *witnessable* bug reports: an issue
 *   is interesting only if there exists a feasible execution reaching it.
 * - The abstract state is biabductive (`AbductiveDomain`): a post-state plus an
 *   inferred precondition that records missing heap facts required for a
 *   witness (materialized at call sites).
 * - Control-flow merging must not conjoin path conditions; joining two paths is
 *   a disjunction (best-effort) to avoid dropping feasible witnesses.
 *
 * Implementation notes:
 * - Uses CFG traversal with bounded disjunction (`kMaxDisjuncts`) and bounded
 *   interprocedural call depth (`kMaxCallDepth`).
 * - Handles GEP/PHI and common library functions via `PulseModels`.
 */
class PulseChecker {
private:
  llvm::Module *module_;
  lotus::AliasAnalysisWrapper *aa_;
  AbstractValueFactory factory_;
  PulseOperations ops_;
  std::unique_ptr<PulseModels> models_;
  static constexpr unsigned kMaxDisjuncts = 10u;
  static constexpr unsigned kMaxCallDepth = 5u;

  int useAfterFreeTypeId_;
  int nullDerefTypeId_;
  int uninitializedReadTypeId_;
  int unnecessaryCopyTypeId_;
  int constRefableParamTypeId_;
  int taintErrorTypeId_;
  int stackAddressEscapeTypeId_;
  int invalidFreeTypeId_;
  int outOfBoundsTypeId_;

  std::map<const llvm::Function *, std::vector<ExecutionDomain>>
      function_states_;
  NonDisjunctiveDomain analysis_non_disj_;
  SummaryManager summary_manager_;
  SpecializationManager specialization_manager_;

  // Disjunctive analysis and loop abstraction (optional, can be nullptr)
  std::map<const llvm::Function *, DisjunctiveDomain> disjunctive_domains_;
  std::map<const llvm::Function *, LoopAbstraction> loop_abstractions_;

  // Transitive information tracking
  std::map<const llvm::Function *, TransitiveInfo> transitive_info_;

  // Latent issues tracking
  std::vector<LatentIssue> latent_issues_;

  // During SCC-based scheduling, we treat calls within the current SCC as
  // unknown/unstable (no summaries available yet).
  std::unordered_set<const llvm::Function *> current_scc_;

public:
  explicit PulseChecker(llvm::Module *M,
                        lotus::AliasAnalysisWrapper *AA = nullptr);
  ~PulseChecker();

  void analyze();
  void analyzeFunction(const llvm::Function *F);

  std::vector<ExecutionDomain> executeInstruction(const llvm::Instruction *I,
                                                  ExecutionDomain exec_state,
                                                  const llvm::BasicBlock *pred,
                                                  unsigned call_depth);

  ExecutionDomain handleLoad(const llvm::LoadInst *LI,
                             ExecutionDomain exec_state,
                             const llvm::BasicBlock *pred);
  ExecutionDomain handleStore(const llvm::StoreInst *SI,
                              ExecutionDomain exec_state,
                              const llvm::BasicBlock *pred);
  std::vector<ExecutionDomain> handleCall(const llvm::CallInst *CI,
                                          ExecutionDomain exec_state,
                                          const llvm::BasicBlock *pred,
                                          unsigned call_depth);
  ExecutionDomain handleAlloca(const llvm::AllocaInst *AI,
                               ExecutionDomain exec_state);
  ExecutionDomain handleReturn(const llvm::ReturnInst *RI,
                               ExecutionDomain exec_state);

  void reportBug(OperationResult kind, const llvm::Instruction *loc,
                 AbstractValue addr, const Trace &trace,
                 const AbductiveDomain *astate = nullptr);

  // Rich diagnostic reporting (for models)
  void reportDiagnostic(const llvm::Instruction *loc,
                        const std::string &message, const std::string &type,
                        int confidence);

  // Overload kept for backward compatibility with older model call sites.
  // Prefer reporting structured diagnostics via DiagnosticManager directly.
  void reportDiagnostic(const Diagnostic &diagnostic);

  AbstractValueFactory &getFactory() { return factory_; }
  PulseOperations &getOperations() { return ops_; }

  void registerBugTypes();
  ExecutionDomain initializeFunction(const llvm::Function *F);

  /** Run callee from caller state at CI; returns (exit_state, return
   * AbstractValue) per return. */
  std::vector<std::pair<ExecutionDomain, std::optional<AbstractValue>>>
  runCallee(const llvm::Function *callee, const ExecutionDomain &caller_state,
            const llvm::CallInst *CI, const llvm::BasicBlock *pred,
            unsigned call_depth);

  /**
   * Create summary from function exit states
   */
  void createSummary(const llvm::Function *F,
                     const std::vector<ExecutionDomain> &exit_states,
                     const std::vector<ExecutionDomain> &latent_exit_states);

  /**
   * Apply summary at call site
   */
  std::vector<ExecutionDomain> applySummary(const llvm::Function *callee,
                                            const ExecutionDomain &caller_state,
                                            const llvm::CallInst *CI,
                                            const llvm::BasicBlock *pred);

  /**
   * Improved summary application with materialization
   */
  std::vector<ExecutionDomain>
  applySummaryImproved(const llvm::Function *callee,
                       const ExecutionDomain &caller_state,
                       const llvm::CallInst *CI, const llvm::BasicBlock *pred,
                       const PulseSummary *summary_override = nullptr);

  /** Handle comparison instructions without assuming their Boolean result. */
  ExecutionDomain handleComparison(const llvm::Instruction *I,
                                   ExecutionDomain exec_state,
                                   const llvm::BasicBlock *pred);

  /**
   * Handle library function calls (malloc, free, etc.)
   */
  std::vector<ExecutionDomain> handleLibraryCall(const llvm::CallInst *CI,
                                                 ExecutionDomain exec_state,
                                                 const llvm::BasicBlock *pred);

  /**
   * Apply branch condition for a given successor (then/else).
   * Forks state: then = condition true, else = condition false.
   * Returns the resulting state, or None if we don't fork (e.g. can't parse).
   */
  std::optional<ExecutionDomain>
  applyBranchCondition(ExecutionDomain state, const llvm::BranchInst *BI,
                       unsigned successor_index,
                       const llvm::BasicBlock *pred_bb);

  void reportUnnecessaryCopies(const llvm::Function *F);
  void reportConstRefableParams(const llvm::Function *F);

private:
  /**
   * Refine a state with the assumption that an i1 condition has the requested
   * truth value. Returns no state when the selected outcome is infeasible or
   * cannot be represented without inventing a witness.
   */
  std::optional<ExecutionDomain>
  assumeCondition(ExecutionDomain state, const llvm::Value *condition,
                  bool assumed_true, const llvm::Instruction *location,
                  const llvm::BasicBlock *pred_bb);

  const PulseSummary *resolveSummaryForCall(const llvm::Function *callee,
                                            const ExecutionDomain &caller_state,
                                            const llvm::CallInst *CI,
                                            const llvm::BasicBlock *pred);
  void materializeSpecializedSummary(const llvm::Function *callee,
                                     const PulseSummary &base_summary,
                                     const SpecializationKey &key);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSECHECKER_H
