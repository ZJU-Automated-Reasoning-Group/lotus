#include "Fuzzing/TargetGeneration.h"

#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"

#include <algorithm>
#include <map>
#include <tuple>

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus {
namespace fuzzing {

namespace {

struct ReachabilityKey {
  unsigned trace_length = 0;
  unsigned max_trace_level = 0;
  int negative_confidence = 0;
  std::string function_name;

  bool operator<(const ReachabilityKey &other) const {
    return std::tie(trace_length, max_trace_level, negative_confidence,
                    function_name) <
           std::tie(other.trace_length, other.max_trace_level,
                    other.negative_confidence, other.function_name);
  }
};

static std::string canonicalizePath(StringRef path) {
  if (path.empty()) {
    return "";
  }

  SmallString<256> absolute_path(path);
  if (sys::path::is_relative(absolute_path)) {
    std::error_code ec = sys::fs::make_absolute(absolute_path);
    if (ec) {
      return "";
    }
  }

  SmallString<256> real_path;
  if (sys::fs::real_path(absolute_path, real_path)) {
    return "";
  }

  return real_path.str().str();
}

static SourceLocation canonicalizeLocation(StringRef file, int line,
                                           int column) {
  SourceLocation location;
  if (file.empty() || line <= 0) {
    return location;
  }

  location.file = canonicalizePath(file);
  if (location.file.empty()) {
    return {};
  }

  location.line = static_cast<unsigned>(line);
  location.column = column > 0 ? static_cast<unsigned>(column) : 0;
  return location;
}

static std::string getCheckerId(const BugReport &report,
                                const BugReportMgr::BugType &bug_type) {
  if (auto *extras = report.get_extras()) {
    auto it = extras->metadata.find("checker");
    if (it != extras->metadata.end() && !it->second.empty()) {
      return it->second;
    }
  }
  return bug_type.bug_name;
}

static SourceLocation chooseTargetLocation(const NormalizedFinding &finding) {
  for (auto it = finding.secondary_locations.rbegin();
       it != finding.secondary_locations.rend(); ++it) {
    if (it->isValid()) {
      return *it;
    }
  }
  return finding.primary_location;
}

static int severityRank(BugDescription::BugImportance severity) {
  return static_cast<int>(severity);
}

} // namespace

std::vector<NormalizedFinding>
collectFindings(const BugReportMgr &mgr,
                const TargetGenerationOptions &options) {
  std::vector<NormalizedFinding> findings;

  for (size_t type_id = 0; type_id < mgr.get_bug_type_count(); ++type_id) {
    const auto *reports = mgr.get_reports_for_type(static_cast<int>(type_id));
    if (!reports) {
      continue;
    }

    const auto &bug_type = mgr.get_bug_type_info(static_cast<int>(type_id));
    for (const BugReport *report : *reports) {
      if (!report) {
        continue;
      }
      if (!options.include_invalid_reports && !report->is_valid()) {
        continue;
      }
      if (report->get_conf_score() < options.min_confidence_score) {
        continue;
      }

      NormalizedFinding finding;
      finding.checker_id = getCheckerId(*report, bug_type);
      finding.bug_class = bug_type.bug_name;
      finding.severity = bug_type.importance;
      finding.confidence_score = report->get_conf_score();
      finding.trace_length = static_cast<unsigned>(report->get_steps().size());
      finding.valid = report->is_valid();

      bool primary_set = false;
      for (const BugDiagStep *step : report->get_steps()) {
        if (!step) {
          continue;
        }

        SourceLocation location = canonicalizeLocation(
            step->src_file, step->src_line, step->src_column);
        if (!location.isValid()) {
          continue;
        }

        if (finding.function_name.empty() && !step->func_name.empty()) {
          finding.function_name = step->func_name;
        }
        finding.max_trace_level = std::max(
            finding.max_trace_level, static_cast<unsigned>(step->trace_level));

        if (!primary_set) {
          finding.primary_location = location;
          primary_set = true;
        } else {
          finding.secondary_locations.push_back(location);
        }
      }

      if (!finding.primary_location.isValid()) {
        continue;
      }

      findings.push_back(std::move(finding));
    }
  }

  return findings;
}

std::vector<RankedTarget> collectTargets(ArrayRef<NormalizedFinding> findings) {
  struct Accumulator {
    RankedTarget target;
    ReachabilityKey reachability;
  };

  std::map<std::pair<std::string, unsigned>, Accumulator> merged_targets;
  for (const auto &finding : findings) {
    SourceLocation target_location = chooseTargetLocation(finding);
    if (!target_location.isValid()) {
      continue;
    }

    auto key = std::make_pair(target_location.file, target_location.line);
    ReachabilityKey reachability{finding.trace_length, finding.max_trace_level,
                                 -finding.confidence_score,
                                 finding.function_name};

    auto insert_result =
        merged_targets.insert(std::make_pair(key, Accumulator()));
    auto it = insert_result.first;
    bool inserted = insert_result.second;
    auto &entry = it->second;
    if (inserted) {
      entry.target.file = target_location.file;
      entry.target.line = target_location.line;
      entry.target.column = target_location.column;
      entry.target.severity = finding.severity;
      entry.target.confidence_score = finding.confidence_score;
      entry.target.trace_length = finding.trace_length;
      entry.target.max_trace_level = finding.max_trace_level;
      entry.reachability = reachability;
    } else {
      if (severityRank(finding.severity) >
          severityRank(entry.target.severity)) {
        entry.target.severity = finding.severity;
      }
      if (reachability < entry.reachability) {
        entry.reachability = reachability;
        entry.target.column = target_location.column;
        entry.target.trace_length = finding.trace_length;
        entry.target.max_trace_level = finding.max_trace_level;
      }
      entry.target.confidence_score =
          std::max(entry.target.confidence_score, finding.confidence_score);
    }

    if (!finding.checker_id.empty()) {
      entry.target.checker_ids.insert(finding.checker_id);
    }
    if (!finding.bug_class.empty()) {
      entry.target.bug_classes.insert(finding.bug_class);
    }
  }

  std::vector<RankedTarget> ranked_targets;
  ranked_targets.reserve(merged_targets.size());
  for (auto &entry : merged_targets) {
    ranked_targets.push_back(std::move(entry.second.target));
  }

  std::sort(ranked_targets.begin(), ranked_targets.end(),
            [](const RankedTarget &lhs, const RankedTarget &rhs) {
              if (severityRank(lhs.severity) != severityRank(rhs.severity)) {
                return severityRank(lhs.severity) > severityRank(rhs.severity);
              }
              if (lhs.trace_length != rhs.trace_length) {
                return lhs.trace_length < rhs.trace_length;
              }
              if (lhs.max_trace_level != rhs.max_trace_level) {
                return lhs.max_trace_level < rhs.max_trace_level;
              }
              if (lhs.confidence_score != rhs.confidence_score) {
                return lhs.confidence_score > rhs.confidence_score;
              }
              if (lhs.file != rhs.file) {
                return lhs.file < rhs.file;
              }
              if (lhs.line != rhs.line) {
                return lhs.line < rhs.line;
              }
              return lhs.column < rhs.column;
            });

  return ranked_targets;
}

void serializeTargets(ArrayRef<RankedTarget> targets, raw_ostream &os) {
  for (const auto &target : targets) {
    os << target.file << ":" << target.line << "\n";
  }
}

bool writeTargetsToFile(ArrayRef<RankedTarget> targets, StringRef output_path,
                        std::string *error_message) {
  std::error_code ec;
  raw_fd_ostream os(output_path, ec, sys::fs::OF_None);
  if (ec) {
    if (error_message) {
      *error_message = ec.message();
    }
    return false;
  }

  serializeTargets(targets, os);
  return true;
}

} // namespace fuzzing
} // namespace lotus
