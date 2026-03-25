/**
 * @file AliasSpecManager.h
 * @brief Unified library-function specification manager for alias analyses.
 *
 * ## Purpose
 *
 * `AliasSpecManager` is the **central knowledge base** about well-known
 * library functions used by every alias analysis in the Lotus framework
 * (AllocAA, SparrowAA, LotusAA, etc.).  It loads structured specifications
 * from Lotus config files (e.g., `config/ptr.spec`) and exposes a rich,
 * type-safe query API so that individual analyses do not need to hard-code
 * lists of function names.
 *
 * ## What It Provides
 *
 * - **Category classification**: Is a function an allocator? A deallocator?
 *   A memory-copy? A pure/no-effect function?
 * - **Allocator details**: Which argument is the size? Does it zero-initialise?
 *   Does it return the pointer or write it through an out-parameter?
 * - **Copy semantics**: Source/destination argument indices, whether the
 *   return value aliases the destination.
 * - **Return-alias information**: Does the return value alias one of the
 *   arguments (e.g., `strcpy` returns its first argument)?
 * - **Mod/ref information**: Which arguments are read or written?
 * - **Batch queries**: Get all allocator names, all no-effect names, etc.
 *   (used during analysis initialisation to seed name sets).
 *
 * ## Usage
 *
 * ```cpp
 * lotus::alias::AliasSpecManager mgr;
 * mgr.initialize(M);   // optional: enables demangling against module symbols
 *
 * if (mgr.isAllocator(F)) {
 *   auto info = mgr.getAllocatorInfo(F);
 *   // info->sizeArgIndex, info->initializesToZero, ...
 * }
 *
 * for (const auto &name : mgr.getAllocatorNames())
 *   allocatorFunctionNames.insert(name);
 * ```
 *
 * ## Spec Files
 *
 * Specifications are loaded from files in `config/` (default) or from paths
 * supplied to the constructor.  The default spec files are located via
 * `getDefaultSpecFiles()`.  Additional specs can be loaded at runtime with
 * `loadSpecFile()` or added programmatically with `addCustomSpec()`.
 *
 * ## Caching
 *
 * All query results are cached after the first lookup (keyed by function
 * name string).  Caching can be disabled with `setCacheEnabled(false)` and
 * the cache can be invalidated with `clearCache()` after loading new specs.
 */

// Unified Spec Management for Alias Analyses
// Provides high-level query interface for function specifications
// used by various alias analyses (SparrowAA, AllocAA, LotusAA, etc.)

#ifndef LOTUS_ALIAS_SPEC_ALIAS_SPEC_MANAGER_H
#define LOTUS_ALIAS_SPEC_ALIAS_SPEC_MANAGER_H

#include "Annotation/APISpec.h"

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/Optional.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

namespace lotus {
namespace alias {

/**
 * @enum FunctionCategory
 * @brief Coarse-grained classification of library functions relevant to
 *        pointer / alias analysis.
 *
 * A function may belong to multiple categories (see `getCategories`), but
 * `getCategory` returns the single most specific one.
 */
enum class FunctionCategory {
  Unknown,       ///< Not recognised by any spec.
  Allocator,     ///< Returns a freshly-allocated region (malloc, calloc, new).
  Deallocator,   ///< Frees a previously-allocated region (free, delete).
  Reallocator,   ///< Reallocates: frees old, allocates new, copies (realloc).
  MemoryCopy,    ///< Copies bytes between two memory regions (memcpy, memmove).
  MemorySet,     ///< Fills a memory region with a byte value (memset).
  MemoryCompare, ///< Compares two memory regions (memcmp).
  StringOperation, ///< String manipulation (strcpy, strcat, strlen, etc.).
  NoEffect,        ///< Pure function: no pointer-related side effects.
  ExitFunction,    ///< Terminates the program (exit, abort, _exit).
  ReturnArgument, ///< Return value aliases a specific argument (strcpy, fgets).
  IoOperation,    ///< File / network I/O (fread, fwrite, send, recv, etc.).
  MathFunction,   ///< Floating-point math (sqrt, sin, cos, etc.).
};

/**
 * @struct AllocatorInfo
 * @brief Detailed description of a memory-allocator function.
 */
struct AllocatorInfo {
  std::string functionName;
  /// Zero-based index of the size argument, or -1 if unknown/not applicable.
  int sizeArgIndex;
  /// `true` if the function returns the allocated pointer (malloc-style);
  /// `false` if it writes the pointer through an out-parameter
  /// (posix_memalign-style).
  bool returnsPointer;
  /// Index of the out-parameter that receives the pointer, or -1 if not
  /// applicable.
  int ptrOutArgIndex;
  /// `true` if the allocated memory is zero-initialised (calloc-style).
  bool initializesToZero;

