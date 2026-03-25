#include "Checker/Report/SuppressionManager.h"

#include "Utils/Formats/cJSON.h"

#include <fstream>
#include <sstream>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>

SuppressionManager::SuppressionManager() {}

SuppressionManager::~SuppressionManager() {}

bool SuppressionManager::Span::contains(int line) const {
  if (type == Every) {
    return true;
  }

  for (const auto &block : blocks) {
    if (line >= block.first && line <= block.second) {
      return true;
    }
  }

  return false;
}

void SuppressionManager::Span::addBlock(int first, int last) {
  if (type == Every) {
    type = Blocks;
    blocks.clear();
  }
  blocks.push_back({first, last});
}

std::string SuppressionManager::normalizePath(const std::string &path) const {
  // Normalize path separators and resolve relative paths
  std::string normalized = path;

  // Replace backslashes with forward slashes
  for (char &c : normalized) {
    if (c == '\\') {
      c = '/';
    }
  }

  // Remove leading "./" or "../"
  while (normalized.size() >= 2 && normalized.substr(0, 2) == "./") {
    normalized = normalized.substr(2);
  }

  return normalized;
}

bool SuppressionManager::loadFromFile(const std::string &filename) {
  auto buffer = llvm::MemoryBuffer::getFile(filename);
  if (!buffer) {
    return false;
  }

  return loadFromString(buffer.get()->getBuffer().str());
}

bool SuppressionManager::loadFromString(const std::string &json) {
  cJSON *root = cJSON_Parse(json.c_str());
  if (!root) {
    return false;
  }

  bool success = parseJsonObject(json);
  cJSON_Delete(root);
  return success;
}

bool SuppressionManager::parseJsonObject(const std::string &json) {
  cJSON *root = cJSON_Parse(json.c_str());
  if (!root || !cJSON_IsObject(root)) {
    if (root)
      cJSON_Delete(root);
    return false;
  }

  cJSON *fileItem = nullptr;
  cJSON_ArrayForEach(fileItem, root) {
    // fileItem->string is the key name, fileItem is the value
    if (!fileItem->string || !cJSON_IsObject(fileItem)) {
      continue;
    }

    std::string filename = normalizePath(fileItem->string);
    cJSON *fileObj = fileItem;

    cJSON *issueItem = nullptr;
    cJSON_ArrayForEach(issueItem, fileObj) {
      // issueItem->string is the key name, issueItem is the value
      if (!issueItem->string) {
        continue;
      }

      std::string issueType = issueItem->string;
      Span span = parseSpan(cJSON_Print(issueItem));

      suppressions[filename][issueType] = span;
    }
  }

  cJSON_Delete(root);
  return true;
}

SuppressionManager::Span
SuppressionManager::parseSpan(const std::string &jsonValue) {
  Span span;

  cJSON *json = cJSON_Parse(jsonValue.c_str());
  if (!json) {
    return span; // Default to Every
  }

  if (cJSON_IsString(json)) {
    std::string str = json->valuestring;
    if (str == "Every" || str == "every") {
      span.type = Span::Every;
    }
  } else if (cJSON_IsArray(json)) {
    span.type = Span::Blocks;
    int arraySize = cJSON_GetArraySize(json);
    for (int i = 0; i < arraySize; ++i) {
      cJSON *block = cJSON_GetArrayItem(json, i);
      if (cJSON_IsArray(block) && cJSON_GetArraySize(block) >= 2) {
        cJSON *first = cJSON_GetArrayItem(block, 0);
        cJSON *last = cJSON_GetArrayItem(block, 1);
        if (cJSON_IsNumber(first) && cJSON_IsNumber(last)) {
          span.addBlock(first->valueint, last->valueint);
        }
      }
    }
  }

  cJSON_Delete(json);
  return span;
}

void SuppressionManager::addSuppression(const std::string &issueType,
                                        const std::string &filename,
                                        const Span &span) {
  std::string normalized = normalizePath(filename);
  suppressions[normalized][issueType] = span;
}

void SuppressionManager::addFileSuppression(const std::string &issueType,
                                            const std::string &filename) {
  Span span(Span::Every);
  addSuppression(issueType, filename, span);
}

