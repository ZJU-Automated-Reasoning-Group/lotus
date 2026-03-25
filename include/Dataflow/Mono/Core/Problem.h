#ifndef LOTUS_DATAFLOW_MONO_CORE_PROBLEM_H_
#define LOTUS_DATAFLOW_MONO_CORE_PROBLEM_H_

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/ControlFlow/FlowDirection.h"
#include "Dataflow/ControlFlow/InterCFG.h"
#include "Dataflow/ControlFlow/IntraCFG.h"
#include "Dataflow/Mono/Support/Soundness.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace mono {

struct HasNoConfigurationType {};

// ============================================================================
// IntraMonoProblem - Base class for intraprocedural monotone dataflow problems
// ============================================================================

/**
 * @brief Base class for intraprocedural monotone dataflow analyses
 *
 * This defines the interface that all intraprocedural analyses must implement.
 * The framework operates on a lattice of facts (mono_container_t) and
 * propagates them along the control flow graph using the normalFlow and merge
 * functions.
 *
 * @tparam AnalysisDomainTy The analysis domain specifying types (nodes, facts,
 * etc.)
 */
template <typename AnalysisDomainTy> class IntraMonoProblem {
public:
  using n_t = typename AnalysisDomainTy::n_t;
  using d_t = typename AnalysisDomainTy::d_t;
  using mono_container_t = typename AnalysisDomainTy::mono_container_t;
  using f_t = typename AnalysisDomainTy::f_t;
  using t_t = typename AnalysisDomainTy::t_t;
  using v_t = typename AnalysisDomainTy::v_t;
  using db_t = typename AnalysisDomainTy::db_t;
  using c_t = typename AnalysisDomainTy::c_t;
  using pt_t = typename AnalysisDomainTy::pt_t;

  using ProblemAnalysisDomain = AnalysisDomainTy;
  using ConfigurationTy = HasNoConfigurationType;

  // Unified constructor — always takes a vector of Function* directly.
  // The string-based constructor has been removed to eliminate the ambiguity
  // where getEntryPoints() returned empty even though entry points were
  // specified by name (they were only resolved lazily at solve-time).
  // Callers that previously used entry-point names should resolve them from
  // the Module before constructing the problem.
  explicit IntraMonoProblem(std::vector<llvm::Function *> EntryPoints = {},
                            pt_t PT = nullptr)
      : PT(PT), EntryPoints(std::move(EntryPoints)) {}

  // Constructor for analyses that also need the CFG and IRDB.
  IntraMonoProblem(const db_t *IRDB, const c_t *CF, pt_t PT,
                   std::vector<llvm::Function *> EntryPoints = {})
      : IRDB(IRDB), CF(CF), PT(std::move(PT)),
        EntryPoints(std::move(EntryPoints)) {}

  virtual ~IntraMonoProblem() = default;

  // ========================================
  // Core dataflow interface (must override)
  // ========================================

  /**
   * @brief Compute the flow function for a single instruction
   *
   * This defines how facts flow through a single instruction. For example,
   * in reaching definitions: OUT[inst] = GEN[inst] ∪ (IN[inst] - KILL[inst])
   *
   * @param Inst The instruction
   * @param In The facts flowing into this instruction
   * @return The facts flowing out of this instruction
   */
  virtual mono_container_t normalFlow(n_t Inst, const mono_container_t &In) = 0;

  /**
   * @brief Merge facts from multiple predecessors
   *
   * This defines the meet operator (∩) or join operator (∪) depending on
   * the lattice. For may-analyses: union. For must-analyses: intersection.
   *
   * @param Lhs First set of facts
   * @param Rhs Second set of facts
   * @return The merged result
   */
  virtual mono_container_t merge(const mono_container_t &Lhs,
                                 const mono_container_t &Rhs) = 0;

  /**
   * @brief Check if two fact sets are equal
   *
   * Used by the solver to detect fixpoint convergence.
   *
   * @param Lhs First set of facts
   * @param Rhs Second set of facts
   * @return true if equal
   */
  virtual bool equal_to(const mono_container_t &Lhs,
                        const mono_container_t &Rhs) = 0;

  // ========================================
  // Lattice configuration
  // ========================================

  /**
   * @brief Return the initial top element
   *
   * For forward may-analyses: typically empty set (bottom of the lattice)
   * For forward must-analyses: typically universe set (top of the lattice)
   * For backward analyses: depends on the lattice
   *
   * Note: the name "allTop" is inherited from Phasar convention and refers
   * to the identity element for the merge operator, which is the top of the
   * lattice for must-analyses and the bottom for may-analyses.
   */
  virtual mono_container_t allTop() { return mono_container_t{}; }

  /**
   * @brief Specify initial seed facts at specific program points
   *
   * This allows starting the analysis with known facts at specific locations.
   * For example, reaching definitions seeds the entry with function parameters.
   *
   * @return Map from program points to initial facts
   */
  virtual std::unordered_map<n_t, mono_container_t> initialSeeds() = 0;

  /**
   * @brief Specify the dataflow direction (Forward or Backward)
   */
  virtual ::dataflow::controlflow::FlowDirection direction() const {
    return ::dataflow::controlflow::FlowDirection::Forward;
  }

  /**
   * @brief Widening operator (L1 fix)
   *
   * Called by the solver instead of using the raw new value when a node has
   * been re-processed more than WideningThreshold times (see IntraSolver.h).
   * Widening must produce a value that is >= both OldVal and NewVal in the
   * lattice order, and must guarantee convergence in a finite number of steps.
   *
   * Default implementation: returns NewVal unchanged (i.e., no widening).
   * This preserves existing behaviour for analyses over finite-height lattices.
   *
   * Override this method for analyses over infinite-height lattices (e.g.,
   * interval analysis, string-length analysis) to ensure termination.
   *
   * Example — interval widening:
   * @code
   *   Interval widen(const Interval &Old, const Interval &New) override {
   *     int64_t lo = (New.lo < Old.lo) ? INT64_MIN : Old.lo;
   *     int64_t hi = (New.hi > Old.hi) ? INT64_MAX : Old.hi;
   *     return Interval{lo, hi};
   *   }
   * @endcode
   *
   * @param OldVal The value stored at the node before this iteration
   * @param NewVal The newly computed value for the node
   * @return A widened value that is >= both OldVal and NewVal
   */
  virtual mono_container_t widen(const mono_container_t &OldVal,
                                 const mono_container_t &NewVal) {
    // Default: no widening — just return the new value.
    // Analyses over finite-height lattices converge without widening.
    (void)OldVal;
    return NewVal;
  }

  // ========================================
  // Optional utilities
  // ========================================

  /**
   * @brief Pretty-print a fact container (for debugging)
   */
  virtual void printContainer(llvm::raw_ostream &,
                              const mono_container_t &) const {}

  // ========================================
  // Configuration accessors
  // ========================================

  const std::vector<llvm::Function *> &getEntryPoints() const {
    return EntryPoints;
  }

  const db_t *getProjectIRDB() const { return IRDB; }

  const c_t *getCFG() const { return CF; }

  pt_t getPointstoInfo() const { return PT; }
  pt_t getAliasAnalysis() const { return PT; }

  // setSoundness now actually stores the value and returns true.
  // Subclasses that want to adjust behavior based on soundness should check
  // this->S in their transfer functions.
  virtual bool setSoundness(Soundness NewS) {
    S = NewS;
    return true;
  }

  Soundness getSoundness() const { return S; }

protected:
  const db_t *IRDB = nullptr;
  const c_t *CF = nullptr;
  pt_t PT{};
  Soundness S = Soundness::Soundy;
  std::vector<llvm::Function *> EntryPoints;
};

