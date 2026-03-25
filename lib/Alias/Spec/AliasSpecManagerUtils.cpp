/**
 * @file AliasSpecManagerUtils.cpp
 * @brief Utility functions for AliasSpecManager: file path resolution and
 * category conversion
 *
 * This file provides helper functions for:
 * - Locating specification files in the filesystem
 * - Converting between FunctionCategory enum and string representations
 *
 * The file path resolution follows a priority order:
 * 1. LOTUS_CONFIG_DIR environment variable (if set)
 * 2. config/ directory in current working directory
 * 3. config/ directory in parent directory
 */

#include "Alias/Spec/AliasSpecManager.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_set>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

using namespace lotus;
using namespace lotus::alias;

namespace {
/**
 * @brief Build a list of candidate configuration directories, ordered by
 * preference.
 *
 * The search order is:
 * 1. Directory specified by LOTUS_CONFIG_DIR environment variable (highest
 * priority)
 * 2. config/ subdirectory in current working directory
 * 3. config/ subdirectory in parent directory
 *
 * @return Vector of directory paths, deduplicated while preserving order
 */
std::vector<std::string> candidateConfigDirs() {
  std::vector<std::string> dirs;

  if (const char *envPath = std::getenv("LOTUS_CONFIG_DIR")) {
    if (*envPath)
      dirs.emplace_back(envPath);
  }

  llvm::SmallString<256> cwd;
  if (!llvm::sys::fs::current_path(cwd)) {
    llvm::SmallString<256> configInCwd = cwd;
    llvm::sys::path::append(configInCwd, "config");
    dirs.emplace_back(configInCwd.str().str());

    llvm::SmallString<256> parent = cwd;
    llvm::sys::path::remove_filename(parent);
    llvm::SmallString<256> configInParent = parent;
    llvm::sys::path::append(configInParent, "config");
    dirs.emplace_back(configInParent.str().str());
  }

  // Deduplicate while preserving order
  std::vector<std::string> unique;
  std::unordered_set<std::string> seen;
  for (const auto &d : dirs) {
    if (seen.insert(d).second)
      unique.push_back(d);
  }
  return unique;
}

/**
 * @brief Find existing specification files in candidate directories.
 *
 * Searches for each spec file name in all candidate directories (in priority
 * order). Returns only files that actually exist on the filesystem, avoiding
 * duplicates.
 *
 * @param specNames Vector of specification file names to search for (e.g.,
 * "ptr.spec")
 * @return Vector of full paths to existing spec files, in order of discovery
 */
std::vector<std::string>
findExistingSpecFiles(const std::vector<std::string> &specNames) {
  std::vector<std::string> results;
  std::unordered_set<std::string> seen;

  for (const auto &dir : candidateConfigDirs()) {
    for (const auto &name : specNames) {
      llvm::SmallString<256> path(dir);
      llvm::sys::path::append(path, name);
      std::string fullPath = path.str().str();
      if (seen.count(fullPath))
        continue;
      if (llvm::sys::fs::exists(fullPath)) {
        results.push_back(fullPath);
        seen.insert(fullPath);
      }
    }
  }
  return results;
}
} // namespace

// ===== Utility Functions =====

/**
 * @brief Get default specification file paths.
 *
 * Returns paths to the default specification files (ptr.spec and modref.spec).
 * The search order follows candidateConfigDirs() priority:
 * 1. Checks LOTUS_CONFIG_DIR environment variable
 * 2. Checks config/ in current and parent directories
 *
 * If no existing files are found, returns paths in the first candidate
 * directory (preserving backward compatibility). As a last resort, returns
 * relative paths assuming config/ in the current directory.
 *
 * @return Vector of specification file paths (typically 2 files: ptr.spec,
 * modref.spec)
 */
