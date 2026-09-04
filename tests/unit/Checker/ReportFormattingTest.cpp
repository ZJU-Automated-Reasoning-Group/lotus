#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/SuppressionManager.h"

#include <string>
#include <vector>

#include <llvm/Support/raw_ostream.h>
#include <gtest/gtest.h>

namespace {

BugDiagStep *makeStep(const std::string &file, int line, const std::string &tip,
                      const std::string &func = "",
                      const std::vector<NodeTag> &tags = {},
                      const std::string &access = "") {
  auto *step = new BugDiagStep();
  step->src_file = file;
  step->src_line = line;
  step->tip = tip;
  step->func_name = func;
  step->node_tags = tags;
  step->access = access;
  return step;
}

TEST(ReportFormattingTest, JsonExportIncludesNarrativeField) {
  BugReportMgr mgr;
  int bugType = mgr.register_bug_type("Use After Free", BugDescription::BI_HIGH,
                                      BugDescription::BC_SECURITY, "CWE-416");

  auto *report = new BugReport(bugType);
  report->append_step(makeStep("main.c", 10, "Pointer escapes into callee",
                               "foo", {NodeTag::CALL_SITE}, "path"));
  report->append_step(
      makeStep("main.c", 18, "Dereference after free", "foo", {}, "memory"));

  ASSERT_TRUE(mgr.insert_report(bugType, report, false));

  std::string json;
  llvm::raw_string_ostream os(json);
  mgr.generate_json_report(os, {});
  os.flush();

  cJSON *root = cJSON_Parse(json.c_str());
  ASSERT_NE(root, nullptr) << json;
  cJSON *bugTypes = cJSON_GetObjectItem(root, "BugTypes");
  cJSON *reports =
      cJSON_GetObjectItem(cJSON_GetArrayItem(bugTypes, 0), "Reports");
  cJSON *steps =
      cJSON_GetObjectItem(cJSON_GetArrayItem(reports, 0), "DiagSteps");
  EXPECT_STREQ(cJSON_GetObjectItem(cJSON_GetArrayItem(steps, 0), "Narrative")
                   ->valuestring,
               "Enter function foo. Access path. Pointer escapes into callee");
  EXPECT_STREQ(cJSON_GetObjectItem(cJSON_GetArrayItem(steps, 1), "Narrative")
                   ->valuestring,
               "Access memory. Dereference after free");
  cJSON_Delete(root);
}

TEST(ReportFormattingTest, JsonExportUsesCJsonForAllEscapingAndStructure) {
  BugReportMgr mgr;
  int bugType = mgr.register_bug_type("Quoted \"Bug\"", BugDescription::BI_HIGH,
                                      BugDescription::BC_SECURITY,
                                      "line one\nline two\\tail");

  auto *report = new BugReport(bugType);
  report->set_suggestion("replace \"value\"\nnext");
  report->add_metadata("key\"with\ncontrol", "value\\with\ttab");
  auto *step = makeStep("dir/quoted\"file.c", 12, "tip\nwith \"quotes\"");
  step->source_code = "char *s = \"text\";";
  report->append_step(step);
  ASSERT_TRUE(mgr.insert_report(bugType, report, false));

  std::string json;
  llvm::raw_string_ostream os(json);
  mgr.generate_json_report(os, {});
  os.flush();

  cJSON *root = cJSON_Parse(json.c_str());
  ASSERT_NE(root, nullptr) << json;
  EXPECT_TRUE(cJSON_IsObject(root));
  EXPECT_EQ(cJSON_GetObjectItem(root, "TotalBugs")->valueint, 1);

  cJSON *srcFiles = cJSON_GetObjectItem(root, "SrcFiles");
  ASSERT_TRUE(cJSON_IsArray(srcFiles));
  ASSERT_EQ(cJSON_GetArraySize(srcFiles), 1);
  EXPECT_STREQ(cJSON_GetArrayItem(srcFiles, 0)->valuestring,
               "dir/quoted\"file.c");

  cJSON *bugTypes = cJSON_GetObjectItem(root, "BugTypes");
  ASSERT_TRUE(cJSON_IsArray(bugTypes));
  cJSON *jsonBugType = cJSON_GetArrayItem(bugTypes, 0);
  EXPECT_STREQ(cJSON_GetObjectItem(jsonBugType, "Name")->valuestring,
               "Quoted \"Bug\"");
  EXPECT_STREQ(cJSON_GetObjectItem(jsonBugType, "Description")->valuestring,
               "line one\nline two\\tail");

  cJSON *reports = cJSON_GetObjectItem(jsonBugType, "Reports");
  cJSON *jsonReport = cJSON_GetArrayItem(reports, 0);
  EXPECT_TRUE(cJSON_IsBool(cJSON_GetObjectItem(jsonReport, "Dominated")));
  EXPECT_TRUE(cJSON_IsBool(cJSON_GetObjectItem(jsonReport, "Valid")));
  EXPECT_STREQ(cJSON_GetObjectItem(jsonReport, "Suggestion")->valuestring,
               "replace \"value\"\nnext");

  cJSON *metadata = cJSON_GetObjectItem(jsonReport, "Metadata");
  EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(metadata, "key\"with\ncontrol")
                   ->valuestring,
               "value\\with\ttab");

