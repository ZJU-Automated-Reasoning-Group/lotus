/**
 * @file AliasSpecManagerQueries.cpp
 * @brief Query implementations for AliasSpecManager: category, allocator, copy,
 * mod/ref queries
 *
 * This file implements all the query methods of AliasSpecManager, organized by
 * query type:
 * - Category queries: Determine function categories (Allocator, Deallocator,
 * etc.)
 * - Allocator queries: Identify and analyze memory allocation functions
 * - Deallocator queries: Identify memory deallocation functions
 * - No-effect queries: Identify pure functions with no pointer side effects
 * - Copy operation queries: Analyze memory copy operations
 * - Return alias queries: Determine if return values alias arguments
 * - Exit function queries: Identify program termination functions
 * - Mod/Ref queries: Determine which arguments are modified or referenced
 * - Batch queries: Efficient bulk queries for analysis initialization
 *
 * All query methods support both LLVM Function* and string-based lookups, with
 * caching for performance when enabled.
 */

#include "Alias/Spec/AliasSpecManager.h"

#include <algorithm>

#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/raw_ostream.h>

using namespace lotus;
using namespace lotus::alias;

// ===== Category Queries =====

/**
 * @brief Categorize an LLVM intrinsic function.
 *
 * Maps well-known LLVM intrinsics to their function categories.
 * Intrinsics are handled separately from regular functions because they
 * don't appear in specification files but have well-defined semantics.
 *
 * @param F Pointer to LLVM Function (must be an intrinsic)
 * @return FunctionCategory for the intrinsic, or Unknown if not recognized
 */
FunctionCategory
AliasSpecManager::categorizeIntrinsic(const llvm::Function *F) const {
  if (!F || !F->isIntrinsic())
    return FunctionCategory::Unknown;

  using namespace llvm;
  switch (F->getIntrinsicID()) {
  case Intrinsic::memcpy:
  case Intrinsic::memmove:
    return FunctionCategory::MemoryCopy;
  case Intrinsic::memset:
    return FunctionCategory::MemorySet;
  case Intrinsic::expect:
  case Intrinsic::assume:
    return FunctionCategory::NoEffect;
  default:
    return FunctionCategory::Unknown;
  }
}

/**
 * @brief Categorize a function based on its specification.
 *
 * Determines the primary category for a function by examining its specification
 * properties. Uses a priority order to select the most relevant category when
 * a function could belong to multiple categories.
 *
 * Priority order:
 * 1. ExitFunction (highest priority)
 * 2. Deallocator
 * 3. Allocator/Reallocator (Reallocator if also has copy operations)
 * 4. NoEffect
 * 5. MemoryCopy (if copies between regions)
 * 6. ReturnArgument (if return aliases an argument)
 * 7. StringOperation (if has copy operations but not region-to-region)
 * 8. IoOperation (if has mod/ref but no copies)
 * 9. Unknown (lowest priority)
 *
 * @param spec FunctionSpec to categorize
 * @return Primary FunctionCategory for this specification
 */
FunctionCategory
AliasSpecManager::categorizeFunctionSpec(const FunctionSpec &spec) const {
  // Priority order for categorization
  if (spec.isExit)
    return FunctionCategory::ExitFunction;
  if (spec.isDeallocator)
    return FunctionCategory::Deallocator;
  if (spec.isAllocator) {
    // Check if it's also a copy (like realloc)
    if (!spec.copies.empty())
      return FunctionCategory::Reallocator;
    return FunctionCategory::Allocator;
  }
  if (spec.isIgnored)
    return FunctionCategory::NoEffect;

  if (!spec.copies.empty()) {
    // Determine if it's a memory copy or return argument
    for (const auto &copy : spec.copies) {
      if (copy.dstQualifier == QualifierKind::Region &&
          copy.srcQualifier == QualifierKind::Region)
        return FunctionCategory::MemoryCopy;
      if (copy.dst.kind == SelectorKind::Ret)
        return FunctionCategory::ReturnArgument;
    }
    return FunctionCategory::StringOperation;
  }

  if (!spec.modref.empty())
    return FunctionCategory::IoOperation;

  return FunctionCategory::Unknown;
}

