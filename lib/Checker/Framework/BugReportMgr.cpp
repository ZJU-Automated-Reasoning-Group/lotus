#include "Checker/Framework/BugReportMgr.h"

#include "Checker/Framework/SARIF.h"

#include <algorithm>
#include <set>
#include <unordered_set>

#include <llvm/Support/ManagedStatic.h>

static llvm::ManagedStatic<BugReportMgr> global_bug_report_mgr;

namespace {
const BugDiagStep *findPreferredStep(const BugReport *report, bool preferLast) {
  if (!report) {
    return nullptr;
  }

  const auto &steps = report->get_steps();
  if (preferLast) {
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
      if (*it && !(*it)->src_file.empty()) {
        return *it;
      }
    }
    return steps.empty() ? nullptr : steps.back();
  }

  for (const BugDiagStep *step : steps) {
    if (step && !step->src_file.empty()) {
      return step;
    }
  }
  return steps.empty() ? nullptr : steps.front();
}

bool sameStepIdentity(const BugDiagStep *a, const BugDiagStep *b) {
  if (a == nullptr || b == nullptr) {
    return a == b;
  }

  return a->src_file == b->src_file && a->src_line == b->src_line &&
         a->src_column == b->src_column && a->tip == b->tip;
}

bool reportsEquivalent(const BugReport *a, const BugReport *b, bool useTrace) {
  if (a == nullptr || b == nullptr) {
    return a == b;
  }

  if (useTrace) {
    const auto &stepsA = a->get_steps();
    const auto &stepsB = b->get_steps();
    if (stepsA.size() != stepsB.size()) {
      return false;
    }

    for (size_t i = 0; i < stepsA.size(); ++i) {
      if (!sameStepIdentity(stepsA[i], stepsB[i])) {
        return false;
      }
    }
    return true;
  }

  return sameStepIdentity(findPreferredStep(a, true),
                          findPreferredStep(b, true));
}

bool shouldIncludeReport(const BugReport *report, int minScore,
                         bool showInvalid) {
  return report != nullptr && report->get_conf_score() >= minScore &&
         (showInvalid || report->is_valid());
}

std::string formatLocationString(const BugDiagStep *step) {
  if (step == nullptr || step->src_file.empty()) {
    return "unknown location";
  }

  std::string location = step->src_file + ":" + std::to_string(step->src_line);
  if (step->src_column > 0) {
    location += ":" + std::to_string(step->src_column);
  }
  return location;
}
} // namespace

BugReportMgr &BugReportMgr::get_instance() { return *global_bug_report_mgr; }

BugReportMgr::BugReportMgr() {}

BugReportMgr::~BugReportMgr() {
  // Clean up all reports
  for (auto &pair : reports) {
    for (BugReport *report : pair.second) {
      delete report;
    }
  }
}

int BugReportMgr::register_bug_type(
    llvm::StringRef ty_name, BugDescription::BugImportance importance,
    BugDescription::BugClassification classification, llvm::StringRef desc) {

  // Check if already registered
  int id = find_bug_type(ty_name);
  if (id != -1) {
    return id;
  }

  // Register new bug type
  id = bug_types.size();
  bug_types.push_back(BugType(id, ty_name, importance, classification, desc));
  bug_type_names[ty_name] = id;

  return id;
}

int BugReportMgr::find_bug_type(llvm::StringRef ty_name) {
  auto it = bug_type_names.find(ty_name);
  return (it == bug_type_names.end()) ? -1 : it->second;
}

const BugReportMgr::BugType &BugReportMgr::get_bug_type_info(int ty_id) const {
  assert(ty_id >= 0 && ty_id < (int)bug_types.size() && "Invalid bug type ID");
  return bug_types[ty_id];
}

void BugReportMgr::clear_reports_for_types(const std::vector<int> &ty_ids) {
  if (ty_ids.empty()) {
    return;
  }

  std::unordered_set<int> ids(ty_ids.begin(), ty_ids.end());
  for (int ty_id : ids) {
    auto it = reports.find(ty_id);
    if (it == reports.end()) {
      continue;
    }
    for (BugReport *report : it->second) {
      delete report;
    }
    reports.erase(it);
  }

  report_hashes.clear();
  for (const auto &pair : reports) {
    for (BugReport *report : pair.second) {
      if (!report) {
        continue;
      }
      report_hashes[report->compute_hash(true)] = report;
    }
  }
}