  cJSON *steps = cJSON_GetObjectItem(jsonReport, "DiagSteps");
  cJSON *jsonStep = cJSON_GetArrayItem(steps, 0);
  EXPECT_STREQ(cJSON_GetObjectItem(jsonStep, "File")->valuestring,
               "dir/quoted\"file.c");
  EXPECT_STREQ(cJSON_GetObjectItem(jsonStep, "Tip")->valuestring,
               "tip\nwith \"quotes\"");
  EXPECT_STREQ(cJSON_GetObjectItem(jsonStep, "SourceCode")->valuestring,
               "char *s = \"text\";");

  cJSON_Delete(root);
}

TEST(ReportFormattingTest, SarifUsesRenderedNarrativeMessages) {
  BugReportMgr mgr;
  int bugType =
      mgr.register_bug_type("Null Pointer Dereference", BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-476");

  auto *report = new BugReport(bugType);
  report->append_step(makeStep("sample.c", 4, "Pointer may be null", "callee",
                               {NodeTag::CALL_SITE}, "argument"));
  report->append_step(makeStep("sink.c", 9, "Null pointer dereference",
                               "callee", {NodeTag::RETURN_SITE}, "result"));

  ASSERT_TRUE(mgr.insert_report(bugType, report, false));

  std::string sarif;
  llvm::raw_string_ostream os(sarif);
  mgr.generate_sarif_report(os, {});
  os.flush();

  EXPECT_NE(sarif.find("Return from function callee. Access result. Null "
                       "pointer dereference"),
            std::string::npos);
  EXPECT_NE(sarif.find("Enter function callee. Access argument. Pointer may be "
                       "null"),
            std::string::npos);
  const size_t locations = sarif.find("\"locations\"");
  ASSERT_NE(locations, std::string::npos);
  EXPECT_LT(sarif.find("sink.c", locations),
            sarif.find("\"codeFlows\"", locations));
}

TEST(ReportFormattingTest, JsonTotalsUseTheSameFilterAsReports) {
  BugReportMgr mgr;
  int bugType = mgr.register_bug_type("Filtered Bug");

  auto *included = new BugReport(bugType);
  included->set_conf_score(90);
  included->append_step(makeStep("filter.c", 10, "included"));
  ASSERT_TRUE(mgr.insert_report(bugType, included, false));

  auto *lowScore = new BugReport(bugType);
  lowScore->set_conf_score(20);
  lowScore->append_step(makeStep("filter.c", 20, "low score"));
  ASSERT_TRUE(mgr.insert_report(bugType, lowScore, false));

  auto *invalid = new BugReport(bugType);
  invalid->set_conf_score(100);
  invalid->set_valid(false);
  invalid->append_step(makeStep("filter.c", 30, "invalid"));
  ASSERT_TRUE(mgr.insert_report(bugType, invalid, false));

  std::string json;
  llvm::raw_string_ostream os(json);
  mgr.generate_json_report(os, BugReportMgr::ReportFilter{80, false});
  os.flush();

  cJSON *root = cJSON_Parse(json.c_str());
  ASSERT_NE(root, nullptr) << json;
  EXPECT_EQ(cJSON_GetObjectItem(root, "TotalBugs")->valueint, 1);
  cJSON *bugTypes = cJSON_GetObjectItem(root, "BugTypes");
  ASSERT_EQ(cJSON_GetArraySize(bugTypes), 1);
  cJSON *jsonBugType = cJSON_GetArrayItem(bugTypes, 0);
  EXPECT_EQ(cJSON_GetObjectItem(jsonBugType, "TotalReports")->valueint, 1);
  cJSON *reports = cJSON_GetObjectItem(jsonBugType, "Reports");
  ASSERT_EQ(cJSON_GetArraySize(reports), 1);
  cJSON *steps =
      cJSON_GetObjectItem(cJSON_GetArrayItem(reports, 0), "DiagSteps");
  EXPECT_STREQ(
      cJSON_GetObjectItem(cJSON_GetArrayItem(steps, 0), "Tip")->valuestring,
      "included");
  cJSON_Delete(root);
}

TEST(ReportFormattingTest, ExactTraceDedupKeepsDistinctPathsToSameEndpoint) {
  BugReportMgr mgr;
  int bugType = mgr.register_bug_type("Path Bug");

  auto *first = new BugReport(bugType);
  first->append_step(makeStep("path.c", 1, "first source"));
  first->append_step(makeStep("path.c", 20, "shared sink"));
  ASSERT_TRUE(mgr.insert_report(bugType, first, false));

  auto *second = new BugReport(bugType);
  second->append_step(makeStep("path.c", 2, "second source"));
  second->append_step(makeStep("path.c", 20, "shared sink"));
  ASSERT_TRUE(mgr.insert_report(bugType, second, false));

  mgr.deduplicate_reports(BugReportMgr::DedupMode::ExactTrace);
  EXPECT_EQ(mgr.get_total_reports(), 2);
}

TEST(ReportFormattingTest, EndpointDedupCanExplicitlyGroupSharedSinks) {
  BugReportMgr mgr;
  int bugType = mgr.register_bug_type("Grouped Bug");

  auto *first = new BugReport(bugType);
  first->append_step(makeStep("group.c", 1, "first source"));
  first->append_step(makeStep("group.c", 20, "shared sink"));
  ASSERT_TRUE(mgr.insert_report(bugType, first, false));

  auto *second = new BugReport(bugType);
  second->append_step(makeStep("group.c", 2, "second source"));
  second->append_step(makeStep("group.c", 20, "shared sink"));
  ASSERT_TRUE(mgr.insert_report(bugType, second, false));

  mgr.deduplicate_reports(BugReportMgr::DedupMode::Endpoint);
  EXPECT_EQ(mgr.get_total_reports(), 1);
}

TEST(ReportFormattingTest, DedupNeverMergesDifferentBugTypes) {
  BugReportMgr mgr;
  int firstType = mgr.register_bug_type("First Bug");
  int secondType = mgr.register_bug_type("Second Bug");

  auto *first = new BugReport(firstType);
  first->append_step(makeStep("types.c", 10, "same trace"));
  ASSERT_TRUE(mgr.insert_report(firstType, first, false));

  auto *second = new BugReport(secondType);
  second->append_step(makeStep("types.c", 10, "same trace"));
  ASSERT_TRUE(mgr.insert_report(secondType, second, false));

  mgr.deduplicate_reports(BugReportMgr::DedupMode::ExactTrace);
  EXPECT_EQ(mgr.get_total_reports(), 2);
}

TEST(ReportFormattingTest, BugTypeRegistryOwnsDynamicStrings) {
  BugReportMgr mgr;
  std::string name = "Dynamically Registered Bug";
  std::string description = "dynamic description";
  int bugType = mgr.register_bug_type(name, BugDescription::BI_MEDIUM,
                                      BugDescription::BC_SECURITY, description);

  name.assign(256, 'x');
  description.assign(256, 'y');

  const auto &registered = mgr.get_bug_type_info(bugType);
  EXPECT_EQ(registered.bug_name, "Dynamically Registered Bug");
  EXPECT_EQ(registered.desc, "dynamic description");
}

TEST(ReportFormattingTest, SuppressionRebuildsDeduplicationIndex) {
  BugReportMgr mgr;
  int bugType = mgr.register_bug_type("Suppressible Bug");

  auto *suppressed = new BugReport(bugType);
  suppressed->append_step(makeStep("suppressed.c", 10, "same finding"));
  ASSERT_TRUE(mgr.insert_report(bugType, suppressed, true));

  SuppressionManager suppressions;
  suppressions.addFileSuppression("Suppressible Bug", "suppressed.c");
  mgr.filterSuppressed(suppressions);
  EXPECT_EQ(mgr.get_total_reports(), 0);

  auto *replacement = new BugReport(bugType);
  replacement->append_step(makeStep("suppressed.c", 10, "same finding"));
  EXPECT_TRUE(mgr.insert_report(bugType, replacement, true));
  EXPECT_EQ(mgr.get_total_reports(), 1);
}

} // namespace
