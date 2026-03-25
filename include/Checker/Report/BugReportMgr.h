#ifndef CHECKER_REPORT_BUGREPORTMGR_H
#define CHECKER_REPORT_BUGREPORTMGR_H

#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugTypes.h"
#include "Checker/Report/SuppressionManager.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/StringMap.h>
#include <llvm/Support/raw_ostream.h>

/**
 * BugReportMgr - Centralized manager for all bug reports.
 * Follows the shared reporting architecture pattern.
 *
 * Responsibilities:
 * - Register bug types with IDs
 * - Store and organize reports by type
 * - Export reports to JSON format
 * - Generate statistics and summaries
 */
class BugReportMgr {
public:
  /**
   * Describes a type of bug (e.g., NPD, Data Race, Taint)
   */
  struct BugType {
    int id;
    llvm::StringRef bug_name;
    BugDescription::BugImportance importance;
    BugDescription::BugClassification classification;
    llvm::StringRef desc;

    BugType()
        : id(-1), bug_name(""), importance(BugDescription::BI_NA),
          classification(BugDescription::BC_NA), desc("") {}

    BugType(int id, const llvm::StringRef &name,
            BugDescription::BugImportance imp,
            BugDescription::BugClassification cls,
            const llvm::StringRef &description)
        : id(id), bug_name(name), importance(imp), classification(cls),
          desc(description) {}
  };

public:
  BugReportMgr();
  ~BugReportMgr();

  /**
   * Register a new bug type and return its ID.
   * If already registered, returns existing ID.
   */
  int register_bug_type(
      llvm::StringRef ty_name,
      BugDescription::BugImportance importance = BugDescription::BI_NA,
      BugDescription::BugClassification classification = BugDescription::BC_NA,
      llvm::StringRef desc = "");

  /**
   * Find bug type ID by name. Returns -1 if not found.
   */
  int find_bug_type(llvm::StringRef ty_name);

  /**
   * Get bug type information by ID
   */
  const BugType &get_bug_type_info(int ty_id) const;

  /**
   * Insert a bug report with deduplication
   * Returns true if the report was added, false if it was a duplicate
   */
  bool insert_report(int ty_id, BugReport *report,
                     bool deduplicate_by_trace = false);

  /**
   * Insert a bug report (legacy method, calls insert_report with deduplication
   * disabled)
   */
  void insert_report(int ty_id, BugReport *report);

  /**
   * Get all reports for a specific bug type
   */
  const std::vector<BugReport *> *get_reports_for_type(int ty_id) const;

  /**
   * Clear reports for a subset of bug types and rebuild deduplication state.
   */
  void clear_reports_for_types(const std::vector<int> &ty_ids);

  /**
   * Clear all stored reports and deduplication state.
   */
  void clear_all_reports();

  /**
   * Deduplicate reports based on location or trace
   * Enhanced with Infer-inspired location-based sorting and preference
   */
  void deduplicate_reports(bool use_trace = false);

  /**
   * Set suppression manager for filtering suppressed issues
   */
  void setSuppressionManager(SuppressionManager *mgr) { suppressionMgr = mgr; }

  /**
   * Filter out suppressed reports
   */
  void filterSuppressed();

  /**
   * Generate JSON report file
   */
  void generate_json_report(llvm::raw_ostream &OS, int min_score = 0) const;

  /**
   * Print summary statistics to console
   */
  void print_summary(llvm::raw_ostream &OS) const;

  /**
   * Get total number of reports across all types
   */
  int get_total_reports() const;

  /**
   * Get number of registered bug types
   */
  size_t get_bug_type_count() const { return bug_types.size(); }

  /**
   * Generate SARIF report file
   */
  void generate_sarif_report(llvm::raw_ostream &OS, int min_score = 0) const;

  /**
   * Get singleton instance
   */
  static BugReportMgr &get_instance();

private:
  // Map bug type names to IDs
  llvm::StringMap<int> bug_type_names;

  // Bug type registry
  std::vector<BugType> bug_types;

  // Reports organized by bug type ID
  std::unordered_map<int, std::vector<BugReport *>> reports;

  // Source file ID management (for compact JSON representation)
  llvm::StringMap<int> src_file_ids;
  std::vector<std::string> src_files;

  // Deduplication tracking (maps hash to report)
  std::unordered_map<size_t, BugReport *> report_hashes;

  // Suppression manager (optional)
  SuppressionManager *suppressionMgr = nullptr;

  int get_src_file_id(llvm::StringRef src_file);

  // Helper to check if a report is a duplicate
  bool is_duplicate(int ty_id, const BugReport *report, bool use_trace) const;

  // Helper to get primary location from report
  struct Location {
    std::string file;
    int line;
    int column;
    bool operator<(const Location &other) const {
      if (file != other.file)
        return file < other.file;
      if (line != other.line)
        return line < other.line;
      return column < other.column;
    }
  };
  Location getPrimaryLocation(const BugReport *report) const;

  // Helper to get trace end locations (for Infer-style deduplication)
  std::vector<Location> getTraceEndLocations(const BugReport *report) const;

  // Sort reports by decreasing preference (shorter traces preferred)
  void sortByDecreasingPreference(std::vector<BugReport *> &reports) const;

  // Sort reports by location
  void sortByLocation(std::vector<BugReport *> &reports) const;
};

#endif // CHECKER_REPORT_BUGREPORTMGR_H