// ============================================================================
// InterMonoProblem - Base class for interprocedural monotone dataflow problems
// ============================================================================

/**
 * @brief Base class for interprocedural monotone dataflow analyses
 *
 * Extends IntraMonoProblem with additional flow functions for handling
 * call sites: callFlow, returnFlow, and callToRetFlow.
 *
 * @tparam AnalysisDomainTy The analysis domain specifying types
 */
template <typename AnalysisDomainTy>
class InterMonoProblem : public IntraMonoProblem<AnalysisDomainTy> {
public:
  enum class UnresolvedCallPolicy {
    Ignore,
    WarnAndIgnore,
  };

  using n_t = typename AnalysisDomainTy::n_t;
  using d_t = typename AnalysisDomainTy::d_t;
  using f_t = typename AnalysisDomainTy::f_t;
  using mono_container_t = typename AnalysisDomainTy::mono_container_t;
  using db_t = typename AnalysisDomainTy::db_t;
  using i_t = typename AnalysisDomainTy::i_t;
  using pt_t = typename AnalysisDomainTy::pt_t;

  explicit InterMonoProblem(std::vector<llvm::Function *> EntryPoints = {},
                            pt_t PT = nullptr)
      : IntraMonoProblem<AnalysisDomainTy>(std::move(EntryPoints), PT) {}

