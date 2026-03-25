/**
 * @file AliasSpecManager.cpp
 * @brief Core implementation of AliasSpecManager: initialization, name
 * normalization, and spec lookup
 *
 * This file implements the main AliasSpecManager class, which provides a
 * unified interface for querying function specifications used by various alias
 * analyses. It handles:
 * - Loading specification files from the filesystem
 * - Name normalization and demangling (for C++ symbols)
 * - Specification lookup with fallback strategies
 * - Cache management for performance
 * - Statistics and debugging utilities
 */

#include "Alias/Spec/AliasSpecManager.h"

#include "Utils/LLVM/Demangle.h"

#include <cctype>

#include <llvm/Support/raw_ostream.h>

using namespace lotus;
using namespace lotus::alias;

// ===== AliasSpecManager Implementation =====

/**
 * @brief Default constructor: loads default specification files.
 *
 * Automatically loads ptr.spec and modref.spec from default locations:
 * - LOTUS_CONFIG_DIR environment variable (if set)
 * - config/ in current or parent directory
 *
 * Warnings are printed for files that fail to load, but the constructor
 * does not fail if no files are found (allows for programmatic spec addition).
 */
AliasSpecManager::AliasSpecManager()
    : module_(nullptr), cacheEnabled_(true), categoryListsBuilt_(false) {
  std::string err;
  for (const auto &path : getDefaultSpecFiles()) {
    if (apiSpec_.loadFile(path, err)) {
      loadedSpecFiles_.push_back(path);
    } else if (!err.empty()) {
      llvm::errs() << "[AliasSpecManager] Warning: " << err << "\n";
    }
  }
}

/**
 * @brief Constructor: loads specified specification files.
 *
 * Allows explicit control over which specification files to load.
 * Useful for testing or when using non-standard file locations.
 *
 * @param specFilePaths Vector of full paths to specification files to load
 */
AliasSpecManager::AliasSpecManager(
    const std::vector<std::string> &specFilePaths)
    : module_(nullptr), cacheEnabled_(true), categoryListsBuilt_(false) {
  std::string err;
  for (const auto &path : specFilePaths) {
    if (apiSpec_.loadFile(path, err)) {
      loadedSpecFiles_.push_back(path);
    } else if (!err.empty()) {
      llvm::errs() << "[AliasSpecManager] Warning: " << err << "\n";
    }
  }
}

/**
 * @brief Initialize the manager with an LLVM Module.
 *
 * Associates the manager with a specific LLVM module. This enables better
 * function name matching and context-aware queries. Clears all caches
 * to ensure consistency with the new module.
 *
 * @param M The LLVM module to associate with this manager
 */
void AliasSpecManager::initialize(const llvm::Module &M) {
  module_ = &M;
  clearCache();
}

/**
 * @brief Load an additional specification file.
 *
 * Loads a specification file and adds it to the manager's knowledge base.
 * Automatically clears caches to ensure queries reflect the new specifications.
 *
 * @param path Full path to the specification file to load
 * @param errorMsg Output parameter: error message if loading fails
 * @return true if the file was loaded successfully, false otherwise
 */
bool AliasSpecManager::loadSpecFile(const std::string &path,
                                    std::string &errorMsg) {
  bool result = apiSpec_.loadFile(path, errorMsg);
  if (result) {
    loadedSpecFiles_.push_back(path);
    clearCache();
  } else if (!errorMsg.empty()) {
    llvm::errs() << "[AliasSpecManager] Failed to load spec file " << path
                 << ": " << errorMsg << "\n";
  }
  return result;
}

/**
 * @brief Normalize an LLVM Function to its string name.
 *
 * Extracts the function name from an LLVM Function object. This is the
 * primary method for converting LLVM IR functions to string identifiers
 * for specification lookup.
 *
 * @param F Pointer to the LLVM Function (may be null)
 * @return Function name as string, or empty string if F is null
 */
std::string
AliasSpecManager::normalizeFunctionName(const llvm::Function *F) const {
  if (!F)
    return "";
  return F->getName().str();
}

/**
 * @brief Demangle a C++ mangled function name.
 *
 * Attempts to demangle C++ symbol names (e.g., "_Z3fooi" -> "foo(int)").
 * Only processes names that start with '_' (typical mangled symbol prefix).
 * Returns the original name if demangling fails or is not applicable.
 *
 * @param mangledName Potentially mangled function name
 * @return Demangled name if successful, original name otherwise
 */
std::string AliasSpecManager::demangle(const std::string &mangledName) const {
  if (mangledName.empty() || mangledName[0] != '_')
    return mangledName;

  std::string demangled = DemangleUtils::demangle(mangledName);
  return demangled.empty() ? mangledName : demangled;
}

/**
 * @brief Canonicalize a function name for specification lookup.
 *
 * Normalizes function names by:
 * - Removing leading underscores (common in C symbol names)
 * - Stripping function signatures from demangled C++ names (e.g., "foo(int)" ->
 * "foo")
 * - Removing trailing whitespace
 *
 * This helps match function names in specifications that may use different
 * naming conventions than the actual symbol names.
 *
 * @param name Function name to canonicalize
 * @return Canonicalized function name
 */
std::string AliasSpecManager::canonicalizeName(const std::string &name) const {
  if (name.empty())
    return name;

  std::string result = name;
  if (!result.empty() && result[0] == '_')
    result = result.substr(1);

  // Strip arguments from demangled names (e.g., "foo(int)" -> "foo")
  auto parenPos = result.find('(');
  if (parenPos != std::string::npos)
    result = result.substr(0, parenPos);

  // Remove trailing spaces that may appear in demangled names
  while (!result.empty() &&
         std::isspace(static_cast<unsigned char>(result.back())))
    result.pop_back();

  return result;
}