  AllocatorInfo()
      : sizeArgIndex(-1), returnsPointer(true), ptrOutArgIndex(-1),
        initializesToZero(false) {}
};

/**
 * @struct CopyInfo
 * @brief Describes the copy semantics of a memory-copy function.
 *
 * Captures which arguments are the source and destination, whether the
 * copy is through a pointer-to-region or directly into the argument, and
 * whether the return value aliases the destination.
 */
struct CopyInfo {
  int dstArgIndex; ///< Zero-based index of the destination argument.
  int srcArgIndex; ///< Zero-based index of the source argument.
  /// `true` if the copy writes to `*dst` (the region pointed to by dst);
  /// `false` if the copy writes to `dst` itself.
  bool dstIsRegion;
  /// `true` if the copy reads from `*src`; `false` if it reads from `src`.
  bool srcIsRegion;
  /// `true` if the return value aliases the destination.
  bool returnsAlias;
  /// Which argument the return value aliases (-1 means it aliases `dst`).
  int retArgIndex;

  CopyInfo()
      : dstArgIndex(-1), srcArgIndex(-1), dstIsRegion(false),
        srcIsRegion(false), returnsAlias(false), retArgIndex(-1) {}
};

/**
 * @struct ReturnAliasInfo
 * @brief Describes how the return value of a function aliases its arguments.
 *
 * For example, `strcpy(dst, src)` returns `dst`, so `argIndex=0`,
 * `isRegion=false`. `fgets(buf, n, stream)` returns `buf` or NULL, so
 * `argIndex=0`, `isNull=true`.
 */
struct ReturnAliasInfo {
  /// Which argument the return value aliases (-1 for a static/global pointer).
  int argIndex;
  /// `true` if the return value aliases `*arg` (the region); `false` for `arg`
  /// itself.
  bool isRegion;
  /// `true` if the function may return a pointer to static/global storage.
  bool isStatic;
  /// `true` if the function may return null.
  bool isNull;

  ReturnAliasInfo()
      : argIndex(-1), isRegion(false), isStatic(false), isNull(false) {}
};

/**
 * @struct ModRefInfo
 * @brief Describes which arguments a function reads or writes.
 *
 * Used to determine whether a function call can be treated as a no-op for
 * alias analysis purposes, or whether it introduces new def-use edges.
 */
struct ModRefInfo {
  /// Zero-based indices of arguments that the function writes through.
  std::vector<int> modifiedArgs;
  /// Zero-based indices of arguments that the function reads through.
  std::vector<int> referencedArgs;
  /// `true` if the function writes to the memory region pointed to by the
  /// return value.
  bool modifiesReturn;
  /// `true` if the function reads from the memory region pointed to by the
  /// return value.
  bool referencesReturn;

  ModRefInfo() : modifiesReturn(false), referencesReturn(false) {}
};

/**
 * @class AliasSpecManager
 * @brief Main interface for querying library-function specifications.
 *
 * Constructed once (typically per analysis pass) and then queried repeatedly.
 * All queries accept either an `llvm::Function *` or a `std::string` function
 * name; the string overloads are useful when the LLVM function object is not
 * available (e.g., during batch initialisation from name lists).
 */
class AliasSpecManager {
public:
  /// Default constructor: loads default spec files from `config/`.
  AliasSpecManager();

  /// Constructor: loads the specified spec files instead of the defaults.
  explicit AliasSpecManager(const std::vector<std::string> &specFilePaths);

  /**
   * @brief Optionally bind to an LLVM module for better name matching.
   *
   * When a module is provided, the manager can demangle C++ names and match
   * them against the module's symbol table, improving lookup accuracy for
   * C++ programs.  This step is optional; queries work without it.
   */
  void initialize(const llvm::Module &M);

  /**
   * @brief Load an additional spec file at runtime.
   * @param path      Path to the spec file.
   * @param errorMsg  Populated with a human-readable error on failure.
   * @return `true` on success; `false` on parse or I/O error.
   */
  bool loadSpecFile(const std::string &path, std::string &errorMsg);

  // =========================================================================
  // Category Queries
  // =========================================================================

  /// @brief Return the primary `FunctionCategory` for @p F.
  FunctionCategory getCategory(const llvm::Function *F) const;
  /// @brief Return the primary `FunctionCategory` for the named function.
  FunctionCategory getCategory(const std::string &functionName) const;

