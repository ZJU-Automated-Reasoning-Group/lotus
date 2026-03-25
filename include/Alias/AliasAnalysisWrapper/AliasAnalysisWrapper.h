/// Unified wrapper for alias analysis - supports multiple AA backends
#pragma once

#include <memory>
#include <string>

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

class AndersenAAResult;
class DyckAliasAnalysis;
class AllocAA;

namespace llvm {
class CFLAndersAAWrapperPass;
class CFLSteensAAWrapperPass;
} // namespace llvm

namespace seadsa {
class SeaDsaAAResult;
} // namespace seadsa

namespace UnderApprox {
class UnderApproxAA;
} // namespace UnderApprox

namespace tpa {
class SemiSparsePointerAnalysis;
class SemiSparseProgram;
} // namespace tpa

namespace lotus {
namespace analysis {
class FlowDDA;
using DemandDrivenAA = FlowDDA;
} // namespace analysis
} // namespace lotus

namespace lotus {

/**
 * @brief Configuration for alias analysis implementation
 *
 * This struct provides a type-safe, extensible way to configure which
 * alias analysis to use and how it should behave. It replaces the old
 * flat enum approach with a structured configuration.
 */
struct AAConfig {
  /**
   * @brief Alias analysis implementation/algorithm
   */
  enum class Implementation {
    // SparrowAA: Andersen-style inclusion-based analysis
    SparrowAA,

    // AserPTA: High-performance pointer analysis with multiple solvers
    AserPTA,

    // TPA: Flow- and context-sensitive semi-sparse pointer analysis
    TPA,

    // DDA: Demand-driven pointer analysis on SVFG
    DDA,

    // DyckAA: Dyck-CFL reachability based alias analysis
    DyckAA,

    // CFL-reachability based analyses (from LLVM)
    CFLAnders,
    CFLSteens,

    // Sea-DSA: Unification-based, flow-insensitive, context-sensitive
    SeaDsa,

    // AllocAA: Simple heuristic-based allocation tracking
    AllocAA,

    // UnderApprox: Under-approximate alias analysis
    UnderApprox,

    // Combined: Multiple backends merged together
    Combined,

    // LLVM built-in analyses
    BasicAA,
    TBAA,
    GlobalsAA,
    SCEVAA,
    SRAA,
  };

  Implementation impl;

  /**
   * @brief Context sensitivity mode
   */
  enum class ContextSensitivity {
    None,      // Context-insensitive
    KCallSite, // k-call-site sensitive (k-CFA)
    KOrigin,   // k-origin sensitive (AserPTA only)
    Adaptive,  // Adaptive context sensitivity (TPA only)
  };

  ContextSensitivity ctxSens;

  /**
   * @brief k-limit for k-CFA analysis (0 = context-insensitive)
   *
   * For KCallSite mode: k=0 means CI, k=1 means 1-CFA, k=2 means 2-CFA, etc.
   * For TPA: k-limit controls maximum call string depth
   */
  unsigned kLimit;

  /**
   * @brief Field sensitivity (for implementations that support it)
   *
   * true: Track individual struct fields separately
   * false: Treat entire objects as single entities
   */
  bool fieldSensitive;

  /**
   * @brief Solver algorithm (for AserPTA)
   */
  enum class Solver {
    Default, // Use default solver for the implementation
    Wave,    // WavePropagation solver (AserPTA)
    Deep,    // DeepPropagation solver (AserPTA)
    Basic,   // Basic/PartialUpdate solver (AserPTA)
  };

  Solver solver;

  /**
   * @brief Default constructor: Creates SparrowAA, context-insensitive
   * configuration
   *
   * @warning This default constructor exists for convenience but explicit
   * factory methods (e.g., SparrowAA_NoCtx()) are preferred. When using this
   * default constructor, be aware that it creates a SparrowAA
   * context-insensitive analysis configuration.
   *
   * @note For explicit configuration, prefer factory methods:
   *       - SparrowAA_NoCtx() - explicit context-insensitive SparrowAA
   *       - TPA_1CFA() - explicit TPA with 1-CFA
   *       - etc.
   */
  AAConfig()
      : impl(Implementation::SparrowAA), ctxSens(ContextSensitivity::None),
        kLimit(0), fieldSensitive(true), solver(Solver::Default) {}

  // Constructor with explicit parameters
  AAConfig(Implementation i, ContextSensitivity cs, unsigned k, bool fs,
           Solver s)
      : impl(i), ctxSens(cs), kLimit(k), fieldSensitive(fs), solver(s) {}

  // Factory methods for common configurations