/**
 * @brief Get all applicable categories for a function specification.
 *
 * Unlike categorizeFunctionSpec(), this method returns ALL categories that
 * apply to a function, not just the primary one. A function can belong to
 * multiple categories simultaneously (e.g., Reallocator is both Allocator
 * and MemoryCopy).
 *
 * @param spec FunctionSpec to categorize
 * @return Set of all FunctionCategory values that apply to this specification
 */
std::set<FunctionCategory>
AliasSpecManager::categorizeFunctionSpecMulti(const FunctionSpec &spec) const {
  std::set<FunctionCategory> cats;

  if (spec.isExit)
    cats.insert(FunctionCategory::ExitFunction);
  if (spec.isDeallocator)
    cats.insert(FunctionCategory::Deallocator);
  if (spec.isAllocator) {
    if (!spec.copies.empty())
      cats.insert(FunctionCategory::Reallocator);
    else
      cats.insert(FunctionCategory::Allocator);
  }
  if (spec.isIgnored)
    cats.insert(FunctionCategory::NoEffect);

  for (const auto &copy : spec.copies) {
    if (copy.dstQualifier == QualifierKind::Region &&
        copy.srcQualifier == QualifierKind::Region)
      cats.insert(FunctionCategory::MemoryCopy);
    if (copy.dst.kind == SelectorKind::Ret)
      cats.insert(FunctionCategory::ReturnArgument);
  }

  if (!spec.modref.empty())
    cats.insert(FunctionCategory::IoOperation);

  return cats;
}

/**
 * @brief Get the primary category for an LLVM Function.
 *
 * Looks up the function's specification and determines its primary category.
 * Falls back to intrinsic categorization if no specification is found.
 * Results are cached when caching is enabled.
 *
 * @param F Pointer to LLVM Function
 * @return Primary FunctionCategory, or Unknown if not found/recognized
 */
FunctionCategory AliasSpecManager::getCategory(const llvm::Function *F) const {
  if (!F)
    return FunctionCategory::Unknown;

  const std::string name = normalizeFunctionName(F);
  FunctionCategory cat = getCategory(name);
  if (cat == FunctionCategory::Unknown) {
    cat = categorizeIntrinsic(F);
    if (cacheEnabled_)
      categoryCache_[name] = cat;
  }
  return cat;
}

/**
 * @brief Get the primary category for a function by name.
 *
 * Looks up the function's specification by name and determines its primary
 * category. Results are cached when caching is enabled.
 *
 * @param functionName Function name to look up
 * @return Primary FunctionCategory, or Unknown if not found
 */
FunctionCategory
AliasSpecManager::getCategory(const std::string &functionName) const {
  if (cacheEnabled_) {
    auto it = categoryCache_.find(functionName);
    if (it != categoryCache_.end())
      return it->second;
  }

  const FunctionSpec *spec = lookupSpec(functionName);
  FunctionCategory cat =
      spec ? categorizeFunctionSpec(*spec) : FunctionCategory::Unknown;

  if (cacheEnabled_)
    categoryCache_[functionName] = cat;

  return cat;
}

/**
 * @brief Get all applicable categories for an LLVM Function.
 *
 * Returns all categories that apply to the function, not just the primary one.
 * Falls back to intrinsic categorization if no specification is found.
 * Results are cached when caching is enabled.
 *
 * @param F Pointer to LLVM Function
 * @return Set of all FunctionCategory values that apply, empty if unknown
 */
std::set<FunctionCategory>
AliasSpecManager::getCategories(const llvm::Function *F) const {
  if (!F)
    return {};

  const std::string name = normalizeFunctionName(F);
  auto cats = getCategories(name);

  if (cats.empty()) {
    FunctionCategory intrinsic = categorizeIntrinsic(F);
    if (intrinsic != FunctionCategory::Unknown)
      cats.insert(intrinsic);
    if (cacheEnabled_)
      categoriesCache_[name] = cats;
  }
  return cats;
}

