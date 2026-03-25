#include "Checker/Report/BugReportMgr.h"

#include "Checker/Report/SARIF.h"

#include <algorithm>
#include <set>
#include <unordered_set>

#include <llvm/Support/ManagedStatic.h>

static llvm::ManagedStatic<BugReportMgr> global_bug_report_mgr;

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

  // Track hash for future deduplication
  if (deduplicate_by_trace) {
    size_t hash = report->compute_hash(deduplicate_by_trace);
    report_hashes[hash] = report;
  }

  return true;
}

void BugReportMgr::insert_report(int ty_id, BugReport *report) {
  // Default to deduplication enabled (trace-based)
  insert_report(ty_id, report, true);
}

bool BugReportMgr::is_duplicate(int ty_id, const BugReport *report,
                                bool use_trace) const {
  size_t hash = report->compute_hash(use_trace);

  // Check if we've seen this hash before
  auto it = report_hashes.find(hash);
  if (it != report_hashes.end()) {
    return true;
  }

  // Also check existing reports of the same type
  auto reports_it = reports.find(ty_id);
  if (reports_it != reports.end()) {
    for (const BugReport *existing : reports_it->second) {
      size_t existing_hash = existing->compute_hash(use_trace);
      if (existing_hash == hash) {
        return true;
      }
    }
  }

  return false;
}

BugReportMgr::Location
BugReportMgr::getPrimaryLocation(const BugReport *report) const {
  Location loc;
  const auto &steps = report->get_steps();
  if (!steps.empty()) {
    const BugDiagStep *primary = steps[0];
    loc.file = primary->src_file;
    loc.line = primary->src_line;
    loc.column = primary->src_column;
  }
  return loc;
}

