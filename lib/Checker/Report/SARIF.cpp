#include "Checker/Report/SARIF.h"

#include "Utils/LLVM/Demangle.h"

#include <fstream>
#include <map>

#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

namespace sarif {

namespace {
// Helper function to convert Level to string
std::string levelToString(Level level) {
  switch (level) {
  case Level::Note:
    return "note";
  case Level::Warning:
    return "warning";
  case Level::Error:
    return "error";
  default:
    return "warning";
  }
}
} // namespace

// Location implementation
cJSON *Location::toJson() const {
  cJSON *location = cJSON_CreateObject();

  if (!file.empty()) {
    cJSON *artifactLocation = cJSON_CreateObject();
    cJSON_AddStringToObject(artifactLocation, "uri", file.c_str());
    cJSON_AddItemToObject(location, "artifactLocation", artifactLocation);
  }

  if (line > 0) {
    cJSON *region = cJSON_CreateObject();
    cJSON_AddNumberToObject(region, "startLine", line);
    if (column > 0) {
      cJSON_AddNumberToObject(region, "startColumn", column);
    }
    if (!snippet.empty()) {
      cJSON *snippetObj = cJSON_CreateObject();
      cJSON_AddStringToObject(snippetObj, "text", snippet.c_str());
      cJSON_AddItemToObject(region, "snippet", snippetObj);
    }
    cJSON_AddItemToObject(location, "region", region);
  }

  if (!function.empty()) {
    cJSON *logicalLocation = cJSON_CreateObject();
    cJSON_AddStringToObject(logicalLocation, "name", function.c_str());
    cJSON_AddStringToObject(logicalLocation, "kind", "function");
    cJSON_AddItemToObject(location, "logicalLocation", logicalLocation);
  }

  return location;
}

cJSON *Location::toThreadFlowLocationJson() const {
  cJSON *physicalLocation = cJSON_CreateObject();

  if (!file.empty()) {
    cJSON *artifactLocation = cJSON_CreateObject();
    cJSON_AddStringToObject(artifactLocation, "uri", file.c_str());
    cJSON_AddItemToObject(physicalLocation, "artifactLocation",
                          artifactLocation);
  }

  if (line > 0) {
    cJSON *region = cJSON_CreateObject();
    cJSON_AddNumberToObject(region, "startLine", line);
    if (column > 0) {
      cJSON_AddNumberToObject(region, "startColumn", column);
    }
    cJSON_AddItemToObject(physicalLocation, "region", region);
  }

  return physicalLocation;
}

// ThreadFlowLocation implementation
cJSON *ThreadFlowLocation::toJson() const {
  cJSON *tfl = cJSON_CreateObject();

  cJSON_AddItemToObject(tfl, "location", location.toThreadFlowLocationJson());

  if (!message.empty()) {
    cJSON *msgObj = cJSON_CreateObject();
    cJSON_AddStringToObject(msgObj, "text", message.c_str());
    cJSON_AddItemToObject(tfl, "message", msgObj);
  }

  if (nestingLevel > 0) {
    cJSON_AddNumberToObject(tfl, "nestingLevel", nestingLevel);
  }

  if (executionOrder > 0) {
    cJSON_AddNumberToObject(tfl, "executionOrder", executionOrder);
  }

  return tfl;
}

// CodeFlow implementation
cJSON *CodeFlow::toJson() const {
  cJSON *codeFlow = cJSON_CreateObject();

  if (!message.empty()) {
    cJSON *msgObj = cJSON_CreateObject();
    cJSON_AddStringToObject(msgObj, "text", message.c_str());
    cJSON_AddItemToObject(codeFlow, "message", msgObj);
  }

  if (!threadFlowLocations.empty()) {
    cJSON *threadFlows = cJSON_CreateArray();
    cJSON *threadFlow = cJSON_CreateObject();
    cJSON *locations = cJSON_CreateArray();

    for (const auto &tfl : threadFlowLocations) {
      cJSON_AddItemToArray(locations, tfl.toJson());
    }

    cJSON_AddItemToObject(threadFlow, "locations", locations);
    cJSON_AddItemToArray(threadFlows, threadFlow);
    cJSON_AddItemToObject(codeFlow, "threadFlows", threadFlows);
  }

  return codeFlow;
}

// Enhanced Result implementation
cJSON *Result::toJson() const {
  cJSON *result = cJSON_CreateObject();

  if (!ruleId.empty()) {
    cJSON_AddStringToObject(result, "ruleId", ruleId.c_str());
  }

  cJSON *messageObj = cJSON_CreateObject();
  cJSON_AddStringToObject(messageObj, "text", message.c_str());
  cJSON_AddItemToObject(result, "message", messageObj);

  if (level != Level::Warning) {
    cJSON_AddStringToObject(result, "level", levelToString(level).c_str());
  }

  if (!locations.empty()) {
    cJSON *locationsArray = cJSON_CreateArray();
    for (const auto &location : locations) {
      cJSON_AddItemToArray(locationsArray, location.toJson());
    }
    cJSON_AddItemToObject(result, "locations", locationsArray);
  }

  if (!relatedLocations.empty()) {
    cJSON *relatedLocationsArray = cJSON_CreateArray();
    for (const auto &location : relatedLocations) {
      cJSON_AddItemToArray(relatedLocationsArray, location.toJson());
    }
    cJSON_AddItemToObject(result, "relatedLocations", relatedLocationsArray);
  }

  if (!codeFlows.empty()) {
    cJSON *codeFlowsArray = cJSON_CreateArray();
    for (const auto &codeFlow : codeFlows) {
      cJSON_AddItemToArray(codeFlowsArray, codeFlow.toJson());
    }
    cJSON_AddItemToObject(result, "codeFlows", codeFlowsArray);
  }

  // Enhanced fields
  if (!fingerprint.empty()) {
    cJSON_AddStringToObject(result, "fingerprints", fingerprint.c_str());
  }

  if (!suppressionState.empty()) {
    cJSON *suppressionsArray = cJSON_CreateArray();
    cJSON *suppressionStateObj = cJSON_CreateObject();
    cJSON_AddStringToObject(suppressionStateObj, "kind",
                            suppressionState.c_str());
    cJSON_AddItemToArray(suppressionsArray, suppressionStateObj);
    cJSON_AddItemToObject(result, "suppressions", suppressionsArray);
  }

  // Properties for additional metadata
  cJSON *properties = cJSON_CreateObject();
  if (!category.empty()) {
    cJSON_AddStringToObject(properties, "category", category.c_str());
  }
  if (!censoredReason.empty()) {
    cJSON_AddStringToObject(properties, "censoredReason",
                            censoredReason.c_str());
  }

  if (!category.empty() || !censoredReason.empty()) {
    cJSON_AddItemToObject(result, "properties", properties);
  } else {
    cJSON_Delete(properties);
  }

  return result;
}

// Enhanced Rule implementation
cJSON *Rule::toJson() const {
  cJSON *rule = cJSON_CreateObject();

  if (!id.empty()) {
    cJSON_AddStringToObject(rule, "id", id.c_str());
  }

  if (!name.empty()) {
    cJSON_AddStringToObject(rule, "name", name.c_str());
  }

  // Short description (required by SARIF)
  if (!shortDescription.empty()) {
    cJSON *shortDesc = cJSON_CreateObject();
    cJSON_AddStringToObject(shortDesc, "text", shortDescription.c_str());
    cJSON_AddItemToObject(rule, "shortDescription", shortDesc);
  } else if (!description.empty()) {
    // Fallback to full description
    cJSON *shortDesc = cJSON_CreateObject();
    cJSON_AddStringToObject(shortDesc, "text", description.c_str());
    cJSON_AddItemToObject(rule, "shortDescription", shortDesc);
  }

  // Full description (optional but recommended)
  if (!description.empty() && description != shortDescription) {
    cJSON *fullDesc = cJSON_CreateObject();
    cJSON_AddStringToObject(fullDesc, "text", description.c_str());
    cJSON_AddItemToObject(rule, "fullDescription", fullDesc);
  }

  // Help URI (link to documentation)
  if (!helpUri.empty()) {
    cJSON_AddStringToObject(rule, "helpUri", helpUri.c_str());
  }

  // Properties for additional metadata
  cJSON *properties = cJSON_CreateObject();
  bool hasProperties = false;
  if (!category.empty()) {
    cJSON_AddStringToObject(properties, "category", category.c_str());
    hasProperties = true;
  }
  if (!severity.empty()) {
    cJSON_AddStringToObject(properties, "defaultSeverity", severity.c_str());
    hasProperties = true;
  }

  if (hasProperties) {
    cJSON_AddItemToObject(rule, "properties", properties);
  } else {
    cJSON_Delete(properties);
  }

  return rule;
}

// SarifLog implementation
SarifLog::SarifLog(const std::string &toolName, const std::string &version)
    : toolName(toolName), toolVersion(version), toolInformationUri("") {}

void SarifLog::addRule(const Rule &rule) { rules.push_back(rule); }

void SarifLog::addResult(const Result &result) { results.push_back(result); }

std::string SarifLog::toJsonString(bool pretty) const {
  cJSON *doc = toJsonDocument();

  char *jsonStr = nullptr;
  if (pretty) {
    jsonStr = cJSON_Print(doc);
  } else {
    jsonStr = cJSON_PrintUnformatted(doc);
  }

  std::string result;
  if (jsonStr) {
    result = jsonStr;
    cJSON_free(jsonStr);
  }
  cJSON_Delete(doc);

  return result;
}

void SarifLog::writeToFile(const std::string &filename, bool pretty) const {
  std::ofstream file(filename);
  if (file.is_open()) {
    file << toJsonString(pretty);
    file.close();
  }
}

void SarifLog::writeToStream(llvm::raw_ostream &os, bool pretty) const {
  os << toJsonString(pretty);
}

cJSON *SarifLog::toJsonDocument() const {
  cJSON *doc = cJSON_CreateObject();

  cJSON_AddStringToObject(doc, "version", "2.1.0");
  cJSON_AddStringToObject(doc, "$schema",
                          "https://json.schemastore.org/sarif-2.1.0.json");

  cJSON *runsArray = cJSON_CreateArray();
  cJSON *run = cJSON_CreateObject();

  // Tool information (enhanced)
  cJSON *tool = cJSON_CreateObject();
  cJSON *driver = cJSON_CreateObject();
  cJSON_AddStringToObject(driver, "name", toolName.c_str());
  cJSON_AddStringToObject(driver, "version", toolVersion.c_str());

  if (!toolInformationUri.empty()) {
    cJSON_AddStringToObject(driver, "informationUri",
                            toolInformationUri.c_str());
  }

  if (!rules.empty()) {
    cJSON *rulesArray = cJSON_CreateArray();
    for (const auto &rule : rules) {
      cJSON_AddItemToArray(rulesArray, rule.toJson());
    }
    cJSON_AddItemToObject(driver, "rules", rulesArray);
  }

  cJSON_AddItemToObject(tool, "driver", driver);
  cJSON_AddItemToObject(run, "tool", tool);

  // Add rule summary to run properties
  if (!results.empty()) {
    cJSON *runProperties = cJSON_CreateObject();
    cJSON *ruleCounts = cJSON_CreateObject();

    std::map<std::string, int> counts;
    for (const auto &result : results) {
      counts[result.ruleId]++;
    }

    for (const auto &pair : counts) {
      cJSON_AddNumberToObject(ruleCounts, pair.first.c_str(),
                              static_cast<int>(pair.second));
    }

    cJSON_AddItemToObject(runProperties, "ruleCounts", ruleCounts);
    cJSON_AddItemToObject(run, "properties", runProperties);
  }

  // Results
  if (!results.empty()) {
    cJSON *resultsArray = cJSON_CreateArray();
    for (const auto &result : results) {
      cJSON_AddItemToArray(resultsArray, result.toJson());
    }
    cJSON_AddItemToObject(run, "results", resultsArray);
  }

  cJSON_AddItemToArray(runsArray, run);
  cJSON_AddItemToObject(doc, "runs", runsArray);

  return doc;
}

// Utility functions implementation
namespace utils {

Location createLocationFromDebugLoc(const llvm::DebugLoc &debugLoc) {
  Location location;

  if (debugLoc) {
    location.line = static_cast<int>(debugLoc.getLine());
    location.column = static_cast<int>(debugLoc.getCol());

    if (auto *scope = debugLoc.getScope()) {
      if (auto *diScope = llvm::dyn_cast<llvm::DIScope>(scope)) {
        location.file =
            diScope->getDirectory().str() + "/" + diScope->getFilename().str();
      }
    }
  }

  return location;
}

Location createLocationFromInstruction(const llvm::Instruction *instruction) {
  Location location;

  if (instruction && instruction->getDebugLoc()) {
    location = createLocationFromDebugLoc(instruction->getDebugLoc());

    if (auto *func = instruction->getFunction()) {
      std::string funcName;

      // Try to get name from debug info first
      if (auto *subprogram = func->getSubprogram()) {
        funcName = subprogram->getName().str();
      } else {
        funcName = func->getName().str();
      }

      // Demangle C++ and Rust function names for better readability
      location.function = DemangleUtils::demangleWithCleanup(funcName);
    }
  }

  return location;
}

std::string levelToString(Level level) {
  switch (level) {
  case Level::Note:
    return "note";
  case Level::Warning:
    return "warning";
  case Level::Error:
    return "error";
  default:
    return "warning";
  }
}

Level stringToLevel(const std::string &level) {
  if (level == "note")
    return Level::Note;
  if (level == "warning")
    return Level::Warning;
  if (level == "error")
    return Level::Error;
  return Level::Warning;
}

} // namespace utils

// SarifBuilder implementation
SarifBuilder::SarifBuilder(const std::string &toolName) : log(toolName) {}

SarifBuilder &SarifBuilder::addRule(const std::string &id,
                                    const std::string &name,
                                    const std::string &description) {
  log.addRule(Rule(id, name, description));
  return *this;
}

SarifBuilder &SarifBuilder::addRule(const std::string &id,
                                    const std::string &name,
                                    const std::string &description,
                                    const std::string &helpUri,
                                    const std::string &category) {
  log.addRule(Rule(id, name, description, helpUri, category));
  return *this;
}

SarifBuilder &SarifBuilder::addResult(const std::string &ruleId,
                                      const std::string &message,
                                      const std::string &file, int line,
                                      int column, Level level) {
  Result result(ruleId, message);
  result.level = level;
  result.locations.push_back(Location(file, line, column));
  log.addResult(result);
  return *this;
}

SarifBuilder &SarifBuilder::addResult(const std::string &ruleId,
                                      const std::string &message,
                                      const std::string &file, int line,
                                      int column, Level level,
                                      const std::string &category) {
  Result result(ruleId, message);
  result.level = level;
  result.category = category;
  result.locations.push_back(Location(file, line, column));
  log.addResult(result);
  return *this;
}

SarifLog SarifBuilder::build() { return log; }

std::vector<SarifLog::RuleSummary> SarifLog::getRuleSummary() const {
  std::map<std::string, int> counts;
  std::map<std::string, std::string> ruleNames;

  // Count results by rule ID
  for (const auto &result : results) {
    counts[result.ruleId]++;
  }

  // Get rule names
  for (const auto &rule : rules) {
    ruleNames[rule.id] = rule.name;
  }

  // Build summary
  std::vector<RuleSummary> summary;
  for (const auto &pair : counts) {
    RuleSummary rs;
    rs.ruleId = pair.first;
    rs.count = pair.second;
    auto it = ruleNames.find(pair.first);
    rs.ruleName = (it != ruleNames.end()) ? it->second : pair.first;
    summary.push_back(rs);
  }

  return summary;
}

} // namespace sarif