  // SparrowAA variants
  static AAConfig SparrowAA_NoCtx() {
    return {Implementation::SparrowAA, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  static AAConfig SparrowAA_1CFA() {
    return {Implementation::SparrowAA, ContextSensitivity::KCallSite, 1, true,
            Solver::Default};
  }

  static AAConfig SparrowAA_2CFA() {
    return {Implementation::SparrowAA, ContextSensitivity::KCallSite, 2, true,
            Solver::Default};
  }

  // AserPTA variants
  static AAConfig AserPTA_NoCtx(Solver s = Solver::Wave) {
    return {Implementation::AserPTA, ContextSensitivity::None, 0, true, s};
  }

  static AAConfig AserPTA_1CFA(Solver s = Solver::Wave) {
    return {Implementation::AserPTA, ContextSensitivity::KCallSite, 1, true, s};
  }

  static AAConfig AserPTA_2CFA(Solver s = Solver::Wave) {
    return {Implementation::AserPTA, ContextSensitivity::KCallSite, 2, true, s};
  }

  static AAConfig AserPTA_Origin(Solver s = Solver::Wave) {
    return {Implementation::AserPTA, ContextSensitivity::KOrigin, 1, true, s};
  }

  // TPA variants
  static AAConfig TPA_NoCtx() {
    return {Implementation::TPA, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  static AAConfig TPA_1CFA() {
    return {Implementation::TPA, ContextSensitivity::KCallSite, 1, true,
            Solver::Default};
  }

  static AAConfig TPA_2CFA() {
    return {Implementation::TPA, ContextSensitivity::KCallSite, 2, true,
            Solver::Default};
  }

  static AAConfig TPA_3CFA() {
    return {Implementation::TPA, ContextSensitivity::KCallSite, 3, true,
            Solver::Default};
  }

  static AAConfig TPA_KCFA(unsigned k) {
    return {Implementation::TPA, ContextSensitivity::KCallSite, k, true,
            Solver::Default};
  }

  /// Selective: 0-CFA at direct calls, k-CFA at indirect calls.
  static AAConfig TPA_Selective(unsigned k = 1) {
    return {Implementation::TPA, ContextSensitivity::Adaptive, k, true,
            Solver::Default};
  }

  // DDA (demand-driven on SVFG)
  static AAConfig DDA_NoCtx() {
    return {Implementation::DDA, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  // Other analyses (no context sensitivity)
  static AAConfig DyckAA() {
    return {Implementation::DyckAA, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  static AAConfig CFLAnders() {
    return {Implementation::CFLAnders, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  static AAConfig CFLSteens() {
    return {Implementation::CFLSteens, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  static AAConfig SeaDsa() {
    return {Implementation::SeaDsa, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  static AAConfig AllocAA() {
    return {Implementation::AllocAA, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  static AAConfig UnderApprox() {
    return {Implementation::UnderApprox, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  static AAConfig Combined() {
    return {Implementation::Combined, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  // LLVM built-ins
  static AAConfig BasicAA() {
    return {Implementation::BasicAA, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  static AAConfig TBAA() {
    return {Implementation::TBAA, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  static AAConfig GlobalsAA() {
    return {Implementation::GlobalsAA, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  static AAConfig SCEVAA() {
    return {Implementation::SCEVAA, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  static AAConfig SRAA() {
    return {Implementation::SRAA, ContextSensitivity::None, 0, true,
            Solver::Default};
  }

  // Comparison operators
  bool operator==(const AAConfig &other) const {
    return impl == other.impl && ctxSens == other.ctxSens &&
           kLimit == other.kLimit && fieldSensitive == other.fieldSensitive &&
           solver == other.solver;
  }

  bool operator!=(const AAConfig &other) const { return !(*this == other); }

  // Get human-readable name
  std::string getName() const;
};

class AliasAnalysisWrapper {
public:
  /**
   * @brief Construct an AliasAnalysisWrapper with an explicit configuration
   *
   * @param M The LLVM module to analyze
   * @param config The alias analysis configuration - MUST be explicitly
   * specified. Use AAConfig factory methods like SparrowAA_NoCtx(), TPA_1CFA(),
   * etc.
   *
   * @note Users MUST explicitly specify which analysis to use. There is no
   * default. This ensures users are aware of what analysis is running and its
   * implications.
   *
   * @example
   * ```cpp
   * // Explicit specification required
   * AliasAnalysisWrapper wrapper(M, AAConfig::SparrowAA_NoCtx());
   * AliasAnalysisWrapper wrapper2(M, AAConfig::TPA_1CFA());
   * ```
   */
  AliasAnalysisWrapper(llvm::Module &M, const AAConfig &config);
  ~AliasAnalysisWrapper();

  llvm::AliasResult query(const llvm::Value *v1, const llvm::Value *v2);
  llvm::AliasResult query(const llvm::MemoryLocation &loc1,
                          const llvm::MemoryLocation &loc2);

  bool mayAlias(const llvm::Value *v1, const llvm::Value *v2);
  bool mustAlias(const llvm::Value *v1, const llvm::Value *v2);
  bool mayNull(const llvm::Value *v);

  bool getPointsToSet(const llvm::Value *ptr,
                      std::vector<const llvm::Value *> &ptsSet);
  /// Get points-to set size only (for metrics). Supported by SparrowAA and TPA.
  bool getPointsToSetSize(const llvm::Value *ptr, size_t &outSize);
  /// Get possible callees for a call (direct or indirect). Supported by
  /// SparrowAA and TPA.
  void getIndirectCallTargets(llvm::CallBase *call,
                              std::vector<const llvm::Function *> &targets);
  bool getAliasSet(const llvm::Value *v,
                   std::vector<const llvm::Value *> &aliasSet);

  /**
   * @brief Get the configuration used by this wrapper
   *
   * Returns a const reference to the AAConfig that was used to initialize
   * this wrapper. This allows clients to inspect the configuration settings
   * (implementation, context sensitivity, k-limit, etc.).
   *
   * @return Const reference to the configuration
   */
  const AAConfig &getConfig() const { return _config; }

  /**
   * @brief Check if the wrapper is initialized and ready for queries
   *
   * Returns true if the alias analysis backend was successfully initialized,
   * false otherwise. If false, all queries will return conservative (MayAlias)
   * results.
   *
   * @return true if initialized and ready, false otherwise
   *
   * @note Always check this before performing expensive operations that
   *       depend on alias analysis results
   */
  bool isInitialized() const { return _initialized; }

private:
  void initialize();
  llvm::AliasResult queryBackend(const llvm::Value *v1, const llvm::Value *v2);
  bool isValidPointerQuery(const llvm::Value *v1, const llvm::Value *v2) const;

  AAConfig _config;
  llvm::Module *_module;
  bool _initialized;

  std::unique_ptr<AndersenAAResult> _andersen_aa;
  std::unique_ptr<DyckAliasAnalysis> _dyck_aa;
  std::unique_ptr<UnderApprox::UnderApproxAA> _underapprox_aa;
  std::unique_ptr<llvm::CFLAndersAAWrapperPass> _cflanders_pass;
  std::unique_ptr<llvm::CFLSteensAAWrapperPass> _cflsteens_pass;
  std::unique_ptr<AllocAA> _alloc_aa;
  std::unique_ptr<lotus::analysis::DemandDrivenAA> _dda_aa;
  std::unique_ptr<tpa::SemiSparsePointerAnalysis> _tpa_aa;
  std::unique_ptr<tpa::SemiSparseProgram> _tpa_program;

  llvm::AAResults *_llvm_aa;
  seadsa::SeaDsaAAResult *_seadsa_aa;
  void *_sraa;
};

class AliasAnalysisFactory {
public:
  static std::unique_ptr<AliasAnalysisWrapper> create(llvm::Module &M,
                                                      const AAConfig &config);
  static std::unique_ptr<AliasAnalysisWrapper> createAuto(llvm::Module &M);
  static std::string getTypeName(const AAConfig &config);

  // Convenience methods for common configurations
  static std::unique_ptr<AliasAnalysisWrapper>
  createSparrowAA(llvm::Module &M, unsigned kCFA = 0);
  static std::unique_ptr<AliasAnalysisWrapper> createAserPTA(llvm::Module &M,
                                                             unsigned kCFA = 0);
  static std::unique_ptr<AliasAnalysisWrapper> createTPA(llvm::Module &M,
                                                         unsigned kCFA = 0);
};

// ===== Utility Functions =====

/**
 * @brief Parse a string to AAConfig
 *
 * Parses common string representations of alias analysis configurations
 * and returns the corresponding AAConfig. Supports:
 * - "andersen", "sparrow-aa", "sparrowaa" -> SparrowAA_NoCtx
 * - "andersen-1cfa", "sparrow-aa-1cfa", "1cfa" -> SparrowAA_1CFA
 * - "andersen-2cfa", "sparrow-aa-2cfa", "2cfa" -> SparrowAA_2CFA
 * - "aser-pta", "aserpta" -> AserPTA_NoCtx
 * - "aser-pta-1cfa" -> AserPTA_1CFA
 * - "tpa", "tpa-0cfa" -> TPA_NoCtx
 * - "tpa-1cfa" -> TPA_1CFA
 * - "tpa-2cfa" -> TPA_2CFA
 * - "dyck", "dyckaa" -> DyckAA
 * - "cfl-anders", "cflanders" -> CFLAnders
 * - "cfl-steens", "cflsteens" -> CFLSteens
 * - "seadsa" -> SeaDsa
 * - "allocaa", "alloc" -> AllocAA
 * - "combined" -> Combined
 * - "underapprox" -> UnderApprox
 *
 * @param str String representation of the alias analysis
 * @param fallback Config to return if string is unknown - MUST be explicitly
 * specified. This ensures users are aware of what fallback analysis will be
 * used.
 * @return AAConfig corresponding to the string, or fallback if unknown
 *
 * @note The fallback parameter is required - there is no default. This ensures
 *       users explicitly choose what analysis to use when parsing fails.
 */
AAConfig parseAAConfigFromString(const std::string &str,
                                 const AAConfig &fallback);

} // namespace lotus
