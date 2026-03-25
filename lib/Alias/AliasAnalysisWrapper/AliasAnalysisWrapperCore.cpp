/**
 * @file AliasAnalysisWrapperCore.cpp
 * @brief Core implementation: constructors, destructor, and initialization
 * 
 * This file handles the lifecycle and initialization of AliasAnalysisWrapper.
 * It contains:
 * - Constructors and destructor
 * - The initialize() method that sets up different alias analysis backends
 * - Helper function for combining alias results from multiple backends
 */

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Alias/AllocAA/AllocAA.h"
#include "Alias/DDA/FlowDDA.h"
#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/SparrowAA/AndersenAA.h"
#include "Alias/TPA/Context/ContextPolicy.h"
#include "Alias/TPA/Context/KLimitContext.h"
#include "Alias/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/SemiSparseProgramBuilder.h"
#include "Alias/TPA/PointerAnalysis/Support/PtsSet.h"
#include "Alias/TPA/Transforms/RunPrepass.h"
#include "Alias/UnderApproxAA/UnderApproxAA.h"
#include "Alias/seadsa/SeaDsaAliasAnalysis.hh"
#include <llvm/Analysis/CFLAndersAliasAnalysis.h>
#include <llvm/Analysis/CFLSteensAliasAnalysis.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <sstream>

using namespace llvm;
using namespace lotus;

namespace {
/**
 * @brief Combine alias results from multiple sound alias analysis backends
 * 
 * This function implements a conservative merging strategy for combining results
 * from multiple alias analysis backends. The strategy prioritizes precision:
 * - If any backend can prove NoAlias, the result is NoAlias (most precise)
 * - If any backend can prove MustAlias (and none prove NoAlias), the result is MustAlias
 * - Otherwise, PartialAlias is preferred over MayAlias
 * 
 * @param Results Array of alias results from different backends
 * @return Combined alias result following the conservative merging strategy
 * @note If contradictory results are found (NoAlias and MustAlias), falls back
 *       to MayAlias as a safe default. This should not happen with sound analyses.
 */
static llvm::AliasResult combineAliasResults(llvm::ArrayRef<llvm::AliasResult> Results) {
  bool SawNo = false, SawMust = false, SawPartial = false;
  for (auto R : Results) {
    if (R == llvm::AliasResult::NoAlias) SawNo = true;
    else if (R == llvm::AliasResult::MustAlias) SawMust = true;
    else if (R == llvm::AliasResult::PartialAlias) SawPartial = true;
  }

  // Contradiction (shouldn't happen with sound analyses): fall back to MayAlias.
  if (SawNo && SawMust) return llvm::AliasResult::MayAlias;
  if (SawNo) return llvm::AliasResult::NoAlias;
  if (SawMust) return llvm::AliasResult::MustAlias;
  if (SawPartial) return llvm::AliasResult::PartialAlias;
  return llvm::AliasResult::MayAlias;
}
} // namespace

/**
 * @brief Construct an AliasAnalysisWrapper with the specified configuration
 * 
 * Creates a new alias analysis wrapper for the given LLVM module using the
 * specified configuration. The wrapper will automatically initialize the
 * appropriate alias analysis backend based on the configuration.
 * 
 * @param M The LLVM module to analyze
 * @param config The alias analysis configuration specifying which implementation
 *               to use, context sensitivity settings, and other parameters
 * 
 * @note Initialization happens automatically in the constructor. If initialization
 *       fails, the wrapper will be marked as uninitialized and queries will return
 *       conservative (MayAlias) results.
 */
AliasAnalysisWrapper::AliasAnalysisWrapper(Module &M, const AAConfig &config)
    : _config(config), _module(&M), _initialized(false),
      _llvm_aa(nullptr), _seadsa_aa(nullptr), _sraa(nullptr) {
  initialize();
}

/**
 * @brief Destructor for AliasAnalysisWrapper
 * 
 * Cleans up all allocated alias analysis backends. All unique_ptr members
 * will automatically deallocate their resources.
 */
AliasAnalysisWrapper::~AliasAnalysisWrapper() = default;

/**
 * @brief Initialize the alias analysis backend based on the configuration
 * 
 * This method sets up the appropriate alias analysis backend according to
 * the configuration specified in _config. It handles:
 * - SparrowAA (Andersen-style) with configurable k-CFA levels
 * - AserPTA (currently rejected until wrapper integration is implemented)
 * - TPA (Flow- and context-sensitive semi-sparse analysis)
 * - DyckAA, CFLAnders, CFLSteens, UnderApprox
 * - Combined mode (multiple backends)
 * 
 * The initialization process:
 * 1. Determines which backend to use from _config.impl
 * 2. Sets up context sensitivity parameters (k-limit for k-CFA)
 * 3. Creates and initializes the backend-specific analysis object
 * 4. For TPA: runs IR normalization, builds semi-sparse program, loads
 *    external pointer table (if available), and runs the analysis
 * 
 * @note If initialization fails, _initialized remains false and queries
 *       will return conservative results. Errors are logged to stderr.
 * @note TPA initialization includes running prepasses and building the
 *       semi-sparse program representation, which may be time-consuming.
 * @note The external pointer table for TPA is loaded from:
 *       - $LOTUS_CONFIG_DIR/ptr.spec (if environment variable is set)
 *       - config/ptr.spec (fallback)
 *       If the file doesn't exist, analysis continues without it.
 */
