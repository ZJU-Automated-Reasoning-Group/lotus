#include "Checker/Report/BugTypes.h"

namespace BugDescription {

std::string to_string(BugImportance bi) {
  switch (bi) {
  case BI_LOW:
    return "Low";
  case BI_MEDIUM:
    return "Medium";
  case BI_HIGH:
    return "High";
  case BI_NA:
    return "N/A";
  default:
    return "Unknown";
  }
}

std::string to_string(BugClassification bc) {
  switch (bc) {
  case BC_SECURITY:
    return "Security";
  case BC_PERFORMANCE:
    return "Performance";
  case BC_ERROR:
    return "Error";
  case BC_WARNING:
    return "Warning";
  case BC_STYLE:
    return "Style";
  case BC_PORTABILITY:
    return "Portability";
  case BC_NA:
    return "N/A";
  default:
    return "Unknown";
  }
}

} // namespace BugDescription