/**
 * @brief Get all applicable categories for a function by name.
 *
 * Returns all categories that apply to the function, not just the primary one.
 * Results are cached when caching is enabled.
 *
 * @param functionName Function name to look up
 * @return Set of all FunctionCategory values that apply, empty if unknown
 */
std::set<FunctionCategory>
AliasSpecManager::getCategories(const std::string &functionName) const {
  if (cacheEnabled_) {
    auto it = categoriesCache_.find(functionName);
    if (it != categoriesCache_.end())
      return it->second;
  }

  const FunctionSpec *spec = lookupSpec(functionName);
  std::set<FunctionCategory> cats =
      spec ? categorizeFunctionSpecMulti(*spec) : std::set<FunctionCategory>{};

  if (cacheEnabled_)
    categoriesCache_[functionName] = cats;

  return cats;
}

// ===== Allocator Queries =====

/**
 * @brief Check if an LLVM Function is an allocator.
 *
 * Allocators are functions that allocate memory (e.g., malloc, calloc, new).
 *
 * @param F Pointer to LLVM Function
 * @return true if the function is an allocator, false otherwise
 */
bool AliasSpecManager::isAllocator(const llvm::Function *F) const {
  return F && isAllocator(normalizeFunctionName(F));
}

/**
 * @brief Check if a function is an allocator by name.
 *
 * @param functionName Function name to check
 * @return true if the function is an allocator, false otherwise
 */
bool AliasSpecManager::isAllocator(const std::string &functionName) const {
  const FunctionSpec *spec = lookupSpec(functionName);
  return spec && spec->isAllocator;
}

/**
 * @brief Build AllocatorInfo from a function specification.
 *
 * Extracts allocator-specific information from a FunctionSpec, including:
 * - Which argument specifies the allocation size
 * - Whether the function returns a pointer or uses an out-parameter
 * - Whether the allocated memory is zero-initialized
 *
 * Handles special cases for well-known allocators (calloc, posix_memalign).
 *
 * @param name Function name (used for special case detection)
 * @param spec FunctionSpec containing allocator information
 * @return AllocatorInfo struct with extracted information
 */
AllocatorInfo
AliasSpecManager::buildAllocatorInfo(const std::string &name,
                                     const FunctionSpec &spec) const {
  AllocatorInfo info;
  info.functionName = name;
  info.returnsPointer = true;
  info.ptrOutArgIndex = -1;
  info.initializesToZero = false;

  if (!spec.allocs.empty()) {
    info.sizeArgIndex = spec.allocs[0].sizeArgIndex;
  } else {
    info.sizeArgIndex = -1;
  }

  // Special cases
  if (name == "calloc") {
    info.initializesToZero = true;
    info.sizeArgIndex = 1; // calloc(count, size)
  } else if (name == "posix_memalign") {
    info.returnsPointer = false;
    info.ptrOutArgIndex = 0;
    info.sizeArgIndex = 2;
  }

  return info;
}

/**
 * @brief Get detailed allocator information for an LLVM Function.
 *
 * Returns comprehensive information about an allocator function, including
 * size argument index, return behavior, and initialization properties.
 * Results are cached when caching is enabled.
 *
 * @param F Pointer to LLVM Function
 * @return Optional containing AllocatorInfo if function is an allocator, None
 * otherwise
 */
llvm::Optional<AllocatorInfo>
AliasSpecManager::getAllocatorInfo(const llvm::Function *F) const {
  if (!F)
    return llvm::None;
  return getAllocatorInfo(normalizeFunctionName(F));
}

/**
 * @brief Get detailed allocator information for a function by name.
 *
 * @param functionName Function name to look up
 * @return Optional containing AllocatorInfo if function is an allocator, None
 * otherwise
 */
llvm::Optional<AllocatorInfo>
AliasSpecManager::getAllocatorInfo(const std::string &functionName) const {
  if (cacheEnabled_) {
    auto it = allocatorCache_.find(functionName);
    if (it != allocatorCache_.end())
      return it->second;
  }

  const FunctionSpec *spec = lookupSpec(functionName);
  llvm::Optional<AllocatorInfo> result = llvm::None;

  if (spec && spec->isAllocator) {
    result = buildAllocatorInfo(functionName, *spec);
  }

  if (cacheEnabled_)
    allocatorCache_[functionName] = result;

  return result;
}