void SuppressionManager::addLineRangeSuppression(const std::string &issueType,
                                                 const std::string &filename,
                                                 int firstLine, int lastLine) {
  std::string normalized = normalizePath(filename);
  Span &span = suppressions[normalized][issueType];
  span.type = Span::Blocks;
  span.addBlock(firstLine, lastLine);
}

bool SuppressionManager::isSuppressed(const std::string &issueType,
                                      const std::string &filename,
                                      int line) const {
  std::string normalized = normalizePath(filename);

  auto fileIt = suppressions.find(normalized);
  if (fileIt == suppressions.end()) {
    return false;
  }

  auto issueIt = fileIt->second.find(issueType);
  if (issueIt == fileIt->second.end()) {
    return false;
  }

  return issueIt->second.contains(line);
}

bool SuppressionManager::isFileSuppressed(const std::string &issueType,
                                          const std::string &filename) const {
  std::string normalized = normalizePath(filename);

  auto fileIt = suppressions.find(normalized);
  if (fileIt == suppressions.end()) {
    return false;
  }

  auto issueIt = fileIt->second.find(issueType);
  if (issueIt == fileIt->second.end()) {
    return false;
  }

  return issueIt->second.type == Span::Every;
}

llvm::StringSet<>
SuppressionManager::getSuppressedTypes(const std::string &filename) const {
  llvm::StringSet<> types;
  std::string normalized = normalizePath(filename);

  auto fileIt = suppressions.find(normalized);
  if (fileIt != suppressions.end()) {
    for (const auto &pair : fileIt->second) {
      types.insert(pair.first);
    }
  }

  return types;
}

void SuppressionManager::clear() { suppressions.clear(); }

void SuppressionManager::exportToJson(llvm::raw_ostream &OS) const {
  OS << "{\n";
  bool firstFile = true;

  for (const auto &filePair : suppressions) {
    if (!firstFile)
      OS << ",\n";
    firstFile = false;

    OS << "  \"" << filePair.first << "\": {\n";
    bool firstIssue = true;

    for (const auto &issuePair : filePair.second) {
      if (!firstIssue)
        OS << ",\n";
      firstIssue = false;

      OS << "    \"" << issuePair.first << "\": ";
      const Span &span = issuePair.second;

      if (span.type == Span::Every) {
        OS << "\"Every\"";
      } else {
        OS << "[";
        for (size_t i = 0; i < span.blocks.size(); ++i) {
          if (i > 0)
            OS << ", ";
          OS << "[" << span.blocks[i].first << ", " << span.blocks[i].second
             << "]";
        }
        OS << "]";
      }
    }

    OS << "\n  }";
  }

  OS << "\n}\n";
}

SuppressionManager::Stats SuppressionManager::getStats() const {
  Stats stats;
  stats.totalFiles = suppressions.size();

  std::set<std::string> issueTypes;
  size_t totalSuppressions = 0;

  for (const auto &filePair : suppressions) {
    for (const auto &issuePair : filePair.second) {
      issueTypes.insert(issuePair.first);
      totalSuppressions++;
    }
  }

  stats.totalIssueTypes = issueTypes.size();
  stats.totalSuppressions = totalSuppressions;

  return stats;
}

bool SuppressionManager::parseSuppressionComment(
    const std::string &comment, std::string &issueType,
    std::vector<std::pair<int, int>> &lineRanges) {
  // Look for "lotus-suppress:" pattern
  size_t pos = comment.find("lotus-suppress:");
  if (pos == std::string::npos) {
    return false;
  }

  // Extract the part after "lotus-suppress:"
  std::string rest = comment.substr(pos + 15); // "lotus-suppress:" is 15 chars

  // Trim whitespace
  while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) {
    rest = rest.substr(1);
  }

  // Parse issue type (everything up to space or end)
  size_t spacePos = rest.find_first_of(" \t\n");
  if (spacePos == std::string::npos) {
    issueType = rest;
    return true;
  }

  issueType = rest.substr(0, spacePos);
  rest = rest.substr(spacePos);

  // Try to parse line ranges (e.g., "on line 10-20")
  // This is a simple implementation - can be enhanced
  size_t linePos = rest.find("line");
  if (linePos != std::string::npos) {
    // Parse ranges like "10-20" or "10,20-30"
    // Simplified: just look for number-number patterns
    // Full implementation would need proper parsing
  }

  return true;
}