void BugReportMgr::clear_all_reports() {
  std::vector<int> ty_ids;
  ty_ids.reserve(reports.size());
  for (const auto &pair : reports) {
    ty_ids.push_back(pair.first);
  }
  clear_reports_for_types(ty_ids);
  src_file_ids.clear();
  src_files.clear();
}

bool BugReportMgr::insert_report(int ty_id, BugReport *report,
                                 bool deduplicate_by_trace) {
  assert(ty_id >= 0 && ty_id < (int)bug_types.size() && "Invalid bug type ID");

  if (deduplicate_by_trace &&
      is_duplicate(ty_id, report, deduplicate_by_trace)) {
    // Duplicate found, delete the new report
    delete report;
    return false;
  }

  reports[ty_id].push_back(report);

  for (const BugDiagStep *step : report->get_steps()) {
    if (step && !step->src_file.empty()) {
      get_src_file_id(step->src_file);
    }
  }

  // Track hash for future deduplication
  if (deduplicate_by_trace) {
    size_t hash = report->compute_hash(deduplicate_by_trace);
    report_hashes[hash] = report;
  }

  return true;
}

bool BugReportMgr::is_duplicate(int ty_id, const BugReport *report,
                                bool use_trace) const {
  size_t hash = report->compute_hash(use_trace);

  // Check if we've seen this hash before
  auto it = report_hashes.find(hash);
  if (it != report_hashes.end() && it->second->get_bug_type_id() == ty_id &&
      reportsEquivalent(it->second, report, use_trace)) {
    return true;
  }

  // Also check existing reports of the same type
  auto reports_it = reports.find(ty_id);
  if (reports_it != reports.end()) {
    for (const BugReport *existing : reports_it->second) {
      if (existing->compute_hash(use_trace) == hash &&
          reportsEquivalent(existing, report, use_trace)) {
        return true;
      }
    }
  }

  return false;
}

BugReportMgr::Location
BugReportMgr::getPrimaryLocation(const BugReport *report) const {
  Location loc;
  const BugDiagStep *primary = findPreferredStep(report, true);
  if (primary != nullptr) {
    loc.file = primary->src_file;
    loc.line = primary->src_line;
    loc.column = primary->src_column;
  }
  return loc;
}

void BugReportMgr::sortByDecreasingPreference(
    std::vector<BugReport *> &reports) const {
  // Sort by trace length (shorter = preferred), then by hash, then by full
  // comparison
  std::sort(reports.begin(), reports.end(),
            [](const BugReport *a, const BugReport *b) {
              // Prefer shorter traces
              int lenA = a->get_steps().size();
              int lenB = b->get_steps().size();
              if (lenA != lenB) {
                return lenA < lenB;
              }

              // Then by hash
              size_t hashA = a->compute_hash(true);
              size_t hashB = b->compute_hash(true);
              if (hashA != hashB) {
                return hashA < hashB;
              }

              // Finally by primary location
              Location locA, locB;
              const BugDiagStep *stepA = findPreferredStep(a, true);
              const BugDiagStep *stepB = findPreferredStep(b, true);
              if (stepA != nullptr) {
                locA.file = stepA->src_file;
                locA.line = stepA->src_line;
                locA.column = stepA->src_column;
              }
              if (stepB != nullptr) {
                locB.file = stepB->src_file;
                locB.line = stepB->src_line;
                locB.column = stepB->src_column;
              }
              return locA < locB;
            });
}

void BugReportMgr::sortByLocation(std::vector<BugReport *> &reports) const {
  std::sort(reports.begin(), reports.end(),
            [this](const BugReport *a, const BugReport *b) {
              Location locA = getPrimaryLocation(a);
              Location locB = getPrimaryLocation(b);
              return locA < locB;
            });
}