// ===== Deallocator Queries =====

/**
 * @brief Check if an LLVM Function is a deallocator.
 *
 * Deallocators are functions that free memory (e.g., free, delete).
 *
 * @param F Pointer to LLVM Function
 * @return true if the function is a deallocator, false otherwise
 */
bool AliasSpecManager::isDeallocator(const llvm::Function *F) const {
  return F && isDeallocator(normalizeFunctionName(F));
}

/**
 * @brief Check if a function is a deallocator by name.
 *
 * @param functionName Function name to check
 * @return true if the function is a deallocator, false otherwise
 */
bool AliasSpecManager::isDeallocator(const std::string &functionName) const {
  return isKnownDeallocator(functionName);
}

// ===== No-Effect Queries =====

/**
 * @brief Check if an LLVM Function has no pointer-related side effects.
 *
 * No-effect functions are pure functions that don't modify or alias any
 * pointers. This includes:
 * - Functions marked as ignored in specifications
 * - LLVM intrinsics like llvm.expect and llvm.assume
 *
 * @param F Pointer to LLVM Function
 * @return true if the function has no pointer effects, false otherwise
 */
bool AliasSpecManager::isNoEffect(const llvm::Function *F) const {
  if (!F)
    return false;
  if (isNoEffect(normalizeFunctionName(F)))
    return true;
  return categorizeIntrinsic(F) == FunctionCategory::NoEffect;
}

/**
 * @brief Check if a function has no pointer effects by name.
 *
 * @param functionName Function name to check
 * @return true if the function has no pointer effects, false otherwise
 */
bool AliasSpecManager::isNoEffect(const std::string &functionName) const {
  const FunctionSpec *spec = lookupSpec(functionName);
  return spec && spec->isIgnored;
}

// ===== Copy Operation Queries =====

/**
 * @brief Check if an LLVM Function performs memory copy operations.
 *
 * Memory copy functions copy data between memory regions (e.g., memcpy,
 * memmove). This includes both LLVM intrinsics and regular functions with copy
 * specifications.
 *
 * @param F Pointer to LLVM Function
 * @return true if the function performs memory copies, false otherwise
 */
bool AliasSpecManager::isMemoryCopy(const llvm::Function *F) const {
  if (!F)
    return false;
  if (isMemoryCopy(normalizeFunctionName(F)))
    return true;
  return categorizeIntrinsic(F) == FunctionCategory::MemoryCopy;
}

/**
 * @brief Check if a function performs memory copies by name.
 *
 * A function is considered a memory copy if it has copy operations between
 * regions (dereferenced pointers), as opposed to copying pointer values.
 *
 * @param functionName Function name to check
 * @return true if the function performs memory copies, false otherwise
 */
bool AliasSpecManager::isMemoryCopy(const std::string &functionName) const {
  const FunctionSpec *spec = lookupSpec(functionName);
  if (!spec)
    return false;

  for (const auto &copy : spec->copies) {
    if (copy.dstQualifier == QualifierKind::Region &&
        copy.srcQualifier == QualifierKind::Region)
      return true;
  }
  return false;
}

/**
 * @brief Build CopyInfo structures from a function specification.
 *
 * Extracts detailed information about all copy operations in a function
 * specification. Each copy operation describes:
 * - Source and destination argument indices
 * - Whether source/destination are regions (dereferenced) or direct pointers
 * - Whether the return value aliases the destination
 *
 * @param spec FunctionSpec containing copy operation information
 * @return Vector of CopyInfo structures, one per copy operation
 */