std::vector<BugReportMgr::Location>
BugReportMgr::getTraceEndLocations(const BugReport *report) const {
  std::vector<Location> endLocs;
  const auto &steps = report->get_steps();

  // Get the last step (end of trace)
  if (!steps.empty()) {
    const BugDiagStep *last = steps.back();
    Location loc;
    loc.file = last->src_file;
    loc.line = last->src_line;
    loc.column = last->src_column;
    endLocs.push_back(loc);
  }

  return endLocs;
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
              if (!a->get_steps().empty()) {
                const auto *stepA = a->get_steps()[0];
                locA.file = stepA->src_file;
                locA.line = stepA->src_line;
                locA.column = stepA->src_column;
              }
              if (!b->get_steps().empty()) {
                const auto *stepB = b->get_steps()[0];
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

void BugReportMgr::deduplicate_reports(bool use_trace) {
  // Clear existing hash map
  report_hashes.clear();

  // Enhanced deduplication inspired by Infer's dedup function
  for (auto &pair : reports) {
    int ty_id = pair.first;
    std::vector<BugReport *> &report_list = pair.second;

    if (report_list.empty()) {
      continue;
    }

    // Step 1: Sort by decreasing preference (shorter traces first)
    sortByDecreasingPreference(report_list);

    // Step 2: Deduplicate using location-based tracking
    std::set<std::vector<Location>> reportedEnds; // Set of location lists
    std::vector<BugReport *> deduplicated;

    for (BugReport *report : report_list) {
      bool isDuplicate = false;

      if (use_trace) {
        // Check trace end locations
        std::vector<Location> endLocs = getTraceEndLocations(report);
        if (!endLocs.empty()) {
          // Sort locations for consistent comparison
          std::sort(endLocs.begin(), endLocs.end());

          if (reportedEnds.find(endLocs) != reportedEnds.end()) {
            isDuplicate = true;
          } else {
            reportedEnds.insert(endLocs);
          }
        }
      } else {
        // Check primary location
        Location primary = getPrimaryLocation(report);
        std::vector<Location> singleLoc = {primary};
        if (reportedEnds.find(singleLoc) != reportedEnds.end()) {
          isDuplicate = true;
        } else {
          reportedEnds.insert(singleLoc);
        }
      }

      if (!isDuplicate) {
        // Also check hash-based deduplication
        size_t hash = report->compute_hash(use_trace);
        if (report_hashes.find(hash) == report_hashes.end()) {
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

void BugReportMgr::filterSuppressed() {
  if (!suppressionMgr) {
    return;
  }

  for (auto &pair : reports) {
    int ty_id = pair.first;
    std::vector<BugReport *> &report_list = pair.second;

    if (report_list.empty()) {
      continue;
    }

    // Get bug type name
    const BugType &bugType = get_bug_type_info(ty_id);
    std::string issueType = bugType.bug_name.str();

    std::vector<BugReport *> filtered;

    for (BugReport *report : report_list) {
      Location primary = getPrimaryLocation(report);

      if (!suppressionMgr->isSuppressed(issueType, primary.file,
                                        primary.line)) {
        filtered.push_back(report);
      } else {
        // Suppressed, delete it
        delete report;
      }
    }

    report_list = std::move(filtered);
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

void BugReportMgr::generate_json_report(llvm::raw_ostream &OS,
                                        int min_score) const {
  OS << "{\n";
  OS << "  \"TotalBugs\": " << get_total_reports() << ",\n";

  // Source files array
  OS << "  \"SrcFiles\": [\n";
  for (size_t i = 0; i < src_files.size(); ++i) {
    OS << "    \"" << src_files[i] << "\"";
    if (i < src_files.size() - 1)
      OS << ",";
    OS << "\n";
  }
  OS << "  ],\n";

  // Bug types and reports
  OS << "  \"BugTypes\": [\n";
  bool first_type = true;

  for (size_t ty_id = 0; ty_id < bug_types.size(); ++ty_id) {
    const BugType &bt = bug_types[ty_id];
    const std::vector<BugReport *> *bt_reports = get_reports_for_type(ty_id);

    if (!bt_reports || bt_reports->empty()) {
      continue;
    }

    // Filter by score
    std::vector<const BugReport *> filtered;
    for (const BugReport *report : *bt_reports) {
      if (report->get_conf_score() >= min_score) {
        filtered.push_back(report);
      }
    }

    if (filtered.empty()) {
      continue;
    }

    if (!first_type)
      OS << ",\n";
    first_type = false;

    OS << "    {\n";
    OS << "      \"Name\": \"" << bt.bug_name << "\",\n";
    OS << "      \"Description\": \"" << bt.desc << "\",\n";
    OS << "      \"Importance\": \"" << BugDescription::to_string(bt.importance)
       << "\",\n";
    OS << "      \"Classification\": \""
       << BugDescription::to_string(bt.classification) << "\",\n";
    OS << "      \"TotalReports\": " << filtered.size() << ",\n";
    OS << "      \"Reports\": [\n";

    for (size_t i = 0; i < filtered.size(); ++i) {
      filtered[i]->export_json(OS);
      if (i < filtered.size() - 1) {
        OS << ",";
      }
      OS << "\n";
    }

    OS << "      ]\n";
    OS << "    }";
  }

  OS << "\n  ]\n";
  OS << "}\n";
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

int BugReportMgr::get_total_reports() const {
  int total = 0;
  for (const auto &pair : reports) {
    total += pair.second.size();
  }
  return total;
}

void BugReportMgr::generate_sarif_report(llvm::raw_ostream &OS,
                                         int min_score) const {
  sarif::SarifLog sarifLog("Lotus", "1.0.0");
  sarifLog.setToolInformationUri("https://github.com/ZJU-PL/lotus");

  // Add rules for all bug types
  for (size_t i = 0; i < bug_types.size(); ++i) {
    const BugType &bugType = bug_types[i];
    std::string helpUri =
        "https://zju-pl.github.io/lotus/docs/bugs/" + bugType.bug_name.str();
    std::string category = BugDescription::to_string(bugType.classification);

    sarif::Rule rule(bugType.bug_name.str(), bugType.bug_name.str(),
                     bugType.desc.str(), helpUri, category);
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
      if (report->get_conf_score() < min_score)
        continue;

      const auto &steps = report->get_steps();
      if (steps.empty())
        continue;

      // Create SARIF result
      std::string message = steps[0]->tip;
      if (report->get_extras() && !report->get_extras()->suggestion.empty()) {
        message += ". Suggestion: " + report->get_extras()->suggestion;
      }

      sarif::Result result(bugType.bug_name.str(), message);

      // Determine severity based on importance
      sarif::Level level = sarif::Level::Warning;
      if (bugType.importance == BugDescription::BI_HIGH) {
        level = sarif::Level::Error;
      } else if (bugType.importance == BugDescription::BI_LOW) {
        level = sarif::Level::Note;
      }
      result.level = level;

      // Add primary location
      if (!steps[0]->src_file.empty()) {
        sarif::Location primaryLoc(steps[0]->src_file, steps[0]->src_line,
                                   steps[0]->src_column);
        if (!steps[0]->func_name.empty()) {
          primaryLoc.function = steps[0]->func_name;
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
          sarif::ThreadFlowLocation tfl(tflLoc, step->tip);
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