void BugReportMgr::deduplicate_reports(DedupMode mode) {
  // Clear existing hash map
  report_hashes.clear();

  // Enhanced deduplication inspired by Infer's dedup function
  for (auto &pair : reports) {
    report_hashes.clear();
    std::vector<BugReport *> &report_list = pair.second;

    if (report_list.empty()) {
      continue;
    }

    // Step 1: Sort by decreasing preference (shorter traces first)
    sortByDecreasingPreference(report_list);

    // Step 2: Deduplicate according to the explicitly requested semantics.
    std::set<Location> reportedLocations;
    std::vector<BugReport *> deduplicated;

    for (BugReport *report : report_list) {
      bool isDuplicate = false;

      if (mode != DedupMode::ExactTrace) {
        Location location = getPrimaryLocation(report);
        if (mode == DedupMode::Endpoint) {
          const auto &steps = report->get_steps();
          const BugDiagStep *endpoint = steps.empty() ? nullptr : steps.back();
          if (endpoint != nullptr) {
            location.file = endpoint->src_file;
            location.line = endpoint->src_line;
            location.column = endpoint->src_column;
          }
        }
        if (reportedLocations.find(location) != reportedLocations.end()) {
          isDuplicate = true;
        } else {
          reportedLocations.insert(location);
        }
      }

      if (!isDuplicate) {
        const bool useTrace = mode == DedupMode::ExactTrace;
        size_t hash = report->compute_hash(useTrace);
        auto existing = report_hashes.find(hash);
        if (existing == report_hashes.end() ||
            !reportsEquivalent(existing->second, report, useTrace)) {
          deduplicated.push_back(report);
          report_hashes[hash] = report;
        } else {
          isDuplicate = true;
        }
      }

      if (isDuplicate) {
        delete report;
      }
    }

    // Step 3: Sort by location for final output
    sortByLocation(deduplicated);

    report_list = std::move(deduplicated);
  }
}

void BugReportMgr::filterSuppressed(const SuppressionManager &manager) {
  for (auto &pair : reports) {
    int ty_id = pair.first;
    std::vector<BugReport *> &report_list = pair.second;

    if (report_list.empty()) {
      continue;
    }

    // Get bug type name
    const BugType &bugType = get_bug_type_info(ty_id);
    const std::string &issueType = bugType.bug_name;

    std::vector<BugReport *> filtered;

    for (BugReport *report : report_list) {
      Location primary = getPrimaryLocation(report);

      if (!manager.isSuppressed(issueType, primary.file, primary.line)) {
        filtered.push_back(report);
      } else {
        // Suppressed, delete it
        delete report;
      }
    }

    report_list = std::move(filtered);
  }

  // Suppression deletes reports, so any raw pointers cached by the
  // deduplication index must be discarded and rebuilt before the manager is
  // used again through its public API.
  report_hashes.clear();
  for (const auto &pair : reports) {
    for (BugReport *report : pair.second) {
      if (report) {
        report_hashes[report->compute_hash(true)] = report;
      }
    }
  }
}

const std::vector<BugReport *> *
BugReportMgr::get_reports_for_type(int ty_id) const {
  auto it = reports.find(ty_id);
  if (it == reports.end()) {
    return nullptr;
  }
  return &it->second;
}

int BugReportMgr::get_src_file_id(llvm::StringRef src_file) {
  auto it = src_file_ids.find(src_file);
  if (it != src_file_ids.end()) {
    return it->second;
  }

  int id = src_files.size();
  src_files.push_back(src_file.str());
  src_file_ids[src_file] = id;
  return id;
}

cJSON *BugReportMgr::toJson(const ReportFilter &filter) const {
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    return nullptr;
  }

  cJSON_AddNumberToObject(root, "TotalBugs", get_filtered_report_count(filter));

  cJSON *srcFiles = cJSON_AddArrayToObject(root, "SrcFiles");
  for (const std::string &srcFile : src_files) {
    cJSON_AddItemToArray(srcFiles, cJSON_CreateString(srcFile.c_str()));
  }

  cJSON *bugTypes = cJSON_AddArrayToObject(root, "BugTypes");

  for (size_t ty_id = 0; ty_id < bug_types.size(); ++ty_id) {
    const BugType &bt = bug_types[ty_id];
    const std::vector<BugReport *> *bt_reports = get_reports_for_type(ty_id);

    if (!bt_reports || bt_reports->empty()) {
      continue;
    }

    // Filter by score
    std::vector<const BugReport *> filtered;
    for (const BugReport *report : *bt_reports) {
      if (shouldIncludeReport(report, filter.minScore, filter.includeInvalid)) {
        filtered.push_back(report);
      }
    }

    if (filtered.empty()) {
      continue;
    }

    cJSON *bugType = cJSON_CreateObject();
    cJSON_AddItemToArray(bugTypes, bugType);
    cJSON_AddStringToObject(bugType, "Name", bt.bug_name.c_str());
    cJSON_AddStringToObject(bugType, "Description", bt.desc.c_str());
    cJSON_AddStringToObject(bugType, "Importance",
                            BugDescription::to_string(bt.importance).c_str());
    cJSON_AddStringToObject(
        bugType, "Classification",
        BugDescription::to_string(bt.classification).c_str());
    cJSON_AddNumberToObject(bugType, "TotalReports", filtered.size());

    cJSON *jsonReports = cJSON_AddArrayToObject(bugType, "Reports");
    for (const BugReport *report : filtered) {
      cJSON *jsonReport = report->toJson();
      if (jsonReport) {
        cJSON_AddItemToArray(jsonReports, jsonReport);
      }
    }
  }

  return root;
}