std::vector<CopyInfo>
AliasSpecManager::buildCopyInfo(const FunctionSpec &spec) const {
  std::vector<CopyInfo> result;

  for (const auto &copy : spec.copies) {
    CopyInfo info;

    // Destination
    if (copy.dst.kind == SelectorKind::Arg) {
      info.dstArgIndex = copy.dst.index;
      info.dstIsRegion = (copy.dstQualifier == QualifierKind::Region);
    } else if (copy.dst.kind == SelectorKind::Ret) {
      info.dstArgIndex = -1; // Return value
      info.dstIsRegion = (copy.dstQualifier == QualifierKind::Region);
    }

    // Source
    if (copy.src.kind == SelectorKind::Arg) {
      info.srcArgIndex = copy.src.index;
      info.srcIsRegion = (copy.srcQualifier == QualifierKind::Region);
    } else if (copy.src.kind == SelectorKind::Static) {
      info.srcArgIndex = -1;
      info.srcIsRegion = false;
    } else if (copy.src.kind == SelectorKind::Null) {
      info.srcArgIndex = -1;
      info.srcIsRegion = false;
    }

    // Return aliasing
    if (copy.dst.kind == SelectorKind::Ret) {
      info.returnsAlias = true;
      info.retArgIndex =
          copy.src.kind == SelectorKind::Arg ? copy.src.index : -1;
    } else {
      info.returnsAlias = false;
      info.retArgIndex = -1;
    }

    result.push_back(info);
  }

  return result;
}

/**
 * @brief Build CopyInfo for LLVM intrinsic copy functions.
 *
 * Handles well-known LLVM intrinsics that perform memory copies:
 * - llvm.memcpy: copies memory from source to destination
 * - llvm.memmove: copies memory (handles overlapping regions)
 *
 * Both intrinsics return the destination pointer, which aliases the first
 * argument.
 *
 * @param F Pointer to LLVM Function (must be an intrinsic)
 * @return Vector of CopyInfo structures, empty if not a copy intrinsic
 */
std::vector<CopyInfo>
AliasSpecManager::buildIntrinsicCopyInfo(const llvm::Function *F) const {
  std::vector<CopyInfo> result;
  if (!F || !F->isIntrinsic())
    return result;

  switch (F->getIntrinsicID()) {
  case llvm::Intrinsic::memcpy:
  case llvm::Intrinsic::memmove: {
    CopyInfo info;
    info.dstArgIndex = 0;
    info.srcArgIndex = 1;
    info.dstIsRegion = true;
    info.srcIsRegion = true;
    info.returnsAlias = true;
    info.retArgIndex = 0;
    result.push_back(info);
    break;
  }
  default:
    break;
  }
  return result;
}

/**
 * @brief Get all copy effects for an LLVM Function.
 *
 * Returns detailed information about all memory copy operations performed
 * by the function. Falls back to intrinsic handling for LLVM intrinsics.
 * Results are cached when caching is enabled.
 *
 * @param F Pointer to LLVM Function
 * @return Vector of CopyInfo structures describing all copy operations
 */
std::vector<CopyInfo>
AliasSpecManager::getCopyEffects(const llvm::Function *F) const {
  if (!F)
    return {};
  auto copies = getCopyEffects(normalizeFunctionName(F));
  if (!copies.empty())
    return copies;

  // Fallback for well-known intrinsics
  auto intrinsicCopies = buildIntrinsicCopyInfo(F);
  if (cacheEnabled_)
    copyCache_[normalizeFunctionName(F)] = intrinsicCopies;
  return intrinsicCopies;
}

/**
 * @brief Get all copy effects for a function by name.
 *
 * @param functionName Function name to look up
 * @return Vector of CopyInfo structures describing all copy operations
 */
std::vector<CopyInfo>
AliasSpecManager::getCopyEffects(const std::string &functionName) const {
  if (cacheEnabled_) {
    auto it = copyCache_.find(functionName);
    if (it != copyCache_.end())
      return it->second;
  }

  const FunctionSpec *spec = lookupSpec(functionName);
  std::vector<CopyInfo> result =
      spec ? buildCopyInfo(*spec) : std::vector<CopyInfo>{};

  if (cacheEnabled_)
    copyCache_[functionName] = result;

  return result;
}

// ===== Return Alias Queries =====

/**
 * @brief Check if an LLVM Function's return value aliases an argument.
 *
 * Some functions return a pointer that aliases one of their arguments
 * (e.g., strcpy returns the destination, memcpy returns the destination).
 * This is important for alias analysis to track pointer relationships.
 *
 * @param F Pointer to LLVM Function
 * @return true if the return value aliases an argument, false otherwise
 */
