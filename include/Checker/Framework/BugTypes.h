/** @file BugTypes.h @brief Bug type enumerations for checker framework. */
#ifndef INCLUDE_REPORT_BUGREPORT_BUGTYPES_H_
#define INCLUDE_REPORT_BUGREPORT_BUGTYPES_H_

#include <string>

namespace BugDescription {
enum BugImportance {
  BI_NA = 0x0,

  BI_LOW = 0x1,
  BI_MEDIUM = 0x2,
  BI_HIGH = 0x3,
};

enum BugClassification {
  BC_NA = 0x0,

  BC_SECURITY = 0x1,
  BC_PERFORMANCE = 0x2,
  BC_ERROR = 0x3,
  BC_WARNING = 0x4,
  BC_STYLE = 0x5,
  BC_PORTABILITY = 0x6,
};

std::string to_string(BugImportance bi);
std::string to_string(BugClassification bc);
} // namespace BugDescription

/*
 * Abbreviations for internal bug types.
 */
#define BUG_BBSF(V)                                                            \
  V("Bad Buffer Size from System Function", BugDescription::BI_HIGH,           \
    BugDescription::BC_SECURITY, "CWE-131")
#define BUG_BCNS(V)                                                            \
  V("Buffer Copy without Checking Size of Input", BugDescription::BI_HIGH,     \
    BugDescription::BC_SECURITY, "CWE-120, CWE-121, CWE-122")
#define BUG_CND(V)                                                             \
  V("Checked Null Dereference", BugDescription::BI_MEDIUM,                     \
    BugDescription::BC_ERROR, "CWE-476")
#define BUG_DBF(V)                                                             \
  V("Double Free", BugDescription::BI_HIGH, BugDescription::BC_SECURITY,       \
    "CWE-415")
#define BUG_DBZ(V)                                                             \
  V("Divide by Zero", BugDescription::BI_MEDIUM, BugDescription::BC_ERROR,     \
    "CWE-369")
#define BUG_DATARACE(V)                                                        \
  V("Data Race", BugDescription::BI_HIGH, BugDescription::BC_ERROR, "CWE-362")
#define BUG_FDL(V)                                                             \
  V("File Descriptor Leak", BugDescription::BI_HIGH,                           \
    BugDescription::BC_PERFORMANCE, "CWE-773, CWE-775")
#define BUG_FDL2(V)                                                            \
  V("File Descriptor Leak 2", BugDescription::BI_HIGH,                         \
    BugDescription::BC_PERFORMANCE, "CWE-773, CWE-775")
#define BUG_FNHM(V)                                                            \
  V("Free of Memory Not on the Heap", BugDescription::BI_HIGH,                 \
    BugDescription::BC_SECURITY, "CWE-590")
#define BUG_ICB(V)                                                             \
  V("Incorrect Calculation of Buffer Size", BugDescription::BI_HIGH,           \
    BugDescription::BC_SECURITY, "CWE-131")
#define BUG_IUSA(V)                                                            \
  V("Invalid Use of Stack Address", BugDescription::BI_HIGH,                   \
    BugDescription::BC_SECURITY, "CWE-562")
#define BUG_ML(V)                                                              \
  V("Memory Leak", BugDescription::BI_HIGH, BugDescription::BC_PERFORMANCE,    \
    "CWE-401")
#define BUG_NPD(V)                                                             \
  V("NULL Pointer Dereference", BugDescription::BI_HIGH,                       \
    BugDescription::BC_SECURITY, "CWE-476, CWE-690")
#define BUG_TAINT(V)                                                           \
  V("Taint-Style Vulnerability", BugDescription::BI_HIGH,                      \
    BugDescription::BC_SECURITY,                                               \
    "CWE-15, CWE-23, CWE-78, CWE-90, CWE-123, CWE-256, CWE-319, CWE-426, "     \
    "CWE-427, CWE-591")
#define BUG_TRP(V)                                                             \
  V("Traps", BugDescription::BI_LOW, BugDescription::BC_ERROR, "")
#define BUG_UAF(V)                                                             \
  V("Use After Free", BugDescription::BI_HIGH, BugDescription::BC_SECURITY,    \
    "CWE-416")
#define BUG_UUV(V)                                                             \
  V("Use of Uninitialized Variable", BugDescription::BI_HIGH,                  \
    BugDescription::BC_SECURITY, "CWE-457")

#define GET_FULL_NAME(abbr, importance, classification, desc)                  \
  abbr, importance, classification, desc
#define GET_ABBREV_NAME(abbr, importance, classification, desc) abbr
#define GET_DESC(abbr, importance, classification, desc) desc

#endif /* CHECKER_REPORT_BUGTYPES_H */
