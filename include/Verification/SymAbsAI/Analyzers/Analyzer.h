/**
 * @file Analyzer.h
 * @brief Fixpoint engine that runs SymAbsAI abstract interpretation
 * on a function for a given abstract domain.
 *
 * ⚠️ **IMPORTANT**: This is part of the PROGRAM-LEVEL analysis framework for
 * LLVM IR. This is NOT the same as `Solvers/SMT/SymAbs`, which provides
 * FORMULA-LEVEL abstraction algorithms for SMT formulas.
 *
 * This analyzer works on LLVM IR programs and provides:
 * - Fixpoint computation over program control flow
 * - Abstract interpretation with multiple abstract domains
 * - Integration with LLVM optimization passes
 *
 * **When to use this framework:**
 * - You're analyzing LLVM IR programs
 * - You need a complete abstract interpretation framework with fixpoint engines
 * - You want to integrate analysis into LLVM optimization passes
 *
 * **When to use Solvers/SMT/SymAbs instead:**
 * - You have SMT formulas (bit-vectors) that need abstraction
 * - You need to approximate bit-vector constraints with linear integer
 * constraints
 * - You're working at the formula/solver level, not the program level
 */
#pragma once

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Fragment.h"
#include "Verification/SymAbsAI/Core/FragmentDecomposition.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <cstdint>
#include <map>

namespace symabs_ai {
class ResultStore;

/**
 * Drives abstract interpretation over a single function.
 *
 * The analyzer owns the per-basic-block abstract states and exposes them
 * through `at()` / `after()` queries while handling fixpoint iteration and
 * optional use of dynamic (recorded) results.
 */
class Analyzer {
public:
  enum mode_t { FULL = 0, ONLY_DYNAMIC, ABS_POINTS_DYNAMIC };

protected:
  const FunctionContext &FunctionContext_;
  const FragmentDecomposition &Fragments_;
  DomainConstructor Domain_;
  mode_t Mode_;

  std::map<llvm::BasicBlock *, unique_ptr<AbstractValue>> Results_;
  std::map<llvm::BasicBlock *, unique_ptr<AbstractValue>> BBEndResults_;
  std::set<llvm::BasicBlock *> AbstractionPoints_;

  /**
   * Points to the currently analyzed fragment. Only used for debugging and
   * statistics.
   */
  mutable const Fragment *CurrentFragment_;

  /**
   * Maps each abstraction point A to the set of abstraction points that have
   * to be updated when the results for A change.
   */
  std::map<llvm::BasicBlock *, std::set<llvm::BasicBlock *>> Infl_;

  /**
   * Set of all abstraction points for which the fixpoint has been reached.
   */
  std::set<llvm::BasicBlock *> Stable_;

  /**
   * Maps each location in the CFG to the set of all the fragments that
   * contain that location.
   */
  std::map<llvm::BasicBlock *, std::set<const Fragment *>> FragMap_;

  /**
   * Wraps a call to z3::solver::check() with time measurements and records
   * some SMT statistics.
   *
   * Emits a CSV record to the verbose output.
   */
  z3::check_result
  checkWithStats(z3::solver *solver,
                 std::vector<z3::expr> *assumptions = nullptr) const;

  /**
   *  Returns a unique_ptr to an initial AbstractValue for bb. This is either
   *  a bottom value or a value given in a ResultStore, if one is specified in
   *  the module.
   */
  std::unique_ptr<AbstractValue> createInitialValue(DomainConstructor &domain,
                                                    llvm::BasicBlock *bb,
                                                    bool after) const;

  Analyzer(const FunctionContext &, const FragmentDecomposition &,
           const DomainConstructor &, mode_t mode = FULL);

public:
  static std::unique_ptr<Analyzer> New(const FunctionContext &fctx,
                                       const FragmentDecomposition &frag,
                                       const DomainConstructor &domain,
                                       mode_t mode = FULL);

  static std::unique_ptr<Analyzer> New(const FunctionContext &fctx,
                                       const FragmentDecomposition &frag,
                                       mode_t mode = FULL);

  /// Profile: bestTransformer vs SMT solver calls. Reset at start of each
  /// analysis.
  static void resetBestTransformerCallCount();
  static std::uint64_t getBestTransformerCallCount();
  static void resetSmtSolverCallCount();
  static std::uint64_t getSmtSolverCallCount();

protected:
  /// Called by derived bestTransformer() implementations to count each
  /// bestTransformer call.
  static void countBestTransformerCall();
  /// Called from checkWithStats() to count each actual SMT solver invocation.
  static void countSmtSolverCall();

public:
  /**
   * Computes the strongest consequence of a formula and an abstract value.
   *
   * Modifies the passed abstract value `result`. The new value will be
   * subsumed both by the old value and the passed logical formula. Pass
   * bottom as `result` to compute the symbolic abstraction
   * \f$\hat\alpha(\phi)\f$.
   *
   * The variable mapping represented by `vmap' will be used to map the
   * abstract value to SMT variables.
   *
   * \returns true if `result` has changed
   */
  virtual bool strongestConsequence(AbstractValue *result, z3::expr phi,
                                    const ValueMapping &vmap) const = 0;