void BugReportMgr::generate_json_report(llvm::raw_ostream &OS,
                                        const ReportFilter &filter) const {
  cJSON *root = toJson(filter);
  if (!root) {
    return;
  }

  char *json = cJSON_Print(root);
  if (json) {
    OS << json << "\n";
    cJSON_free(json);
  }
  cJSON_Delete(root);
}

void BugReportMgr::print_summary(llvm::raw_ostream &OS) const {
  OS << "\n==================================================\n";
  OS << "               Bug Report Summary\n";
  OS << "==================================================\n\n";

  int total = 0;

  for (size_t ty_id = 0; ty_id < bug_types.size(); ++ty_id) {
    const BugType &bt = bug_types[ty_id];
    const std::vector<BugReport *> *bt_reports = get_reports_for_type(ty_id);

    if (!bt_reports || bt_reports->empty()) {
      continue;
    }

    OS << bt.bug_name << " (" << bt.desc << ")\n";
    OS << "  Total: " << bt_reports->size() << "\n\n";

    total += bt_reports->size();
  }

  OS << "==================================================\n";
  OS << "Total Bugs Found: " << total << "\n";
  OS << "==================================================\n\n";
}

void BugReportMgr::print_detailed_reports(llvm::raw_ostream &OS, bool verbose,
                                          const ReportFilter &filter) const {
  int total = 0;
  for (size_t ty_id = 0; ty_id < bug_types.size(); ++ty_id) {
    const auto *reportList = get_reports_for_type(ty_id);
    if (!reportList) {
      continue;
    }
    for (const BugReport *report : *reportList) {
      if (shouldIncludeReport(report, filter.minScore, filter.includeInvalid)) {
        ++total;
      }
    }
  }

  if (total == 0) {
    OS << "\nNo findings.\n";
    return;
  }

  OS << "\nFindings (" << total << ")\n";
  OS << "================\n";

  int index = 1;
  for (size_t ty_id = 0; ty_id < bug_types.size(); ++ty_id) {
    const auto *reportList = get_reports_for_type(ty_id);
    if (!reportList) {
      continue;
    }

    const BugType &bugType = get_bug_type_info(ty_id);
    for (const BugReport *report : *reportList) {
      if (!shouldIncludeReport(report, filter.minScore,
                               filter.includeInvalid)) {
        continue;
      }

      const BugDiagStep *primary = findPreferredStep(report, true);
      OS << "\n" << index++ << ". " << bugType.bug_name;
      if (primary != nullptr) {
        OS << "\n   Location: " << formatLocationString(primary);
        if (!primary->func_name.empty()) {
          OS << " in " << primary->func_name;
        }
      }

      std::string message = report->render_primary_message();
      if (!message.empty()) {
        OS << "\n   Message: " << message;
      }

      if (primary != nullptr && !primary->source_code.empty()) {
        OS << "\n   Source: " << primary->source_code;
      }

      const BugReportExtras *extras = report->get_extras();
      if (extras != nullptr && !extras->suggestion.empty()) {
        OS << "\n   Suggestion: " << extras->suggestion;
      }

      if (verbose && primary != nullptr && !primary->llvm_ir.empty()) {
        OS << "\n   LLVM IR: " << primary->llvm_ir;
      }

      if (verbose && extras != nullptr && !extras->metadata.empty()) {
        for (const auto &entry : extras->metadata) {
          OS << "\n   " << entry.first << ": " << entry.second;
        }
      }

      if (verbose) {
        const auto &steps = report->get_steps();
        if (steps.size() > 1) {
          OS << "\n   Trace:";
          for (const BugDiagStep *step : steps) {
            if (step == nullptr) {
              continue;
            }
            OS << "\n     - ";
            if (!step->src_file.empty()) {
              OS << formatLocationString(step) << ": ";
            }
            std::string stepMessage = report->render_step_message(*step);
            OS << (stepMessage.empty() ? step->tip : stepMessage);
          }
        }
      }

      OS << "\n";
    }
  }
}

