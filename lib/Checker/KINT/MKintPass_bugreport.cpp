// Implementation of bug reporting methods for MKintPass
// This file adds shared bug reporting integration to BugReportMgr

#include "Checker/KINT/Log.h"
#include "Checker/KINT/MKintPass.h"
#include "Checker/KINT/Options.h"

namespace kint {

void MKintPass::reportBugsToManager() {
  const auto &bug_paths = m_bug_detection->getBugPaths();

  MKINT_LOG() << "Reporting " << bug_paths.size() << " bugs to BugReportMgr";

  // Report bugs that have path information. Track which instructions have
  // already been reported to avoid duplicates with the raw sets below.
  std::set<BugKey> reported;
  for (const auto &pair : bug_paths) {
    const BugKey &key = pair.first;
    const Instruction *inst = key.first;
    const BugPath &bug_path = pair.second;
    reportBug(bug_path.bugType, inst, bug_path.path);
    reported.insert(key);
  }

  // Report bugs from raw sets only if they were not already reported above.
  for (const auto *inst : m_overflow_insts) {
    if (!reported.count(BugKey(inst, interr::INT_OVERFLOW)))
      reportBug(interr::INT_OVERFLOW, inst);
  }

  for (const auto *inst : m_div_zero_insts) {
    if (!reported.count(BugKey(inst, interr::DIV_BY_ZERO)))
      reportBug(interr::DIV_BY_ZERO, inst);
  }

  for (const auto *inst : m_bad_shift_insts) {
    if (!reported.count(BugKey(inst, interr::BAD_SHIFT)))
      reportBug(interr::BAD_SHIFT, inst);
  }

  for (const auto *gep : m_gep_oob) {
    if (!reported.count(BugKey(gep, interr::ARRAY_OOB)))
      reportBug(interr::ARRAY_OOB, gep);
  }

  for (const auto &pair : m_impossible_branches) {
    const ICmpInst *cmp = pair.first;
    bool is_true_branch = pair.second;
    interr type =
        is_true_branch ? interr::DEAD_TRUE_BR : interr::DEAD_FALSE_BR;
    if (!reported.count(BugKey(cmp, type))) {
      reportBug(type, cmp);
    }
  }
}

void MKintPass::reportBug(interr bug_type, const Instruction *inst,
                          const std::vector<PathPoint> &path) {
  if (bug_type == interr::NONE)
    return;

  // Determine bug type ID and description
  int bug_type_id;
  std::string main_desc;

  switch (bug_type) {
  case interr::INT_OVERFLOW:
    if (!CheckIntOverflow)
      return;
    bug_type_id = m_intOverflowTypeId;
    main_desc = "Integer overflow detected";
    break;
  case interr::DIV_BY_ZERO:
    if (!CheckDivByZero)
      return;
    bug_type_id = m_divByZeroTypeId;
    main_desc = "Division by zero detected";
    break;
  case interr::BAD_SHIFT:
    if (!CheckBadShift)
      return;
    bug_type_id = m_badShiftTypeId;
    main_desc = "Invalid shift amount detected";
    break;
  case interr::ARRAY_OOB:
    if (!CheckArrayOOB)
      return;
    bug_type_id = m_arrayOOBTypeId;
    main_desc = "Array out of bounds access detected";
    break;
  case interr::DEAD_TRUE_BR:
  case interr::DEAD_FALSE_BR:
    if (!CheckDeadBranch)
      return;
    bug_type_id = m_deadBranchTypeId;
    main_desc = (bug_type == interr::DEAD_TRUE_BR)
                    ? "Dead true branch detected"
                    : "Dead false branch detected";
    break;
  default:
    return;
  }

  // Create bug report
  BugReport *report = new BugReport(bug_type_id);
  int trace_level = 0;

  // Add path trace if available
  if (!path.empty()) {
    for (const auto &point : path) {
      if (point.inst) {
        std::vector<NodeTag> tags;
        if (isa<CallInst>(point.inst)) {
          tags.push_back(NodeTag::CALL_SITE);
          trace_level++;
        } else if (isa<ReturnInst>(point.inst)) {
          tags.push_back(NodeTag::RETURN_SITE);
        }
        report->append_step(const_cast<Instruction *>(point.inst),
                            point.description, trace_level, tags, "path");
      }
    }
  }

  // Add the bug instruction as the final step
  std::vector<NodeTag> bug_tags;
  if (isa<CallInst>(inst)) {
    bug_tags.push_back(NodeTag::CALL_SITE);
  }
  report->append_step(const_cast<Instruction *>(inst), main_desc, trace_level,
                      bug_tags, "bug");

  // Set confidence score (SMT-based results are high confidence)
  report->set_conf_score(85);

  // Add suggestions based on bug type
  switch (bug_type) {
  case interr::INT_OVERFLOW:
    report->set_suggestion(
        "Check for integer overflow before arithmetic operations");
    break;
  case interr::DIV_BY_ZERO:
    report->set_suggestion("Add a check to ensure divisor is not zero");
    break;
  case interr::BAD_SHIFT:
    report->set_suggestion("Ensure shift amount is within valid range");
    break;
  case interr::ARRAY_OOB:
    report->set_suggestion("Add bounds checking before array access");
    break;
  case interr::DEAD_TRUE_BR:
  case interr::DEAD_FALSE_BR:
    report->set_suggestion(
        "Review the condition logic - this branch may be unreachable");
    break;
  default:
    break;
  }

  report->add_metadata("checker", "MKintPass");
  report->add_metadata("analysis", "SMT-based");

  // Report to manager
  BugReportMgr::get_instance().insert_report(bug_type_id, report, true);
}

} // namespace kint