  /**
   * Computes the best abstract transformer for a Fragment.
   *
   * If \f$f\f$ is the concrete transformer associated with `fragment`, then
   * `result` will be updated to
   * \f$result \sqcup (\alpha \circ f \circ \gamma)(input)\f$.
   *
   * \returns true if `result` has changed
   */
  virtual bool bestTransformer(const AbstractValue *input,
                               const Fragment &fragment,
                               AbstractValue *result) const;

  /**
   * Returns the analysis result for a given location.
   *
   * Computes the abstract value representing all program states at a given
   * location (after the phi nodes in the basic block but before any non-phi
   * node). Result is cached so this method can be called multiple times on
   * the same argument without compromising performance.
   *
   * The returned pointer is owned by this Analyzer object and must not be
   * deallocated.
   */
  AbstractValue *at(llvm::BasicBlock *location);

  /**
   * Returns the analysis results after a given basic block.
   *
   * Computes the abstract value representing all program states after the
   * execution of a given basic block. Result is cached so you may call this
   * method multiple times without compromising performance.
   *
   * The returned pointer is owned by this Analyzer object and must not be
   * deallocated.
   */
  AbstractValue *after(llvm::BasicBlock *location);

  virtual ~Analyzer() {}
};

class UnilateralAnalyzer : public Analyzer {
private:
  struct TransfCacheData {
    z3::solver solver;
    std::vector<z3::expr> ind_vars;

    TransfCacheData(z3::context &ctx) : solver(ctx) {}
  };

  mutable std::map<const Fragment *, unique_ptr<TransfCacheData>> TransfCache_;

  // variable used to enable or disable old inputs of an abstract transformer
  static constexpr const char *INPUT_VAR_PREFIX = "__INPUT_ACTIVE_";

public:
  UnilateralAnalyzer(const FunctionContext &s, const FragmentDecomposition &fd,
                     const DomainConstructor &ad, mode_t mode)
      : Analyzer(s, fd, ad, mode) {}

  virtual bool bestTransformer(const AbstractValue *input,
                               const Fragment &fragment,
                               AbstractValue *result) const override;

  bool strongestConsequence(AbstractValue *result, const ValueMapping &vmap,
                            z3::solver *solver,
                            std::vector<z3::expr> *assumptions) const;

  virtual bool strongestConsequence(AbstractValue *result, z3::expr phi,
                                    const ValueMapping &vmap) const override {
    z3::solver solver(phi.ctx());
    solver.add(phi);
    std::vector<z3::expr> assumptions;
    return strongestConsequence(result, vmap, &solver, &assumptions);
  }
};

class BilateralAnalyzer : public Analyzer {
public:
  BilateralAnalyzer(const FunctionContext &s, const FragmentDecomposition &fd,
                    const DomainConstructor &ad, mode_t mode)
      : Analyzer(s, fd, ad, mode) {}

  virtual bool strongestConsequence(AbstractValue *result, z3::expr phi,
                                    const ValueMapping &vmap) const;
};

class OMTAnalyzer : public Analyzer {
private:
  enum class OptimizeStatus { Sat, Unsat, Unknown };

  OptimizeStatus runOptimize(const z3::expr &objective, const z3::expr &phi,
                             const ValueMapping &vmap, AbstractValue *target,
                             bool maximize, unsigned timeout_ms) const;
  static OptimizeStatus
  runOptimizeWithRetry(const OMTAnalyzer *analyzer, const z3::expr &objective,
                       const z3::expr &phi, const ValueMapping &vmap,
                       AbstractValue *target, bool maximize,
                       unsigned timeout_ms, bool retry_unknown);
  bool overapproximateToTop(AbstractValue *result) const;

public:
  OMTAnalyzer(const FunctionContext &s, const FragmentDecomposition &fd,
              const DomainConstructor &ad, mode_t mode)
      : Analyzer(s, fd, ad, mode) {}

  virtual bool strongestConsequence(AbstractValue *result, z3::expr phi,
                                    const ValueMapping &vmap) const override;
};

} // namespace symabs_ai