void AliasAnalysisWrapper::initialize() {
  if (!_module) return;

  auto initAA = [this](auto fn, const char *name) -> bool {
    try {
      fn();
      return true;
    } catch (const std::exception &e) {
      errs() << "AliasAnalysisWrapper: Failed to init " << name << ": " << e.what() << "\n";
      return false;
    }
  };

  switch (_config.impl) {
  case AAConfig::Implementation::SparrowAA: {
    unsigned k = (_config.ctxSens == AAConfig::ContextSensitivity::KCallSite) ? _config.kLimit : 0;
    _initialized = initAA([this, k]{
      _andersen_aa = std::make_unique<AndersenAAResult>(*_module, makeContextPolicy(k));
    }, _config.getName().c_str());
    break;
  }
  
  case AAConfig::Implementation::AserPTA: {
    errs() << "AliasAnalysisWrapper: AserPTA config requested, but the "
              "wrapper backend is not integrated yet; refusing to fall back "
              "to a different analysis\n";
    _initialized = false;
    break;
  }
  
  case AAConfig::Implementation::TPA: {
    _initialized = initAA([this]{
      // Set context strategy and k-limit for TPA
      if (_config.ctxSens == AAConfig::ContextSensitivity::Adaptive) {
        context::setContextStrategy(context::ContextStrategy::Selective);
        context::KLimitContext::setLimit(_config.kLimit);
      } else if (_config.ctxSens == AAConfig::ContextSensitivity::KCallSite) {
        context::setContextStrategy(context::ContextStrategy::KLimit);
        context::KLimitContext::setLimit(_config.kLimit);
      } else {
        context::setContextStrategy(context::ContextStrategy::KLimit);
        context::KLimitContext::setLimit(0); // Context-insensitive
      }
      
      // Run TPA IR normalization prepasses
      transform::runPrepassOn(*_module);
      // Build semi-sparse program representation
      tpa::SemiSparseProgramBuilder builder;
      _tpa_program = std::make_unique<tpa::SemiSparseProgram>(builder.runOnModule(*_module));
      // Create and run the pointer analysis
      _tpa_aa = std::make_unique<tpa::SemiSparsePointerAnalysis>();
      // Try to load external pointer table (optional, won't fail if not found)
      // Check LOTUS_CONFIG_DIR first, then fallback to config/ptr.spec
      std::string specPath;
      const char *envPath = std::getenv("LOTUS_CONFIG_DIR");
      if (envPath) {
        specPath = std::string(envPath) + "/ptr.spec";
      } else {
        specPath = "config/ptr.spec";
      }
      // Use a simple file existence check - if file doesn't exist, analysis will still work
      std::error_code EC;
      llvm::sys::fs::file_status Status;
      EC = llvm::sys::fs::status(specPath, Status);
      if (!EC && llvm::sys::fs::exists(Status)) {
        _tpa_aa->loadExternalPointerTable(specPath.c_str());
      }
      // Run the analysis
      _tpa_aa->runOnProgram(*_tpa_program);
    }, _config.getName().c_str());
    break;
  }

  case AAConfig::Implementation::DDA:
    _initialized = initAA([this]{
      _dda_aa = std::make_unique<lotus::analysis::DemandDrivenAA>();
      _dda_aa->run(*_module);
    }, _config.getName().c_str());
    break;
  
  case AAConfig::Implementation::DyckAA:
    _initialized = initAA([this]{ 
      _dyck_aa = std::make_unique<DyckAliasAnalysis>(); 
      _dyck_aa->runOnModule(*_module); 
    }, "DyckAA");
    break;
  
  case AAConfig::Implementation::UnderApprox:
    _initialized = initAA([this]{ 
      _underapprox_aa = std::make_unique<UnderApprox::UnderApproxAA>(*_module); 
    }, "UnderApprox");
    break;
  
  case AAConfig::Implementation::CFLAnders:
    _initialized = initAA([this]{
      _cflanders_pass.reset(static_cast<CFLAndersAAWrapperPass *>(createCFLAndersAAWrapperPass()));
      legacy::PassManager PM;
      PM.add(_cflanders_pass.get());
      PM.run(*_module);
    }, "CFLAnders");
    break;
  
  case AAConfig::Implementation::CFLSteens:
    _initialized = initAA([this]{
      _cflsteens_pass.reset(static_cast<CFLSteensAAWrapperPass *>(createCFLSteensAAWrapperPass()));
      legacy::PassManager PM;
      PM.add(_cflsteens_pass.get());
      PM.run(*_module);
    }, "CFLSteens");
    break;
  
  case AAConfig::Implementation::Combined: {
    // Truly "combined": initialize multiple backends and merge their answers.
    // Mark as initialized if at least one backend succeeds.
    bool andersenInitialized = initAA([this]{
      _andersen_aa = std::make_unique<AndersenAAResult>(*_module, makeContextPolicy(0));
    }, "Andersen(NoCtx)");
    bool dyckInitialized = initAA([this]{
      _dyck_aa = std::make_unique<DyckAliasAnalysis>();
      _dyck_aa->runOnModule(*_module);
    }, "DyckAA");
    // At least one backend must succeed for Combined mode to work
    _initialized = andersenInitialized || dyckInitialized;
    break;
  }
  
  case AAConfig::Implementation::SeaDsa:
  case AAConfig::Implementation::AllocAA:
  case AAConfig::Implementation::BasicAA:
  case AAConfig::Implementation::TBAA:
  case AAConfig::Implementation::GlobalsAA:
  case AAConfig::Implementation::SCEVAA:
  case AAConfig::Implementation::SRAA:
    // These are not yet fully integrated in the wrapper
    errs() << "AliasAnalysisWrapper: " << _config.getName() 
           << " is not yet fully supported\n";
    break;
  
  default:
    errs() << "AliasAnalysisWrapper: Unknown implementation type\n";
    break;
  }
}

