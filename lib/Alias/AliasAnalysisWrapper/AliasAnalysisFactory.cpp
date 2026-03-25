/**
 * @file AliasAnalysisFactory.cpp
 * @brief Factory methods for creating AliasAnalysisWrapper instances
 * 
 * This file implements the AliasAnalysisFactory class which provides
 * convenient factory methods for creating AliasAnalysisWrapper instances:
 * - create() - Create wrapper with specified configuration
 * - createAuto() - Create wrapper with default configuration
 * - getTypeName() - Get human-readable name for configuration
 * - Convenience methods for common configurations
 */

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include <llvm/IR/Module.h>
#include <memory>

using namespace llvm;
using namespace lotus;

/**
 * @brief Create an AliasAnalysisWrapper with the specified configuration
 * 
 * Factory method that creates a new AliasAnalysisWrapper instance for the given
 * module using the provided configuration. The wrapper will be automatically
 * initialized with the appropriate backend.
 * 
 * @param M The LLVM module to analyze
 * @param config The alias analysis configuration specifying implementation,
 *               context sensitivity, and other parameters
 * @return A unique_ptr to the newly created AliasAnalysisWrapper
 * 
 * @note The wrapper takes ownership of the module reference (does not copy it)
 * @note Initialization happens in the constructor; check isInitialized() after
 *       creation to verify successful initialization
 */
std::unique_ptr<AliasAnalysisWrapper> AliasAnalysisFactory::create(Module &M, const AAConfig &config) {
  return std::make_unique<AliasAnalysisWrapper>(M, config);
}

/**
 * @brief Create an AliasAnalysisWrapper with default configuration
 * 
 * Convenience factory method that creates a wrapper with the default configuration
 * (SparrowAA, context-insensitive). This is useful when you don't need specific
 * analysis features.
 * 
 * @param M The LLVM module to analyze
 * @return A unique_ptr to the newly created AliasAnalysisWrapper with default config
 * 
 * @note Default is SparrowAA with no context sensitivity (fastest option)
 */
std::unique_ptr<AliasAnalysisWrapper> AliasAnalysisFactory::createAuto(Module &M) {
  return create(M, AAConfig::SparrowAA_NoCtx());
}

/**
 * @brief Get a human-readable name for an alias analysis configuration
 * 
 * Returns a descriptive string representation of the configuration, useful for
 * logging, debugging, and user-facing output.
 * 
 * @param config The alias analysis configuration
 * @return A string describing the configuration (e.g., "SparrowAA(1-CFA)")
 * 
 * @note This is a convenience wrapper around AAConfig::getName()
 */
std::string AliasAnalysisFactory::getTypeName(const AAConfig &config) {
  return config.getName();
}

/**
 * @brief Create a SparrowAA (Andersen-style) alias analysis wrapper
 * 
 * Convenience factory method for creating SparrowAA instances with configurable
 * k-CFA context sensitivity. SparrowAA is an inclusion-based pointer analysis
 * that provides good precision/performance tradeoffs.
 * 
 * @param M The LLVM module to analyze
 * @param kCFA The k-CFA context sensitivity level (0 = context-insensitive)
 * @return A unique_ptr to the newly created AliasAnalysisWrapper
 * 
 * @note kCFA = 0: Context-insensitive (fastest, least precise)
 * @note kCFA = 1: 1-CFA (good balance)
 * @note kCFA = 2: 2-CFA (more precise, slower)
 * @note kCFA > 2: Custom k-CFA level (may be slow for large programs)
 */
std::unique_ptr<AliasAnalysisWrapper> AliasAnalysisFactory::createSparrowAA(Module &M, unsigned kCFA) {
  if (kCFA == 0) {
    return create(M, AAConfig::SparrowAA_NoCtx());
  } else if (kCFA == 1) {
    return create(M, AAConfig::SparrowAA_1CFA());
  } else if (kCFA == 2) {
    return create(M, AAConfig::SparrowAA_2CFA());
  } else {
    // Custom k-CFA
    AAConfig config = AAConfig::SparrowAA_NoCtx();
    config.ctxSens = AAConfig::ContextSensitivity::KCallSite;
    config.kLimit = kCFA;
    return create(M, config);
  }
}

/**
 * @brief Create an AserPTA alias analysis wrapper
 * 
 * Convenience factory method for creating AserPTA instances with configurable
 * k-CFA context sensitivity. AserPTA is a high-performance pointer analysis
 * with multiple solver algorithms available.
 * 
 * @param M The LLVM module to analyze
 * @param kCFA The k-CFA context sensitivity level (0 = context-insensitive)
 * @return A unique_ptr to the newly created AliasAnalysisWrapper
 * 
 * @note This only constructs an AserPTA wrapper configuration. The wrapper
 *       backend currently rejects AserPTA requests until native integration is
 *       implemented.
 * @note kCFA = 0: Context-insensitive
 * @note kCFA = 1: 1-CFA
 * @note kCFA = 2: 2-CFA
 * @note kCFA > 2: Custom k-CFA level
 * @note AserPTA supports additional features like origin-sensitive analysis
 *       and different solver algorithms (Wave, Deep, Basic) - use AAConfig
 *       directly for these options
 */
std::unique_ptr<AliasAnalysisWrapper> AliasAnalysisFactory::createAserPTA(Module &M, unsigned kCFA) {
  if (kCFA == 0) {
    return create(M, AAConfig::AserPTA_NoCtx());
  } else if (kCFA == 1) {
    return create(M, AAConfig::AserPTA_1CFA());
  } else if (kCFA == 2) {
    return create(M, AAConfig::AserPTA_2CFA());
  } else {
    // Custom k-CFA
    AAConfig config = AAConfig::AserPTA_NoCtx();
    config.ctxSens = AAConfig::ContextSensitivity::KCallSite;
    config.kLimit = kCFA;
    return create(M, config);
  }
}

/**
 * @brief Create a TPA (Flow- and Context-Sensitive Semi-Sparse) alias analysis wrapper
 * 
 * Convenience factory method for creating TPA instances with configurable
 * k-CFA context sensitivity. TPA is a flow- and context-sensitive semi-sparse
 * pointer analysis that provides high precision for pointer queries.
 * 
 * @param M The LLVM module to analyze
 * @param kCFA The k-CFA context sensitivity level (0 = context-insensitive)
 * @return A unique_ptr to the newly created AliasAnalysisWrapper
 * 
 * @note kCFA = 0: Context-insensitive (fastest)
 * @note kCFA = 1: 1-CFA (good balance)
 * @note kCFA = 2: 2-CFA (more precise)
 * @note kCFA = 3: 3-CFA (very precise, may be slow)
 * @note kCFA > 3: Custom k-CFA level (may be very slow for large programs)
 * @note TPA initialization includes IR normalization and building the
 *       semi-sparse program representation, which may take time for large modules
 * @note TPA supports loading external pointer tables from configuration files
 */
std::unique_ptr<AliasAnalysisWrapper> AliasAnalysisFactory::createTPA(Module &M, unsigned kCFA) {
  if (kCFA == 0) {
    return create(M, AAConfig::TPA_NoCtx());
  } else if (kCFA == 1) {
    return create(M, AAConfig::TPA_1CFA());
  } else if (kCFA == 2) {
    return create(M, AAConfig::TPA_2CFA());
  } else if (kCFA == 3) {
    return create(M, AAConfig::TPA_3CFA());
  } else {
    return create(M, AAConfig::TPA_KCFA(kCFA));
  }
}
