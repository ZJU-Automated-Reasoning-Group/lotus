#pragma once

#include "Checker/Framework/BugTypes.h"

#include <set>
#include <string>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/raw_ostream.h>

class BugReportMgr;

namespace lotus {
namespace fuzzing {

struct TargetGenerationOptions {
  int min_confidence_score = 0;
  bool include_invalid_reports = false;
};

struct SourceLocation {
  std::string file;
  unsigned line = 0;
  unsigned column = 0;

  bool isValid() const { return !file.empty() && line > 0; }
};

struct NormalizedFinding {
  std::string checker_id;
  std::string bug_class;
  BugDescription::BugImportance severity = BugDescription::BI_NA;
  SourceLocation primary_location;
  std::vector<SourceLocation> secondary_locations;
  std::string function_name;
  int confidence_score = 100;
  unsigned trace_length = 0;
  unsigned max_trace_level = 0;
  bool valid = true;
};

struct RankedTarget {
  std::string file;
  unsigned line = 0;
  unsigned column = 0;
  BugDescription::BugImportance severity = BugDescription::BI_NA;
  int confidence_score = 0;
  unsigned trace_length = 0;
  unsigned max_trace_level = 0;
  std::set<std::string> checker_ids;
  std::set<std::string> bug_classes;
};

std::vector<NormalizedFinding>
collectFindings(const BugReportMgr &mgr,
                const TargetGenerationOptions &options = {});

std::vector<RankedTarget>
collectTargets(llvm::ArrayRef<NormalizedFinding> findings);

void serializeTargets(llvm::ArrayRef<RankedTarget> targets,
                      llvm::raw_ostream &os);

bool writeTargetsToFile(llvm::ArrayRef<RankedTarget> targets,
                        llvm::StringRef output_path,
                        std::string *error_message = nullptr);

} // namespace fuzzing
} // namespace lotus