  InterMonoProblem(const db_t *IRDB, const i_t *ICF, pt_t PT,
                   std::vector<llvm::Function *> EntryPoints = {})
      : IntraMonoProblem<AnalysisDomainTy>(IRDB, ICF, std::move(PT),
                                           std::move(EntryPoints)),
        ICF(ICF) {}

  // ========================================
  // Interprocedural flow functions (must override)
  // ========================================

  /**
   * @brief Flow function from call site to callee entry
   *
   * Models parameter passing and caller context propagation.
   *
   * @param CallSite The call instruction
   * @param Callee The called function
   * @param In Facts at the call site
   * @return Facts at the callee entry
   */
  virtual mono_container_t callFlow(n_t CallSite, f_t Callee,
                                    const mono_container_t &In) = 0;

  /**
   * @brief Flow function from callee exit to return site
   *
   * Models return value propagation and context restoration.
   *
   * @param CallSite The call instruction
   * @param Callee The called function
   * @param ExitStmt The exit instruction in the callee
   * @param RetSite The instruction after the call site
   * @param In Facts at the callee exit
   * @return Facts at the return site
   */
  virtual mono_container_t returnFlow(n_t CallSite, f_t Callee, n_t ExitStmt,
                                      n_t RetSite,
                                      const mono_container_t &In) = 0;

  /**
   * @brief Flow function bypassing the call (call-to-return edge)
   *
   * Models facts that flow directly from call to return without entering
   * the callee. Used for facts unaffected by the call.
   *
   * @param CallSite The call instruction
   * @param RetSite The instruction after the call site
   * @param Callees All possible callees
   * @param In Facts at the call site
   * @return Facts at the return site (bypassing the call)
   */
  virtual mono_container_t callToRetFlow(n_t CallSite, n_t RetSite,
                                         llvm::ArrayRef<f_t> Callees,
                                         const mono_container_t &In) = 0;

  // ========================================
  // Call graph resolution
  // ========================================

  /**
   * @brief Resolve callees at a call site
   *
   * Override to provide more precise call graph resolution (e.g., for
   * indirect calls using points-to analysis).
   *
   * Default: returns direct callee only.  For indirect calls (function
   * pointers, virtual dispatch) the default returns an empty vector and
   * emits a diagnostic warning.  Subclasses should override this
   * method and use points-to information to resolve indirect calls soundly.
   *
   * @param CallSite The call instruction
   * @return Vector of possible callees
   */
  virtual std::vector<f_t> getCalleesOfCallAt(n_t CallSite) const {
    std::vector<f_t> Callees;
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr) {
      return Callees;
    }
    if (auto *Callee = Call->getCalledFunction()) {
      Callees.push_back(Callee);
    } else {
      Callees = resolve_indirect_callees(CallSite);
      if (Callees.empty() &&
          unresolved_call_policy() == UnresolvedCallPolicy::WarnAndIgnore) {
        llvm::errs()
            << "[InterMonoProblem] WARNING: indirect call site encountered "
               "but no callee resolution was provided — callee(s) will be "
               "ignored and the analysis may be unsound.\n"
            << "  Call site: " << *CallSite << "\n";
      }
    }
    return Callees;
  }

  virtual std::vector<f_t> resolve_indirect_callees(n_t) const { return {}; }

  virtual UnresolvedCallPolicy unresolved_call_policy() const {
    return UnresolvedCallPolicy::WarnAndIgnore;
  }

  const i_t *getICFG() const { return ICF; }

protected:
  const i_t *ICF = nullptr;
};

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_CORE_PROBLEM_H_