  /// @brief Return all applicable categories for @p F (a function may have
  /// multiple).
  std::set<FunctionCategory> getCategories(const llvm::Function *F) const;
  /// @brief Return all applicable categories for the named function.
  std::set<FunctionCategory>
  getCategories(const std::string &functionName) const;

  // =========================================================================
  // Allocator Queries
  // =========================================================================

  /// @brief Return `true` if @p F is a known memory allocator.
  bool isAllocator(const llvm::Function *F) const;
  /// @brief Return `true` if the named function is a known memory allocator.
  bool isAllocator(const std::string &functionName) const;

  /// @brief Return detailed allocator information for @p F, or `None`.
  llvm::Optional<AllocatorInfo> getAllocatorInfo(const llvm::Function *F) const;
  /// @brief Return detailed allocator information for the named function, or
  /// `None`.
  llvm::Optional<AllocatorInfo>
  getAllocatorInfo(const std::string &functionName) const;

  // =========================================================================
  // Deallocator Queries
  // =========================================================================

  /// @brief Return `true` if @p F is a known memory deallocator.
  bool isDeallocator(const llvm::Function *F) const;
  /// @brief Return `true` if the named function is a known memory deallocator.
  bool isDeallocator(const std::string &functionName) const;

  // =========================================================================
  // No-Effect (Pure) Function Queries
  // =========================================================================

  /**
   * @brief Return `true` if @p F has no pointer-related side effects.
   *
   * A "no-effect" function neither reads nor writes memory through pointer
   * arguments and does not return a pointer to heap/static storage.
   * Typical examples: `abs`, `strlen` (read-only), math functions.
   */
  bool isNoEffect(const llvm::Function *F) const;
  /// @brief Return `true` if the named function has no pointer-related side
  /// effects.
  bool isNoEffect(const std::string &functionName) const;

  // =========================================================================
  // Copy / Memory Operation Queries
  // =========================================================================

  /// @brief Return `true` if @p F copies memory between two regions.
  bool isMemoryCopy(const llvm::Function *F) const;
  /// @brief Return `true` if the named function copies memory between two
  /// regions.
  bool isMemoryCopy(const std::string &functionName) const;

  /// @brief Return all copy effects for @p F (a function may have multiple copy
  /// effects).
  std::vector<CopyInfo> getCopyEffects(const llvm::Function *F) const;
  /// @brief Return all copy effects for the named function.
  std::vector<CopyInfo> getCopyEffects(const std::string &functionName) const;

  // =========================================================================
  // Return Alias Queries
  // =========================================================================

  /// @brief Return `true` if the return value of @p F aliases one of its
  /// arguments.
  bool returnsArgumentAlias(const llvm::Function *F) const;
  /// @brief Return `true` if the return value of the named function aliases an
  /// argument.
  bool returnsArgumentAlias(const std::string &functionName) const;

  /// @brief Return all return-alias descriptions for @p F.
  std::vector<ReturnAliasInfo>
  getReturnAliasInfo(const llvm::Function *F) const;
  /// @brief Return all return-alias descriptions for the named function.
  std::vector<ReturnAliasInfo>
  getReturnAliasInfo(const std::string &functionName) const;

  // =========================================================================
  // Exit Function Queries
  // =========================================================================

  /// @brief Return `true` if @p F terminates the program (exit, abort, etc.).
  bool isExitFunction(const llvm::Function *F) const;
  /// @brief Return `true` if the named function terminates the program.
  bool isExitFunction(const std::string &functionName) const;

  // =========================================================================
  // Mod/Ref Queries
  // =========================================================================

  /// @brief Return mod/ref information for @p F.
  ModRefInfo getModRefInfo(const llvm::Function *F) const;
  /// @brief Return mod/ref information for the named function.
  ModRefInfo getModRefInfo(const std::string &functionName) const;

  /// @brief Return `true` if @p F writes through argument @p argIndex.
  bool modifiesArg(const llvm::Function *F, int argIndex) const;
  /// @brief Return `true` if @p F reads through argument @p argIndex.
  bool referencesArg(const llvm::Function *F, int argIndex) const;

  // =========================================================================
  // Batch Queries (for Analysis Initialisation)
  // =========================================================================

  /// @brief Return all known function names in the given category.
  std::vector<std::string> getFunctionsByCategory(FunctionCategory cat) const;

  /// @brief Return all known allocator function names (e.g., "malloc",
  /// "calloc").
  std::vector<std::string> getAllocatorNames() const;

  /// @brief Return all known deallocator function names (e.g., "free",
  /// "delete").
  std::vector<std::string> getDeallocatorNames() const;

  /// @brief Return all known no-effect function names.
  std::vector<std::string> getNoEffectNames() const;