bool AliasSpecManager::returnsArgumentAlias(const llvm::Function *F) const {
  if (!F)
    return false;
  if (returnsArgumentAlias(normalizeFunctionName(F)))
    return true;
  // memcpy/memmove return the destination pointer
  return categorizeIntrinsic(F) == FunctionCategory::MemoryCopy;
}

/**
 * @brief Check if a function's return value aliases an argument by name.
 *
 * @param functionName Function name to check
 * @return true if the return value aliases an argument, false otherwise
 */
bool AliasSpecManager::returnsArgumentAlias(
    const std::string &functionName) const {
  const FunctionSpec *spec = lookupSpec(functionName);
  if (!spec)
    return false;

  for (const auto &copy : spec->copies) {
    if (copy.dst.kind == SelectorKind::Ret)
      return true;
  }
  return false;
}

/**
 * @brief Build ReturnAliasInfo structures from a function specification.
 *
 * Extracts information about how the return value aliases arguments or
 * other memory locations. A function can have multiple return alias
 * relationships (though this is rare).
 *
 * @param spec FunctionSpec containing return alias information
 * @return Vector of ReturnAliasInfo structures, one per return alias
 * relationship
 */
std::vector<ReturnAliasInfo>
AliasSpecManager::buildReturnAliasInfo(const FunctionSpec &spec) const {
  std::vector<ReturnAliasInfo> result;

  for (const auto &copy : spec.copies) {
    if (copy.dst.kind != SelectorKind::Ret)
      continue;

    ReturnAliasInfo info;
    info.argIndex = copy.src.kind == SelectorKind::Arg ? copy.src.index : -1;
    info.isRegion = (copy.dstQualifier == QualifierKind::Region);
    info.isStatic = (copy.src.kind == SelectorKind::Static);
    info.isNull = (copy.src.kind == SelectorKind::Null);

    result.push_back(info);
  }

  return result;
}

/**
 * @brief Get return alias information for an LLVM Function.
 *
 * Returns detailed information about how the function's return value
 * relates to its arguments. Handles both specifications and intrinsics.
 * Results are cached when caching is enabled.
 *
 * @param F Pointer to LLVM Function
 * @return Vector of ReturnAliasInfo structures, empty if no return aliasing
 */
std::vector<ReturnAliasInfo>
AliasSpecManager::getReturnAliasInfo(const llvm::Function *F) const {
  if (!F)
    return {};
  std::string name = normalizeFunctionName(F);
  auto info = getReturnAliasInfo(name);
  if (!info.empty())
    return info;

  if (categorizeIntrinsic(F) == FunctionCategory::MemoryCopy) {
    ReturnAliasInfo intrinsicInfo;
    intrinsicInfo.argIndex = 0;
    intrinsicInfo.isRegion = false;
    intrinsicInfo.isStatic = false;
    intrinsicInfo.isNull = false;
    std::vector<ReturnAliasInfo> intrinsic = {intrinsicInfo};
    if (cacheEnabled_)
      returnAliasCache_[name] = intrinsic;
    return intrinsic;
  }
  return {};
}

/**
 * @brief Get return alias information for a function by name.
 *
 * @param functionName Function name to look up
 * @return Vector of ReturnAliasInfo structures, empty if no return aliasing
 */
std::vector<ReturnAliasInfo>
AliasSpecManager::getReturnAliasInfo(const std::string &functionName) const {
  if (cacheEnabled_) {
    auto it = returnAliasCache_.find(functionName);
    if (it != returnAliasCache_.end())
      return it->second;
  }

  const FunctionSpec *spec = lookupSpec(functionName);
  std::vector<ReturnAliasInfo> result =
      spec ? buildReturnAliasInfo(*spec) : std::vector<ReturnAliasInfo>{};

  if (cacheEnabled_)
    returnAliasCache_[functionName] = result;

  return result;
}

// ===== Exit Function Queries =====

/**
 * @brief Check if an LLVM Function is an exit function.
 *
 * Exit functions terminate program execution (e.g., exit, abort, _exit).
 * These are important for alias analysis because they represent program
 * termination points.
 *
 * @param F Pointer to LLVM Function
 * @return true if the function is an exit function, false otherwise
 */