/**
 * @brief Look up a function specification by name.
 *
 * Performs a multi-strategy lookup to find a function specification:
 * 1. Direct lookup using the exact function name
 * 2. Lookup using canonicalized name (stripped underscore, signature)
 * 3. Lookup using demangled and canonicalized name (for C++ symbols)
 *
 * This fallback strategy ensures specifications can be found even when
 * symbol names differ slightly from specification entries.
 *
 * @param functionName Function name to look up
 * @return Pointer to FunctionSpec if found, nullptr otherwise
 */
const FunctionSpec *
AliasSpecManager::lookupSpec(const std::string &functionName) const {
  if (functionName.empty())
    return nullptr;

  if (const FunctionSpec *spec = apiSpec_.get(functionName))
    return spec;

  // Try canonicalized version (strip leading underscore and signature)
  std::string canonical = canonicalizeName(functionName);
  if (canonical != functionName) {
    if (const FunctionSpec *spec = apiSpec_.get(canonical))
      return spec;
  }

  // Try demangled name (for C++ mangled symbols)
  std::string demangled = demangle(functionName);
  if (demangled != functionName) {
    demangled = canonicalizeName(demangled);
    if (const FunctionSpec *spec = apiSpec_.get(demangled))
      return spec;
  }

  return nullptr;
}

/**
 * @brief Check if a function is a known deallocator.
 *
 * Internal helper method that checks if a function has a specification
 * marking it as a deallocator (e.g., free, delete).
 *
 * @param functionName Function name to check
 * @return true if the function is a known deallocator, false otherwise
 */
bool AliasSpecManager::isKnownDeallocator(
    const std::string &functionName) const {
  const FunctionSpec *spec = lookupSpec(functionName);
  return spec && spec->isDeallocator;
}

// ===== Configuration =====

/**
 * @brief Enable or disable result caching.
 *
 * When caching is enabled (default), query results are stored to improve
 * performance for repeated queries. Disable caching if memory usage is
 * a concern or if specifications are frequently modified.
 *
 * @param enabled true to enable caching, false to disable
 */
void AliasSpecManager::setCacheEnabled(bool enabled) {
  cacheEnabled_ = enabled;
  if (!enabled)
    clearCache();
}

/**
 * @brief Clear all internal caches.
 *
 * Invalidates all cached query results. Should be called after:
 * - Loading new specification files
 * - Modifying specifications programmatically
 * - Changing module context
 *
 * Also resets the category lists built state.
 */
void AliasSpecManager::clearCache() {
  categoryCache_.clear();
  categoriesCache_.clear();
  allocatorCache_.clear();
  copyCache_.clear();
  returnAliasCache_.clear();
  modRefCache_.clear();
  categoryListsBuilt_ = false;
  categoryLists_.clear();
}

/**
 * @brief Add or replace a function specification programmatically.
 *
 * Allows adding custom specifications at runtime without loading from files.
 * Useful for:
 * - Testing with synthetic specifications
 * - Adding project-specific function annotations
 * - Overriding default specifications
 *
 * Automatically clears caches to ensure queries reflect the new specification.
 *
 * @param functionName Name of the function to specify
 * @param spec FunctionSpec object containing the specification
 */
void AliasSpecManager::addCustomSpec(const std::string &functionName,
                                     const FunctionSpec &spec) {
  FunctionSpec copy = spec;
  if (copy.functionName.empty())
    copy.functionName = functionName;
  apiSpec_.addOrReplaceSpec(copy);
  clearCache();
}

// ===== Debugging =====

/**
 * @brief Print all loaded function specifications to an output stream.
 *
 * Useful for debugging and understanding what specifications are available.
 * Prints a summary of each function's properties:
 * - IGNORE: No-effect functions
 * - EXIT: Exit functions
 * - ALLOC: Allocator functions
 * - COPY(n): Functions with n copy operations
 * - MODREF(n): Functions with n mod/ref annotations
 *
 * @param OS Output stream to write to (e.g., llvm::errs(), llvm::outs())
 */
void AliasSpecManager::printAllSpecs(llvm::raw_ostream &OS) const {
  for (const auto &entry : apiSpec_.all()) {
    OS << entry.first << ": ";
    const FunctionSpec &spec = entry.second;

    if (spec.isIgnored)
      OS << "IGNORE ";
    if (spec.isExit)
      OS << "EXIT ";
    if (spec.isAllocator)
      OS << "ALLOC ";
    if (!spec.copies.empty())
      OS << "COPY(" << spec.copies.size() << ") ";
    if (!spec.modref.empty())
      OS << "MODREF(" << spec.modref.size() << ") ";

    OS << "\n";
  }
}

/**
 * @brief Get statistics about loaded specifications.
 *
 * Computes aggregate statistics about the loaded function specifications,
 * including counts of different function categories. Useful for:
 * - Understanding specification coverage
 * - Debugging missing specifications
 * - Performance analysis
 *
 * @return Statistics struct containing counts of various function types
 */
AliasSpecManager::Statistics AliasSpecManager::getStatistics() const {
  Statistics stats;
  stats.totalFunctions = apiSpec_.all().size();
  stats.allocators = 0;
  stats.deallocators = 0;
  stats.noEffectFunctions = 0;
  stats.copyFunctions = 0;
  stats.exitFunctions = 0;

  for (const auto &entry : apiSpec_.all()) {
    const FunctionSpec &spec = entry.second;
    if (spec.isAllocator)
      stats.allocators++;
    if (spec.isIgnored)
      stats.noEffectFunctions++;
    if (!spec.copies.empty())
      stats.copyFunctions++;
    if (spec.isExit)
      stats.exitFunctions++;
  }

  stats.deallocators = getDeallocatorNames().size();

  return stats;
}
