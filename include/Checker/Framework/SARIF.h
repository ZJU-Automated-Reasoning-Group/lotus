/** @file SARIF.h @brief SARIF (Static Analysis Results Interchange Format) output for bug reports. */
#ifndef SARIF_H
#define SARIF_H

#include "Utils/Formats/cJSON.h"

#include <string>
#include <vector>

#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>

namespace sarif {

enum class Level { Note, Warning, Error };

// Simple location representation
struct Location {
  std::string file;
  int line = 0;
  int column = 0;
  std::string function;
  std::string snippet;
  std::string message; // Optional message for this location

  Location() = default;
  Location(const std::string &file, int line, int column = 0)
      : file(file), line(line), column(column) {}

  cJSON *toJson() const;
  cJSON *toThreadFlowLocationJson() const; // For codeFlow representation
};

// Thread flow location for code flows (execution paths)
struct ThreadFlowLocation {
  Location location;
  std::string message;
  int nestingLevel = 0; // For showing call depth
  int executionOrder = 0;

  ThreadFlowLocation() = default;
  ThreadFlowLocation(const Location &loc, const std::string &msg = "",
                     int order = 0)
      : location(loc), message(msg), executionOrder(order) {}

  cJSON *toJson() const;
};

// Code flow represents an execution path
struct CodeFlow {
  std::vector<ThreadFlowLocation> threadFlowLocations;
  std::string message;

  CodeFlow() = default;

  cJSON *toJson() const;
};

// Enhanced result representation with categorization
struct Result {
  std::string ruleId;
  std::string message;
  Level level = Level::Warning;
  std::vector<Location> locations;
  std::vector<Location> relatedLocations;
  std::vector<CodeFlow> codeFlows; // Execution paths that lead to the result

  // Enhanced fields inspired by Infer
  std::string category;         // Issue category
  std::string fingerprint;      // Unique identifier for deduplication
  std::string suppressionState; // "suppressed" or empty
  std::string censoredReason;   // Reason if censored (privacy)

  Result(const std::string &ruleId, const std::string &message)
      : ruleId(ruleId), message(message) {}

  cJSON *toJson() const;
};

// Enhanced rule representation with metadata (inspired by Infer)
struct Rule {
  std::string id;
  std::string name;
  std::string description;
  std::string shortDescription; // Brief description for UI
  std::string helpUri;          // Link to documentation
  std::string category; // Issue category (e.g., "Security", "Correctness")
  std::string severity; // Default severity ("error", "warning", "note")

  Rule(const std::string &id, const std::string &name,
       const std::string &description = "")
      : id(id), name(name), description(description),
        shortDescription(description), category(""), severity("warning") {}

  Rule(const std::string &id, const std::string &name,
       const std::string &description, const std::string &helpUri,
       const std::string &category = "")
      : id(id), name(name), description(description),
        shortDescription(description), helpUri(helpUri), category(category),
        severity("warning") {}

  cJSON *toJson() const;
};

// Main SARIF log with enhanced metadata
class SarifLog {
public:
  SarifLog(const std::string &toolName = "Lotus",
           const std::string &version = "1.0.0");

  void addRule(const Rule &rule);
  void addResult(const Result &result);

  // Generate rule summary (counts by rule ID)
  struct RuleSummary {
    std::string ruleId;
    int count;
    std::string ruleName;
  };
  std::vector<RuleSummary> getRuleSummary() const;

  std::string toJsonString(bool pretty = true) const;
  void writeToFile(const std::string &filename, bool pretty = true) const;
  void writeToStream(llvm::raw_ostream &os, bool pretty = true) const;

  // Set tool information URI
  void setToolInformationUri(const std::string &uri) {
    toolInformationUri = uri;
  }

private:
  std::string toolName;
  std::string toolVersion;
  std::string toolInformationUri; // Link to tool documentation
  std::vector<Rule> rules;
  std::vector<Result> results;

  cJSON *toJsonDocument() const;
};

// Utility functions
namespace utils {
Location createLocationFromDebugLoc(const llvm::DebugLoc &debugLoc);
Location createLocationFromInstruction(const llvm::Instruction *instruction);
std::string levelToString(Level level);
Level stringToLevel(const std::string &level);
} // namespace utils

// Enhanced builder for common use cases
class SarifBuilder {
public:
  SarifBuilder(const std::string &toolName = "Lotus");

  SarifBuilder &addRule(const std::string &id, const std::string &name,
                        const std::string &description = "");
  SarifBuilder &addRule(const std::string &id, const std::string &name,
                        const std::string &description,
                        const std::string &helpUri,
                        const std::string &category = "");
  SarifBuilder &addResult(const std::string &ruleId, const std::string &message,
                          const std::string &file, int line, int column = 0,
                          Level level = Level::Warning);
  SarifBuilder &addResult(const std::string &ruleId, const std::string &message,
                          const std::string &file, int line, int column,
                          Level level, const std::string &category);

  SarifLog build();

private:
  SarifLog log;
};

} // namespace sarif

#endif // SARIF_H