bool AliasSpecManager::isExitFunction(const llvm::Function *F) const {
  return F && isExitFunction(normalizeFunctionName(F));
}

/**
 * @brief Check if a function is an exit function by name.
 *
 * @param functionName Function name to check
 * @return true if the function is an exit function, false otherwise
 */
bool AliasSpecManager::isExitFunction(const std::string &functionName) const {
  const FunctionSpec *spec = lookupSpec(functionName);
  return spec && spec->isExit;
}

// ===== Mod/Ref Queries =====

/**
 * @brief Build ModRefInfo from a function specification.
 *
 * Extracts modification and reference information from a function
 * specification. Mod/Ref information describes which arguments are:
 * - Modified (written to): arguments that the function changes
 * - Referenced (read from): arguments that the function reads
 *
 * Also tracks whether the return value region is modified or referenced.
 *
 * @param spec FunctionSpec containing mod/ref annotations
 * @return ModRefInfo struct with extracted modification and reference
 * information
 */
ModRefInfo AliasSpecManager::buildModRefInfo(const FunctionSpec &spec) const {
  ModRefInfo info;

  for (const auto &mr : spec.modref) {
    if (mr.target.kind == SelectorKind::Arg) {
      int argIdx = mr.target.index;
      if (mr.op == SpecOpKind::Mod) {
        info.modifiedArgs.push_back(argIdx);
      } else if (mr.op == SpecOpKind::Ref) {
        info.referencedArgs.push_back(argIdx);
      }
    } else if (mr.target.kind == SelectorKind::Ret) {
      if (mr.op == SpecOpKind::Mod) {
        info.modifiesReturn = true;
      } else if (mr.op == SpecOpKind::Ref) {
        info.referencesReturn = true;
      }
    }
  }

  return info;
}

/**
 * @brief Build ModRefInfo for LLVM intrinsic functions.
 *
 * Handles well-known LLVM intrinsics that modify or reference memory:
 * - llvm.memcpy, llvm.memmove: modify arg 0, reference arg 1
 * - llvm.memset: modifies arg 0
 *
 * @param F Pointer to LLVM Function (must be an intrinsic)
 * @return ModRefInfo struct, empty if not a recognized intrinsic
 */
ModRefInfo
AliasSpecManager::buildIntrinsicModRefInfo(const llvm::Function *F) const {
  ModRefInfo info;
  if (!F || !F->isIntrinsic())
    return info;

  switch (F->getIntrinsicID()) {
  case llvm::Intrinsic::memcpy:
  case llvm::Intrinsic::memmove:
    info.modifiedArgs.push_back(0);
    info.referencedArgs.push_back(1);
    break;
  case llvm::Intrinsic::memset:
    info.modifiedArgs.push_back(0);
    break;
  default:
    break;
  }
  return info;
}

/**
 * @brief Get mod/ref information for an LLVM Function.
 *
 * Returns comprehensive information about which arguments are modified
 * or referenced by the function. Handles both specifications and intrinsics.
 * Results are cached when caching is enabled.
 *
 * @param F Pointer to LLVM Function
 * @return ModRefInfo struct describing modification and reference behavior
 */
ModRefInfo AliasSpecManager::getModRefInfo(const llvm::Function *F) const {
  if (!F)
    return ModRefInfo();

  const std::string name = normalizeFunctionName(F);
  if (cacheEnabled_) {
    auto it = modRefCache_.find(name);
    if (it != modRefCache_.end())
      return it->second;
  }

  const FunctionSpec *spec = lookupSpec(name);
  ModRefInfo result =
      spec ? buildModRefInfo(*spec) : buildIntrinsicModRefInfo(F);

  if (cacheEnabled_)
    modRefCache_[name] = result;

  return result;
}

/**
 * @brief Get mod/ref information for a function by name.
 *
 * @param functionName Function name to look up
 * @return ModRefInfo struct describing modification and reference behavior
 */