/**
 * @brief Get a human-readable name for this alias analysis configuration
 * 
 * Generates a descriptive string representation of the configuration that
 * includes:
 * - The implementation name (SparrowAA, AserPTA, TPA, etc.)
 * - Context sensitivity information (NoCtx, k-CFA level, Origin)
 * - Solver information for AserPTA (if specified)
 * 
 * Examples:
 * - "SparrowAA(NoCtx)"
 * - "SparrowAA(1-CFA)"
 * - "AserPTA(2-CFA)[Deep]"
 * - "TPA(NoCtx)"
 * - "DyckAA"
 * 
 * @return A string describing the configuration in a human-readable format
 */
std::string AAConfig::getName() const {
  std::ostringstream oss;
  
  switch (impl) {
  case Implementation::SparrowAA:
    oss << "SparrowAA";
    if (ctxSens == ContextSensitivity::KCallSite && kLimit > 0) {
      oss << "(" << kLimit << "-CFA)";
    } else {
      oss << "(NoCtx)";
    }
    break;
    
  case Implementation::AserPTA:
    oss << "AserPTA";
    if (ctxSens == ContextSensitivity::KCallSite && kLimit > 0) {
      oss << "(" << kLimit << "-CFA)";
    } else if (ctxSens == ContextSensitivity::KOrigin) {
      oss << "(Origin)";
    } else {
      oss << "(NoCtx)";
    }
    if (solver != Solver::Default) {
      oss << "[";
      switch (solver) {
      case Solver::Wave: oss << "Wave"; break;
      case Solver::Deep: oss << "Deep"; break;
      case Solver::Basic: oss << "Basic"; break;
      default: break;
      }
      oss << "]";
    }
    break;
    
  case Implementation::TPA:
    oss << "TPA";
    if (ctxSens == ContextSensitivity::KCallSite && kLimit > 0) {
      oss << "(" << kLimit << "-CFA)";
    } else {
      oss << "(NoCtx)";
    }
    break;

  case Implementation::DDA:
    oss << "DDA(NoCtx)";
    break;
    
  case Implementation::DyckAA:
    oss << "DyckAA";
    break;
    
  case Implementation::CFLAnders:
    oss << "CFLAnders";
    break;
    
  case Implementation::CFLSteens:
    oss << "CFLSteens";
    break;
    
  case Implementation::SeaDsa:
    oss << "SeaDsa";
    break;
    
  case Implementation::AllocAA:
    oss << "AllocAA";
    break;
    
  case Implementation::UnderApprox:
    oss << "UnderApprox";
    break;
    
  case Implementation::Combined:
    oss << "Combined";
    break;
    
  case Implementation::BasicAA:
    oss << "BasicAA";
    break;
    
  case Implementation::TBAA:
    oss << "TBAA";
    break;
    
  case Implementation::GlobalsAA:
    oss << "GlobalsAA";
    break;
    
  case Implementation::SCEVAA:
    oss << "SCEVAA";
    break;
    
  case Implementation::SRAA:
    oss << "SRAA";
    break;
  }
  
  return oss.str();
}
