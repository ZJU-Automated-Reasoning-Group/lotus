#ifndef CHECKER_PULSE_PULSEREPORT_H
#define CHECKER_PULSE_PULSEREPORT_H

#include "Checker/Pulse/Report/PulseDiagnostic.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse {

/**
 * DiagnosticManager: Handles deduplication, trace merging, and reporting.
 * Implements the "PulseDiagnostic.ml" sophisticated reporting engine logic.
 */
class DiagnosticManager {
public:
  static DiagnosticManager &getInstance() {
    static DiagnosticManager instance;
    return instance;
  }

  // Register a bug type ID from BugReportMgr
  void registerBugType(const std::string &type, int id);

  // Add a diagnostic. It will be deduped against existing ones.
  void report(std::unique_ptr<Diagnostic> diagnostic);

  // Flush all diagnostics to the backend (BugReportMgr)
  void flush();

  // Clear all diagnostics (for testing or reset)
  void clear();

  // Sync with external server (pulse_sync)
  void syncWithServer();

private:
  DiagnosticManager() = default;
  ~DiagnosticManager() = default;

  // Disable copy
  DiagnosticManager(const DiagnosticManager &) = delete;
  DiagnosticManager &operator=(const DiagnosticManager &) = delete;

  struct ReportEntry {
    std::unique_ptr<Diagnostic> representative;
    int count;
    // We could store multiple traces here to merge them

    ReportEntry(std::unique_ptr<Diagnostic> d)
        : representative(std::move(d)), count(1) {}
  };

  std::map<size_t, std::vector<ReportEntry>>
      reports_; // Map hash -> entries (handle collision)
  std::unordered_map<std::string, int>
      bugTypeIds_; // Map issue type string -> BugReportMgr ID
  std::mutex mutex_;
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEREPORT_H
