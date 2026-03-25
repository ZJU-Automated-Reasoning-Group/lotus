/**
 * @file AliasAnalysisConfigParser.cpp
 * @brief String parsing utilities for AAConfig
 * 
 * This file provides utility functions to parse string representations
 * of alias analysis configurations into AAConfig objects. This is useful
 * for command-line tools and configuration files.
 */

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include <algorithm>
#include <cctype>

using namespace lotus;

namespace {

/**
 * @brief Convert a string to lowercase
 * 
 * Helper function that creates a lowercase copy of the input string.
 * Used for case-insensitive string matching in the parser.
 * 
 * @param str Input string to convert
 * @return Lowercase copy of the input string
 */
std::string toLower(const std::string &str) {
  std::string result = str;
  std::transform(result.begin(), result.end(), result.begin(), ::tolower);
  return result;
}

} // namespace

/**
 * @brief Parse a string representation into an AAConfig object
 * 
 * This function parses common string representations of alias analysis
 * configurations and returns the corresponding AAConfig. It supports a wide
 * variety of string formats for backward compatibility and ease of use.
 * 
 * Supported formats:
 * 
 * **SparrowAA (Andersen-style):**
 * - "andersen", "sparrow-aa", "sparrowaa" -> SparrowAA_NoCtx()
 * - "andersen-1cfa", "1cfa", "sparrow-aa-1cfa" -> SparrowAA_1CFA()
 * - "andersen-2cfa", "2cfa", "sparrow-aa-2cfa" -> SparrowAA_2CFA()
 * - "nocx", "noctx", "0cfa" -> SparrowAA_NoCtx()
 * 
 * **AserPTA:**
 * - "aser-pta", "aserpta" -> AserPTA_NoCtx()
 * - "aser-pta-1cfa" -> AserPTA_1CFA()
 * - "aser-pta-2cfa" -> AserPTA_2CFA()
 * - "aser-pta-origin" -> AserPTA_Origin()
 * 
 * **TPA:**
 * - "tpa", "tpa-0cfa" -> TPA_NoCtx()
 * - "tpa-1cfa" -> TPA_1CFA()
 * - "tpa-2cfa" -> TPA_2CFA()
 * - "tpa-3cfa" -> TPA_3CFA()
 * - "tpa-k" or "tpa-kcfa" (where k is a number) -> TPA_KCFA(k)
 * 
 * **Other analyses:**
 * - "dyck", "dyckaa" -> DyckAA()
 * - "cfl-anders", "cflanders" -> CFLAnders()
 * - "cfl-steens", "cflsteens" -> CFLSteens()
 * - "seadsa" -> SeaDsa()
 * - "allocaa", "alloc" -> AllocAA()
 * - "basic", "basicaa" -> BasicAA()
 * - "tbaa" -> TBAA()
 * - "globals", "globalsaa" -> GlobalsAA()
 * - "scevaa", "scev" -> SCEVAA()
 * - "sraa" -> SRAA()
 * - "combined" -> Combined()
 * - "underapprox" -> UnderApprox()
 * 
 * @param str String representation of the alias analysis configuration
 * @param fallback Configuration to return if the string is unknown - MUST be explicitly specified.
 *                 This ensures users are aware of what fallback analysis will be used.
 * @return AAConfig corresponding to the string, or fallback if the string
 *         is not recognized
 * 
 * @note Matching is case-insensitive
 * @note If the string is empty or unrecognized, returns the fallback config
 * @note The fallback parameter is required - there is no default. This ensures
 *       users explicitly choose what analysis to use when parsing fails.
 * @note For TPA, supports parsing custom k-CFA levels from strings like
 *       "tpa-5" or "tpa-5cfa" to create TPA_KCFA(5)
 * @note This function is designed for command-line tools and configuration
 *       files where users specify analyses by name
 * 
 * @example
 * ```cpp
 * // Parse common formats - fallback MUST be explicitly specified
 * auto config1 = parseAAConfigFromString("andersen-1cfa", AAConfig::SparrowAA_NoCtx());
 * auto config2 = parseAAConfigFromString("tpa-2cfa", AAConfig::TPA_NoCtx());
 * auto config3 = parseAAConfigFromString("dyck", AAConfig::DyckAA());
 * 
 * // Custom k-CFA for TPA
 * auto config4 = parseAAConfigFromString("tpa-5cfa", AAConfig::TPA_NoCtx());
 * 
 * // With explicit fallback for unknown strings
 * auto config5 = parseAAConfigFromString("unknown", AAConfig::DyckAA());
 * ```
 */
