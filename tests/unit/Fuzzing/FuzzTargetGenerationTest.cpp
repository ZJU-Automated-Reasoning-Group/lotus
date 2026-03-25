#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"
#include "Fuzzing/TargetGeneration.h"

#include <memory>
#include <string>
#include <utility>

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <gtest/gtest.h>

using namespace llvm;

namespace {

struct TempFile {
  std::string path;
  std::string canonical_path;

  explicit TempFile(StringRef suffix, StringRef contents = "") {
    SmallString<128> temp_path;
    int fd = -1;
    std::error_code ec =
        sys::fs::createTemporaryFile("lotus-fuzz", suffix, fd, temp_path);
    if (ec) {
      report_fatal_error("failed to create temporary file for fuzzing test");
    }
    path = temp_path.str().str();
    SmallString<128> real_path;
    std::error_code real_path_ec = sys::fs::real_path(path, real_path);
    if (real_path_ec) {
      report_fatal_error("failed to canonicalize temporary file for fuzzing test");
    }
    canonical_path = real_path.str().str();

    raw_fd_ostream os(fd, true);
    os << contents;
  }

  ~TempFile() {
    if (!path.empty()) {
      sys::fs::remove(path);
    }
  }
};

BugDiagStep *makeStep(StringRef file, int line, int column = 1,
                      StringRef function = "", int trace_level = 0) {
  auto *step = new BugDiagStep();
  step->src_file = file.str();
  step->src_line = line;
  step->src_column = column;
  step->func_name = function.str();
  step->trace_level = trace_level;
  return step;
}

TEST(FuzzTargetGenerationTest, CollectFindingsUsesSinkLocationAndMetadata) {
  TempFile source_a(".c", "int first_line;\n");
  TempFile source_b(".c", "int second_line;\n");

  BugReportMgr mgr;
  int bug_type = mgr.register_bug_type("Use After Free", BugDescription::BI_HIGH,
                                       BugDescription::BC_SECURITY, "CWE-416");

  auto *report = new BugReport(bug_type);
  report->set_conf_score(81);
  report->add_metadata("checker", "UseAfterFreeChecker");
  report->append_step(makeStep(source_a.path, 10, 2, "entry", 0));
  report->append_step(makeStep("", 0));
  report->append_step(makeStep(source_b.path, 42, 7, "sink", 2));
  ASSERT_TRUE(mgr.insert_report(bug_type, report, false));

  auto findings = lotus::fuzzing::collectFindings(mgr);
  ASSERT_EQ(findings.size(), 1u);

  const auto &finding = findings.front();
  EXPECT_EQ(finding.checker_id, "UseAfterFreeChecker");
  EXPECT_EQ(finding.bug_class, "Use After Free");
  EXPECT_EQ(finding.primary_location.file, source_a.canonical_path);
  EXPECT_EQ(finding.primary_location.line, 10u);
  ASSERT_EQ(finding.secondary_locations.size(), 1u);
  EXPECT_EQ(finding.secondary_locations.back().file, source_b.canonical_path);
  EXPECT_EQ(finding.secondary_locations.back().line, 42u);
  EXPECT_EQ(finding.function_name, "entry");
  EXPECT_EQ(finding.trace_length, 3u);
  EXPECT_EQ(finding.max_trace_level, 2u);
}

TEST(FuzzTargetGenerationTest, CollectFindingsSkipsUnresolvableLocations) {
  BugReportMgr mgr;
  int bug_type = mgr.register_bug_type(
      "NULL Pointer Dereference", BugDescription::BI_HIGH,
      BugDescription::BC_SECURITY, "CWE-476");

  auto *report = new BugReport(bug_type);
  report->append_step(makeStep("/path/that/does/not/exist.c", 12));
  ASSERT_TRUE(mgr.insert_report(bug_type, report, false));

  auto findings = lotus::fuzzing::collectFindings(mgr);
  EXPECT_TRUE(findings.empty());
}

TEST(FuzzTargetGenerationTest, CollectTargetsRanksAndDeduplicatesTargets) {
  TempFile source_a(".c", "int a;\n");
  TempFile source_b(".c", "int b;\n");
  TempFile source_c(".c", "int c;\n");

  lotus::fuzzing::NormalizedFinding duplicated_a;
  duplicated_a.checker_id = "NullPointerChecker";
  duplicated_a.bug_class = "NULL Pointer Dereference";
  duplicated_a.severity = BugDescription::BI_HIGH;
  duplicated_a.primary_location = {source_a.path, 10, 1};
  duplicated_a.secondary_locations = {{source_a.path, 20, 1}};
  duplicated_a.confidence_score = 70;
  duplicated_a.trace_length = 4;
  duplicated_a.max_trace_level = 2;
  duplicated_a.function_name = "foo";

  lotus::fuzzing::NormalizedFinding duplicate_b = duplicated_a;
  duplicate_b.checker_id = "UseAfterFreeChecker";
  duplicate_b.bug_class = "Use After Free";
  duplicate_b.confidence_score = 95;
  duplicate_b.trace_length = 2;
  duplicate_b.max_trace_level = 1;

  lotus::fuzzing::NormalizedFinding high_short;
  high_short.checker_id = "FreeOfNonHeapMemoryChecker";
  high_short.bug_class = "Free of Memory Not on the Heap";
  high_short.severity = BugDescription::BI_HIGH;
  high_short.primary_location = {source_c.path, 30, 1};
  high_short.confidence_score = 80;
  high_short.trace_length = 1;
  high_short.max_trace_level = 0;
  high_short.function_name = "bar";

  lotus::fuzzing::NormalizedFinding medium;
  medium.checker_id = "KintChecker";
  medium.bug_class = "Divide by Zero";
  medium.severity = BugDescription::BI_MEDIUM;
  medium.primary_location = {source_b.path, 5, 1};
  medium.confidence_score = 90;
  medium.trace_length = 1;
  medium.max_trace_level = 0;
  medium.function_name = "baz";

  std::vector<lotus::fuzzing::NormalizedFinding> findings = {
      duplicated_a, duplicate_b, high_short, medium};
  auto targets = lotus::fuzzing::collectTargets(findings);
  ASSERT_EQ(targets.size(), 3u);

  EXPECT_EQ(targets[0].file, source_c.path);
  EXPECT_EQ(targets[0].line, 30u);

  EXPECT_EQ(targets[1].file, source_a.path);
  EXPECT_EQ(targets[1].line, 20u);
  EXPECT_EQ(targets[1].confidence_score, 95);
  EXPECT_EQ(targets[1].trace_length, 2u);
  EXPECT_EQ(targets[1].max_trace_level, 1u);
  EXPECT_EQ(targets[1].checker_ids.size(), 2u);
  EXPECT_TRUE(targets[1].checker_ids.count("NullPointerChecker"));
  EXPECT_TRUE(targets[1].checker_ids.count("UseAfterFreeChecker"));
  EXPECT_EQ(targets[1].bug_classes.size(), 2u);

  EXPECT_EQ(targets[2].file, source_b.path);
  EXPECT_EQ(targets[2].line, 5u);
}

TEST(FuzzTargetGenerationTest, WriteTargetsToFileUsesCanonicalFileLineFormat) {
  TempFile source(".c", "int target;\n");
  TempFile output(".txt");

  lotus::fuzzing::RankedTarget target;
  target.file = source.path;
  target.line = 17;

  std::string error_message;
  ASSERT_TRUE(lotus::fuzzing::writeTargetsToFile({target}, output.path,
                                                 &error_message))
      << error_message;

  ErrorOr<std::unique_ptr<MemoryBuffer>> buffer =
      MemoryBuffer::getFile(output.path);
  ASSERT_TRUE(static_cast<bool>(buffer));
  EXPECT_EQ(buffer.get()->getBuffer(), source.path + ":17\n");
}

} // namespace