std::vector<std::string> lotus::alias::getDefaultSpecFiles() {
  static const std::vector<std::string> specNames = {"ptr.spec", "modref.spec"};
  auto existing = findExistingSpecFiles(specNames);
  if (!existing.empty())
    return existing;

  // Fallback to first candidate directory even if files don't yet exist,
  // preserving previous behavior.
  auto dirs = candidateConfigDirs();
  if (!dirs.empty()) {
    std::vector<std::string> candidates;
    for (const auto &name : specNames)
      candidates.push_back(dirs.front() + "/" + name);
    return candidates;
  }

  // Last resort: assume relative config/ like old behavior.
  return {"config/ptr.spec", "config/modref.spec"};
}

/**
 * @brief Get the full path to a specific specification file.
 *
 * Searches for the given spec file name in candidate directories (in priority
 * order) and returns the first existing file found. If no existing file is
 * found, returns a path in the config/ subdirectory (for backward
 * compatibility).
 *
 * @param specFileName Name of the specification file (e.g., "ptr.spec")
 * @return Full path to the spec file, or "config/" + specFileName if not found
 */
std::string lotus::alias::getSpecFilePath(const std::string &specFileName) {
  // Prefer the first candidate directory that exists.
  for (const auto &dir : candidateConfigDirs()) {
    llvm::SmallString<256> path(dir);
    llvm::sys::path::append(path, specFileName);
    if (llvm::sys::fs::exists(path))
      return path.str().str();
  }

  // Preserve original fallback behavior.
  return "config/" + specFileName;
}

/**
 * @brief Convert a FunctionCategory enum value to its string representation.
 *
 * Useful for debugging, logging, and serialization. Returns a human-readable
 * string describing the function category.
 *
 * @param cat The function category to convert
 * @return C-string containing the category name, or "Unknown" if invalid
 */
const char *lotus::alias::categoryToString(FunctionCategory cat) {
  switch (cat) {
  case FunctionCategory::Unknown:
    return "Unknown";
  case FunctionCategory::Allocator:
    return "Allocator";
  case FunctionCategory::Deallocator:
    return "Deallocator";
  case FunctionCategory::Reallocator:
    return "Reallocator";
  case FunctionCategory::MemoryCopy:
    return "MemoryCopy";
  case FunctionCategory::MemorySet:
    return "MemorySet";
  case FunctionCategory::MemoryCompare:
    return "MemoryCompare";
  case FunctionCategory::StringOperation:
    return "StringOperation";
  case FunctionCategory::NoEffect:
    return "NoEffect";
  case FunctionCategory::ExitFunction:
    return "ExitFunction";
  case FunctionCategory::ReturnArgument:
    return "ReturnArgument";
  case FunctionCategory::IoOperation:
    return "IoOperation";
  case FunctionCategory::MathFunction:
    return "MathFunction";
  default:
    return "Unknown";
  }
}

/**
 * @brief Parse a FunctionCategory from its string representation.
 *
 * Converts a string (e.g., "Allocator", "Deallocator") to the corresponding
 * FunctionCategory enum value. This is the inverse of categoryToString().
 *
 * Note: Not all categories are supported in this conversion (only the most
 * commonly used ones for specification files).
 *
 * @param str String representation of the category
 * @return Optional containing the category if parsing succeeds, None otherwise
 */
llvm::Optional<FunctionCategory>
lotus::alias::stringToCategory(const std::string &str) {
  if (str == "Allocator")
    return FunctionCategory::Allocator;
  if (str == "Deallocator")
    return FunctionCategory::Deallocator;
  if (str == "Reallocator")
    return FunctionCategory::Reallocator;
  if (str == "MemoryCopy")
    return FunctionCategory::MemoryCopy;
  if (str == "MemorySet")
    return FunctionCategory::MemorySet;
  if (str == "NoEffect")
    return FunctionCategory::NoEffect;
  if (str == "ExitFunction")
    return FunctionCategory::ExitFunction;
  if (str == "ReturnArgument")
    return FunctionCategory::ReturnArgument;
  return llvm::None;
}