AAConfig lotus::parseAAConfigFromString(const std::string &str, const AAConfig &fallback) {
  std::string lower = toLower(str);
  
  // SparrowAA variants
  if (lower == "andersen" || lower == "sparrow-aa" || lower == "sparrowaa" ||
      lower == "andersen-nocontext" || lower == "andersen-noctx" ||
      lower == "andersen-0cfa" || lower == "andersen0" ||
      lower == "nocx" || lower == "noctx" || lower == "0cfa") {
    return AAConfig::SparrowAA_NoCtx();
  }
  if (lower == "andersen-1cfa" || lower == "andersen1" || lower == "1cfa" ||
      lower == "sparrow-aa-1cfa" || lower == "sparrowaa-1cfa") {
    return AAConfig::SparrowAA_1CFA();
  }
  if (lower == "andersen-2cfa" || lower == "andersen2" || lower == "2cfa" ||
      lower == "sparrow-aa-2cfa" || lower == "sparrowaa-2cfa") {
    return AAConfig::SparrowAA_2CFA();
  }
  
  // AserPTA variants
  if (lower == "aser-pta" || lower == "aserpta" || lower == "aser-pta-0cfa") {
    return AAConfig::AserPTA_NoCtx();
  }
  if (lower == "aser-pta-1cfa" || lower == "aserpta-1cfa") {
    return AAConfig::AserPTA_1CFA();
  }
  if (lower == "aser-pta-2cfa" || lower == "aserpta-2cfa") {
    return AAConfig::AserPTA_2CFA();
  }
  if (lower == "aser-pta-origin" || lower == "aserpta-origin") {
    return AAConfig::AserPTA_Origin();
  }
  
  // DDA (demand-driven on SVFG)
  if (lower == "dda" || lower == "demand-driven" || lower == "demanddriven") {
    return AAConfig::DDA_NoCtx();
  }
  
  // TPA variants
  if (lower == "tpa" || lower == "tpa-0cfa") {
    return AAConfig::TPA_NoCtx();
  }
  if (lower == "tpa-1cfa") {
    return AAConfig::TPA_1CFA();
  }
  if (lower == "tpa-2cfa") {
    return AAConfig::TPA_2CFA();
  }
  if (lower == "tpa-3cfa") {
    return AAConfig::TPA_3CFA();
  }
  // Parse custom k-CFA for TPA: "tpa-k" or "tpa-kcfa" where k is a number
  // This handles cases like "tpa-5", "tpa-5cfa", "tpa-10", etc.
  if (lower.find("tpa-") == 0 && lower.size() > 4) {
    // Extract the part after "tpa-"
    std::string kStr = lower.substr(4);
    // Remove "cfa" suffix if present
    if (kStr.size() >= 3 && kStr.substr(kStr.size() - 3) == "cfa") {
      kStr = kStr.substr(0, kStr.size() - 3);
    }
    // Only parse if we haven't already matched a specific TPA variant above
    // (i.e., if it's not "tpa-0cfa", "tpa-1cfa", "tpa-2cfa", or "tpa-3cfa")
    if (kStr != "0" && kStr != "1" && kStr != "2" && kStr != "3") {
      try {
        unsigned k = std::stoul(kStr);
        return AAConfig::TPA_KCFA(k);
      } catch (...) {
        // Invalid number, fall through
      }
    }
  }
  
  // Other analyses
  if (lower == "dyck" || lower == "dyckaa") {
    return AAConfig::DyckAA();
  }
  if (lower == "cfl-anders" || lower == "cflanders") {
    return AAConfig::CFLAnders();
  }
  if (lower == "cfl-steens" || lower == "cflsteens") {
    return AAConfig::CFLSteens();
  }
  if (lower == "seadsa") {
    return AAConfig::SeaDsa();
  }
  if (lower == "allocaa" || lower == "alloc") {
    return AAConfig::AllocAA();
  }
  if (lower == "basic" || lower == "basicaa") {
    return AAConfig::BasicAA();
  }
  if (lower == "tbaa") {
    return AAConfig::TBAA();
  }
  if (lower == "globals" || lower == "globalsaa") {
    return AAConfig::GlobalsAA();
  }
  if (lower == "scevaa" || lower == "scev") {
    return AAConfig::SCEVAA();
  }
  if (lower == "sraa") {
    return AAConfig::SRAA();
  }
  if (lower == "combined") {
    return AAConfig::Combined();
  }
  if (lower == "underapprox") {
    return AAConfig::UnderApprox();
  }
  
  // Unknown string, return fallback
  return fallback;
}
