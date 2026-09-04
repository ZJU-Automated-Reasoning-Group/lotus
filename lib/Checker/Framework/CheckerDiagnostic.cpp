#include "Checker/Framework/CheckerDiagnostic.h"

namespace lotus::checker {

BugReport *CheckerDiagnostic::toBugReport(int bug_type_id) const {
  auto *report = new BugReport(bug_type_id);
  if (!trace.empty()) {
    for (const auto &step : trace) {
      report->append_step(const_cast<llvm::Value *>(step.value), step.message,
                          step.trace_level);
    }
  } else {
    report->append_step(const_cast<llvm::Value *>(primary_value), message);
  }

  report->set_conf_score(confidence);
  if (!suggestion.empty()) {
    report->set_suggestion(suggestion);
  }

  report->add_metadata("checker_id", checker_id);
  report->add_metadata("severity", toString(severity));
  for (const auto &entry : metadata) {
    report->add_metadata(entry.first, entry.second);
  }
  return report;
}

} // namespace lotus::checker