ModRefInfo
AliasSpecManager::getModRefInfo(const std::string &functionName) const {
  if (cacheEnabled_) {
    auto it = modRefCache_.find(functionName);
    if (it != modRefCache_.end())
      return it->second;
  }

  const FunctionSpec *spec = lookupSpec(functionName);
  ModRefInfo result = spec ? buildModRefInfo(*spec) : ModRefInfo();

  if (cacheEnabled_)
    modRefCache_[functionName] = result;

  return result;
}

/**
 * @brief Check if a function modifies a specific argument.
 *
 * Convenience method that checks if the given argument index is in the
 * list of modified arguments.
 *
 * @param F Pointer to LLVM Function
 * @param argIndex Argument index to check (0-based)
 * @return true if the argument is modified, false otherwise
 */
bool AliasSpecManager::modifiesArg(const llvm::Function *F,
                                   int argIndex) const {
  auto info = getModRefInfo(F);
  return std::find(info.modifiedArgs.begin(), info.modifiedArgs.end(),
                   argIndex) != info.modifiedArgs.end();
}

/**
 * @brief Check if a function references (reads) a specific argument.
 *
 * Convenience method that checks if the given argument index is in the
 * list of referenced arguments.
 *
 * @param F Pointer to LLVM Function
 * @param argIndex Argument index to check (0-based)
 * @return true if the argument is referenced, false otherwise
 */
bool AliasSpecManager::referencesArg(const llvm::Function *F,
                                     int argIndex) const {
  auto info = getModRefInfo(F);
  return std::find(info.referencedArgs.begin(), info.referencedArgs.end(),
                   argIndex) != info.referencedArgs.end();
}

// ===== Batch Queries =====

/**
 * @brief Build pre-computed category lists for efficient batch queries.
 *
 * Pre-processes all loaded specifications to build lists of function names
 * grouped by category. This enables efficient retrieval of all functions
 * in a specific category without iterating through all specifications.
 *
 * The lists are built lazily on first access and cached for subsequent queries.
 * Should be called automatically by getFunctionsByCategory().
 */
void AliasSpecManager::buildCategoryLists() const {
  if (categoryListsBuilt_)
    return;

  categoryLists_.clear();

  for (const auto &entry : apiSpec_.all()) {
    const std::string &name = entry.first;
    const FunctionSpec &spec = entry.second;

    auto cats = categorizeFunctionSpecMulti(spec);
    for (auto cat : cats) {
      categoryLists_[cat].push_back(name);
    }
  }

  categoryListsBuilt_ = true;
}

/**
 * @brief Get all function names in a specific category.
 *
 * Returns a list of all function names that belong to the given category.
 * This is useful for analysis initialization when you need to know all
 * allocators, deallocators, etc. upfront.
 *
 * @param cat FunctionCategory to query
 * @return Vector of function names in the category, empty if none found
 */
std::vector<std::string>
AliasSpecManager::getFunctionsByCategory(FunctionCategory cat) const {
  buildCategoryLists();

  auto it = categoryLists_.find(cat);
  if (it != categoryLists_.end())
    return it->second;
  return {};
}

/**
 * @brief Get all known allocator function names.
 *
 * Convenience method that returns all functions in the Allocator category.
 * Useful for initializing allocation-aware alias analyses.
 *
 * @return Vector of allocator function names
 */
std::vector<std::string> AliasSpecManager::getAllocatorNames() const {
  return getFunctionsByCategory(FunctionCategory::Allocator);
}

/**
 * @brief Get all known deallocator function names.
 *
 * Convenience method that returns all functions in the Deallocator category.
 *
 * @return Vector of deallocator function names
 */
std::vector<std::string> AliasSpecManager::getDeallocatorNames() const {
  return getFunctionsByCategory(FunctionCategory::Deallocator);
}

/**
 * @brief Get all known no-effect function names.
 *
 * Convenience method that returns all functions in the NoEffect category.
 * These are pure functions that can be safely ignored for pointer analysis.
 *
 * @return Vector of no-effect function names
 */
std::vector<std::string> AliasSpecManager::getNoEffectNames() const {
  return getFunctionsByCategory(FunctionCategory::NoEffect);
}