  // =========================================================================
  // Configuration
  // =========================================================================

  /// @brief Enable or disable result caching (default: enabled).
  void setCacheEnabled(bool enabled);

  /// @brief Invalidate all caches (call after loading new spec files).
  void clearCache();

  /// @brief Add a custom specification programmatically.
  void addCustomSpec(const std::string &functionName, const FunctionSpec &spec);

  /// @brief Return the underlying `APISpec` for advanced / low-level queries.
  const APISpec &getAPISpec() const { return apiSpec_; }
  /// @brief Return the list of spec files that have been loaded.
  const std::vector<std::string> &getLoadedSpecFiles() const {
    return loadedSpecFiles_;
  }

  // =========================================================================
  // Debugging / Statistics
  // =========================================================================

  /// @brief Print all loaded specifications to @p OS.
  void printAllSpecs(llvm::raw_ostream &OS) const;

  /// @brief Aggregate counts of loaded specifications by category.
  struct Statistics {
    size_t totalFunctions;    ///< Total number of named functions in all specs.
    size_t allocators;        ///< Number of allocator functions.
    size_t deallocators;      ///< Number of deallocator functions.
    size_t noEffectFunctions; ///< Number of no-effect (pure) functions.
    size_t copyFunctions;     ///< Number of memory-copy functions.
    size_t exitFunctions;     ///< Number of exit/abort functions.
  };
  /// @brief Return aggregate statistics about the loaded specifications.
  Statistics getStatistics() const;

private:
  APISpec apiSpec_;            ///< Underlying raw spec data.
  const llvm::Module *module_; ///< Optional module for name matching.
  bool cacheEnabled_;          ///< Whether query caching is active.
  std::vector<std::string>
      loadedSpecFiles_; ///< Paths of all loaded spec files.

  // Per-query result caches (keyed by canonical function name string).
  mutable std::unordered_map<std::string, FunctionCategory> categoryCache_;
  mutable std::unordered_map<std::string, std::set<FunctionCategory>>
      categoriesCache_;
  mutable std::unordered_map<std::string, llvm::Optional<AllocatorInfo>>
      allocatorCache_;
  mutable std::unordered_map<std::string, std::vector<CopyInfo>> copyCache_;
  mutable std::unordered_map<std::string, std::vector<ReturnAliasInfo>>
      returnAliasCache_;
  mutable std::unordered_map<std::string, ModRefInfo> modRefCache_;

  /// Pre-computed lists of function names per category (built lazily).
  mutable std::unordered_map<FunctionCategory, std::vector<std::string>>
      categoryLists_;
  mutable bool categoryListsBuilt_;

  // Internal helpers
  std::string normalizeFunctionName(const llvm::Function *F) const;
  std::string demangle(const std::string &mangledName) const;
  std::string canonicalizeName(const std::string &name) const;
  const FunctionSpec *lookupSpec(const std::string &functionName) const;
  FunctionCategory categorizeIntrinsic(const llvm::Function *F) const;
  bool isKnownDeallocator(const std::string &functionName) const;

  FunctionCategory categorizeFunctionSpec(const FunctionSpec &spec) const;
  std::set<FunctionCategory>
  categorizeFunctionSpecMulti(const FunctionSpec &spec) const;

  AllocatorInfo buildAllocatorInfo(const std::string &name,
                                   const FunctionSpec &spec) const;
  std::vector<CopyInfo> buildCopyInfo(const FunctionSpec &spec) const;
  std::vector<CopyInfo> buildIntrinsicCopyInfo(const llvm::Function *F) const;
  std::vector<ReturnAliasInfo>
  buildReturnAliasInfo(const FunctionSpec &spec) const;
  ModRefInfo buildModRefInfo(const FunctionSpec &spec) const;
  ModRefInfo buildIntrinsicModRefInfo(const llvm::Function *F) const;

  void buildCategoryLists() const;
};

// =========================================================================
// Utility Functions
// =========================================================================

/// @brief Return the default spec file paths (from `LOTUS_CONFIG_DIR` or
///        relative to the binary).
std::vector<std::string> getDefaultSpecFiles();

/// @brief Return the full path to a named spec file.
std::string getSpecFilePath(const std::string &specFileName);

/// @brief Convert a `FunctionCategory` to a human-readable string.
const char *categoryToString(FunctionCategory cat);

/// @brief Parse a `FunctionCategory` from a string, or return `None`.
llvm::Optional<FunctionCategory> stringToCategory(const std::string &str);

} // namespace alias
} // namespace lotus

#endif // LOTUS_ALIAS_COMMON_ALIAS_SPEC_MANAGER_H
