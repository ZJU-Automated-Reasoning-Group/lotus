/**
 * @file PDGAliasWrapper.h
 * @brief Backward-compatible wrapper for alias analysis used in PDG
 * construction
 *
 * This file provides a backward-compatible interface for PDG construction that
 * delegates to the unified AliasAnalysisWrapper in lib/Alias/.
 *
 * NOTE: This is a compatibility layer. New code should use AliasAnalysisWrapper
 * directly.
 *
 * Usage:
 *   PDGAliasWrapper wrapper(module, pdg::AAConfig::SparrowAA_NoCtx());
 *   AliasResult result = wrapper.query(v1, v2);
 */

#pragma once

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <memory>

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

namespace pdg {

// Re-export AAConfig from lotus namespace for backward compatibility
using AAConfig = lotus::AAConfig;

/**
 * @class PDGAliasWrapper
 * @brief Backward-compatible wrapper that delegates to AliasAnalysisWrapper
 *
 * This class provides backward compatibility for PDG construction. It simply
 * delegates all calls to the unified AliasAnalysisWrapper.
 *
 * @deprecated Use lotus::AliasAnalysisWrapper directly instead.
 */
class PDGAliasWrapper {
public:
  /**
   * @brief Construct an alias wrapper with specified analysis configuration
   * @param M The LLVM module to analyze
   * @param config The alias analysis configuration to use - MUST be explicitly
   * specified. Use AAConfig factory methods like SparrowAA_NoCtx(), TPA_1CFA(),
   * etc.
   *
   * @note Users MUST explicitly specify which analysis to use. There is no
   * default. This ensures users are aware of what analysis is running.
   *
   * @example
   * ```cpp
   * // Explicit specification required
   * PDGAliasWrapper wrapper(M, pdg::AAConfig::SparrowAA_NoCtx());
   * ```
   */
  PDGAliasWrapper(llvm::Module &M, const AAConfig &config)
      : _wrapper(std::make_unique<lotus::AliasAnalysisWrapper>(M, config)) {}

  /**
   * @brief Destructor
   */
  ~PDGAliasWrapper() = default;

  /**
   * @brief Query alias relationship between two values
   */
  llvm::AliasResult query(const llvm::Value *v1, const llvm::Value *v2) {
    return _wrapper->query(v1, v2);
  }

  /**
   * @brief Query alias relationship with memory locations
   */
  llvm::AliasResult query(const llvm::MemoryLocation &loc1,
                          const llvm::MemoryLocation &loc2) {
    return _wrapper->query(loc1, loc2);
  }

  /**
   * @brief Check if two values may alias
   */
  bool mayAlias(const llvm::Value *v1, const llvm::Value *v2) {
    return _wrapper->mayAlias(v1, v2);
  }

  /**
   * @brief Check if two values must alias
   */
  bool mustAlias(const llvm::Value *v1, const llvm::Value *v2) {
    return _wrapper->mustAlias(v1, v2);
  }

  /**
   * @brief Check if a value may be null
   */
  bool mayNull(const llvm::Value *v) { return _wrapper->mayNull(v); }

  /**
   * @brief Get the points-to set for a pointer value
   */
  bool getPointsToSet(const llvm::Value *ptr,
                      std::vector<const llvm::Value *> &ptsSet) {
    return _wrapper->getPointsToSet(ptr, ptsSet);
  }

  /**
   * @brief Get the alias set for a value
   */
  bool getAliasSet(const llvm::Value *v,
                   std::vector<const llvm::Value *> &aliasSet) {
    return _wrapper->getAliasSet(v, aliasSet);
  }

  /**
   * @brief Get the configuration of alias analysis being used
   */
  const AAConfig &getConfig() const { return _wrapper->getConfig(); }

  /**
   * @brief Check if the wrapper is initialized and ready to use
   */
  bool isInitialized() const { return _wrapper->isInitialized(); }

private:
  /// The underlying alias analysis wrapper
  std::unique_ptr<lotus::AliasAnalysisWrapper> _wrapper;
};

/**
 * @class PDGAliasFactory
 * @brief Factory class for creating PDGAliasWrapper instances
 *
 * @deprecated Use lotus::AliasAnalysisFactory directly instead.
 */
class PDGAliasFactory {
public:
  /**
   * @brief Create an alias wrapper with the specified configuration
   */
  static std::unique_ptr<PDGAliasWrapper> create(llvm::Module &M,
                                                 const AAConfig &config) {
    return std::make_unique<PDGAliasWrapper>(M, config);
  }

  /**
   * @brief Create an alias wrapper with auto-selected analysis
   */
  static std::unique_ptr<PDGAliasWrapper> createAuto(llvm::Module &M) {
    return std::make_unique<PDGAliasWrapper>(M, AAConfig::SparrowAA_NoCtx());
  }

  /**
   * @brief Get a human-readable name for an AAConfig
   */
  static std::string getTypeName(const AAConfig &config) {
    return lotus::AliasAnalysisFactory::getTypeName(config);
  }
};

} // namespace pdg
