/** @file SuppressionManager.h @brief Bug report suppression manager for filtering known false positives. */
#ifndef CHECKER_REPORT_SUPPRESSIONMANAGER_H
#define CHECKER_REPORT_SUPPRESSIONMANAGER_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/raw_ostream.h>

/**
 * SuppressionManager - Manages issue suppressions for bug reports.
 *
 * Inspired by Infer's Suppressions.ml, this class handles:
 * - File-based suppressions (entire file)
 * - Line range suppressions (blocks of lines)
 * - Issue-type specific suppressions
 * - JSON-based suppression file format
 *
 * Usage:
 *   SuppressionManager mgr;
 *   mgr.loadFromFile("suppressions.json");
 *   if (mgr.isSuppressed("NPD", "file.cpp", 42)) {
 *       // Skip this report
 *   }
 */
class SuppressionManager {
public:
  /**
   * Represents a span of lines that can be suppressed.
   * - Every: Suppress entire file
   * - Blocks: Suppress specific line ranges
   */
  struct Span {
    enum Type { Every, Blocks };

    Type type;
    std::vector<std::pair<int, int>> blocks; // (first, last) line pairs

    Span() : type(Every) {}
    Span(Type t) : type(t) {}

    bool contains(int line) const;
    void addBlock(int first, int last);
  };

  SuppressionManager();
  ~SuppressionManager();

  /**
   * Load suppressions from a JSON file.
   * Format:
   * {
   *   "file.cpp": {
   *     "NPD": "Every",  // or [[1, 10], [20, 30]] for line ranges
   *     "DataRace": [[5, 15]]
   *   }
   * }
   */
  bool loadFromFile(const std::string &filename);

  /**
   * Load suppressions from JSON string.
   */
  bool loadFromString(const std::string &json);

  /**
   * Add a suppression programmatically.
   * @param issueType Issue type name (e.g., "NPD", "DataRace")
   * @param filename Source file path
   * @param span Suppression span (Every or Blocks)
   */
  void addSuppression(const std::string &issueType, const std::string &filename,
                      const Span &span);

  /**
   * Add a suppression for entire file.
   */
  void addFileSuppression(const std::string &issueType,
                          const std::string &filename);

  /**
   * Add a suppression for a line range.
   */
  void addLineRangeSuppression(const std::string &issueType,
                               const std::string &filename, int firstLine,
                               int lastLine);

  /**
   * Check if an issue is suppressed.
   * @param issueType Issue type name
   * @param filename Source file path
   * @param line Line number (0-based or 1-based, will be normalized)
   * @return true if suppressed, false otherwise
   */
  bool isSuppressed(const std::string &issueType, const std::string &filename,
                    int line) const;

  /**
   * Check if an issue type is suppressed for entire file.
   */
  bool isFileSuppressed(const std::string &issueType,
                        const std::string &filename) const;

  /**
   * Get all suppressed issue types for a file.
   */
  llvm::StringSet<> getSuppressedTypes(const std::string &filename) const;

  /**
   * Clear all suppressions.
   */
  void clear();

  /**
   * Export suppressions to JSON format.
   */
  void exportToJson(llvm::raw_ostream &OS) const;

  /**
   * Get statistics about suppressions.
   */
  struct Stats {
    size_t totalFiles = 0;
    size_t totalIssueTypes = 0;
    size_t totalSuppressions = 0;
  };
  Stats getStats() const;

  /**
   * Parse suppression comment from source code.
   * Supports formats:
   *   - // lotus-suppress: ISSUE_TYPE
   *   - // lotus-suppress: ISSUE_TYPE on line 10-20
   *   - C-style block comments with lotus-suppress: ISSUE_TYPE
   */
  static bool
  parseSuppressionComment(const std::string &comment, std::string &issueType,
                          std::vector<std::pair<int, int>> &lineRanges);

private:
  // Map: filename -> (issueType -> Span)
  using SuppressionMap = std::map<std::string, std::map<std::string, Span>>;
  SuppressionMap suppressions;

  // Helper to normalize file paths
  std::string normalizePath(const std::string &path) const;

  // Helper to parse JSON
  bool parseJsonObject(const std::string &json);
  Span parseSpan(const std::string &jsonValue);
};

#endif // CHECKER_REPORT_SUPPRESSIONMANAGER_H