int BugReportMgr::get_total_reports() const {
  int total = 0;
  for (const auto &pair : reports) {
    total += pair.second.size();
  }
  return total;
}

int BugReportMgr::get_filtered_report_count(const ReportFilter &filter) const {
  int total = 0;
  for (const auto &pair : reports) {
    for (const BugReport *report : pair.second) {
      if (shouldIncludeReport(report, filter.minScore, filter.includeInvalid)) {
        ++total;
      }
    }
  }
  return total;
}

void BugReportMgr::generate_sarif_report(llvm::raw_ostream &OS,
                                         const ReportFilter &filter) const {
  sarif::SarifLog sarifLog("Lotus", "1.0.0");
  sarifLog.setToolInformationUri("https://github.com/ZJU-PL/lotus");

  // Add rules for all bug types
  for (size_t i = 0; i < bug_types.size(); ++i) {
    const BugType &bugType = bug_types[i];
    std::string helpUri =
        "https://zju-pl.github.io/lotus/docs/bugs/" + bugType.bug_name;
    std::string category = BugDescription::to_string(bugType.classification);

    sarif::Rule rule(bugType.bug_name, bugType.bug_name, bugType.desc, helpUri,
                     category);
    sarifLog.addRule(rule);
  }

  // Add results
  for (size_t ty_id = 0; ty_id < bug_types.size(); ++ty_id) {
    const auto *reportList = get_reports_for_type(ty_id);
    if (!reportList || reportList->empty())
      continue;

    const BugType &bugType = get_bug_type_info(ty_id);
    std::string category = BugDescription::to_string(bugType.classification);

    for (const BugReport *report : *reportList) {
      if (!shouldIncludeReport(report, filter.minScore, filter.includeInvalid))
        continue;

      const auto &steps = report->get_steps();
      if (steps.empty())
        continue;

      // Create SARIF result
      std::string message = report->render_primary_message();
      if (message.empty()) {
        message = bugType.bug_name;
      }
      if (report->get_extras() && !report->get_extras()->suggestion.empty()) {
        message += ". Suggestion: " + report->get_extras()->suggestion;
      }

      sarif::Result result(bugType.bug_name, message);

      // Determine severity based on importance
      sarif::Level level = sarif::Level::Warning;
      if (bugType.importance == BugDescription::BI_HIGH) {
        level = sarif::Level::Error;
      } else if (bugType.importance == BugDescription::BI_LOW) {
        level = sarif::Level::Note;
      }
      result.level = level;

      // Add primary location
      const BugDiagStep *primary = findPreferredStep(report, true);
      if (primary != nullptr && !primary->src_file.empty()) {
        sarif::Location primaryLoc(primary->src_file, primary->src_line,
                                   primary->src_column);
        if (!primary->func_name.empty()) {
          primaryLoc.function = primary->func_name;
        }
        result.locations.push_back(primaryLoc);
      }

      // Add code flow (trace)
      if (steps.size() > 1) {
        sarif::CodeFlow codeFlow;
        codeFlow.message = "Execution trace leading to issue";

        for (size_t i = 0; i < steps.size(); ++i) {
          const auto *step = steps[i];
          if (step->src_file.empty())
            continue;

          sarif::Location tflLoc(step->src_file, step->src_line,
                                 step->src_column);
          if (!step->func_name.empty()) {
            tflLoc.function = step->func_name;
          }
          sarif::ThreadFlowLocation tfl(tflLoc,
                                        report->render_step_message(*step));
          tfl.nestingLevel = step->trace_level;
          tfl.executionOrder = static_cast<int>(i);
          if (!step->func_name.empty()) {
            tfl.location.function = step->func_name;
          }
          codeFlow.threadFlowLocations.push_back(tfl);
        }

        if (!codeFlow.threadFlowLocations.empty()) {
          result.codeFlows.push_back(codeFlow);
        }
      }

      // Set category
      result.category = category;

      sarifLog.addResult(result);
    }
  }

  // Write SARIF
  sarifLog.writeToStream(OS, true);
}
