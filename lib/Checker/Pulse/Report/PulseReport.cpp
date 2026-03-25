#include "Checker/Pulse/Report/PulseReport.h"

#include "Checker/Report/BugReportMgr.h"

#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

namespace pulse {

//===----------------------------------------------------------------------===//
// Reporting
//
// Converts internal Pulse diagnostics (operation failures + traces + state)
// into user-facing bug reports. For sound incorrectness, reports should
// correspond to feasible witnesses; reporting code should not "upgrade"
// uncertain conditions into definite ones.
//===----------------------------------------------------------------------===//

void DiagnosticManager::registerBugType(const std::string &type, int id) {
  std::lock_guard<std::mutex> lock(mutex_);
  bugTypeIds_[type] = id;
}

void DiagnosticManager::report(std::unique_ptr<Diagnostic> diagnostic) {
  std::lock_guard<std::mutex> lock(mutex_);

  size_t hash = diagnostic->getHash();
  auto &bucket = reports_[hash];

  for (auto &entry : bucket) {
    if (entry.representative->equals(*diagnostic)) {
      // Duplicate found
      entry.count++;
      // Here we could implement trace merging:
      // if
      // (diagnostic->getTrace().isShortThan(entry.representative->getTrace()))
      // {
      //     entry.representative = std::move(diagnostic);
      // }
      return;
    }
  }

  // New issue
  bucket.emplace_back(std::move(diagnostic));
}

void DiagnosticManager::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto &mgr = BugReportMgr::get_instance();

  for (auto &kv : reports_) {
    auto &bucket = kv.second;
    for (const auto &entry : bucket) {
      const Diagnostic *diag = entry.representative.get();
      std::string type = diag->getIssueType();

      int bugTypeId = -1;
      auto it = bugTypeIds_.find(type);
      if (it != bugTypeIds_.end()) {
        bugTypeId = it->second;
      } else {
        // Fallback: try to find by name in manager or log error
        // Assuming we can't find it, we skip or use default
        llvm::errs() << "[Pulse] Warning: Unknown bug type '" << type << "'\n";
        continue;
      }

      BugReport *report = new BugReport(bugTypeId);

      // Add trace steps
      if (const Trace *trace = diag->getTrace()) {
        for (const auto &event : trace->getEvents()) {
          if (event.location) {
            report->append_step(const_cast<llvm::Instruction *>(event.location),
                                event.description, 0, {}, "trace");
          }
        }
      }

      report->append_step(const_cast<llvm::Instruction *>(diag->getLocation()),
                          diag->getMessage() + ": " + diag->getDescription(), 0,
                          {}, "bug");

      if (!diag->getSuggestion().empty()) {
        report->append_step(
            const_cast<llvm::Instruction *>(diag->getLocation()),
            "Suggestion: " + diag->getSuggestion(), 0, {}, "suggestion");
      }

      mgr.insert_report(bugTypeId, report, true);
    }
  }

  clear();
}

void DiagnosticManager::clear() { reports_.clear(); }

void DiagnosticManager::syncWithServer() {
  std::lock_guard<std::mutex> lock(mutex_);

  // Pulse Sync Implementation (Placeholder)
  llvm::json::OStream J(llvm::outs());
  J.object([&] {
    J.attributeArray("diagnostics", [&] {
      for (auto &kv : reports_) {
        auto &bucket = kv.second;
        for (const auto &entry : bucket) {
          const Diagnostic *diag = entry.representative.get();
          J.object([&] {
            J.attribute("type", diag->getIssueType());
            J.attribute("message", diag->getMessage());
            // Add more fields...
          });
        }
      }
    });
  });
  llvm::outs() << "\n";
}

} // namespace pulse
