#include "Verification/SymAbsAI/Utils/Config.h"

#include "Verification/SymAbsAI/Utils/Utils.h"

#include <map>
#include <string>

#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

using namespace symabs_ai;

namespace symabs_ai {
namespace configparser {

// Parse a simple key=value configuration file
static void parseConfigFile(const std::string &filename,
                            std::map<std::string, std::string> &config) {
  auto bufferOrErr = llvm::MemoryBuffer::getFile(filename);
  if (!bufferOrErr) {
    llvm::errs() << "Warning: Could not open config file: " << filename << "\n";
    return;
  }

  llvm::StringRef content = bufferOrErr.get()->getBuffer();
  llvm::SmallVector<llvm::StringRef, 0> lines;
  content.split(lines, '\n');

  for (auto line : lines) {
    // Trim whitespace
    line = line.trim();

    // Skip empty lines and comments
    if (line.empty() || line.startswith("#")) {
      continue;
    }

    // Parse key = value
    auto eqPos = line.find('=');
    if (eqPos == llvm::StringRef::npos) {
      continue;
    }

    llvm::StringRef key = line.substr(0, eqPos).trim();
    llvm::StringRef value = line.substr(eqPos + 1).trim();

    config[key.str()] = value.str();
  }
}

Config::Config(llvm::StringRef filename) {
  ConfigDict_ = make_shared<std::map<std::string, std::string>>();

  if (!filename.empty()) {
    parseConfigFile(filename.str(), *ConfigDict_);
  }
}

Config::Config() {
  ConfigDict_ = make_shared<std::map<std::string, std::string>>();

  // Check environment variable
  const char *envConfig = std::getenv(ENV_VAR);
  if (envConfig) {
    parseConfigFile(envConfig, *ConfigDict_);
  }
}

template <typename T>
T Config::get(const char *module, const char *key, T default_value) const {
  // Build full key: module.key
  std::string fullKey = std::string(module) + "." + std::string(key);

  auto it = ConfigDict_->find(fullKey);
  if (it == ConfigDict_->end()) {
    return default_value;
  }

  // For now, just return default - specializations handle conversion
  return default_value;
}

// Specialization for string
template <>
std::string Config::get(const char *module, const char *key,
                        std::string default_value) const {
  std::string fullKey = std::string(module) + "." + std::string(key);
  auto it = ConfigDict_->find(fullKey);
  return (it != ConfigDict_->end()) ? it->second : default_value;
}

// Specialization for bool
template <>
bool Config::get(const char *module, const char *key,
                 bool default_value) const {
  std::string fullKey = std::string(module) + "." + std::string(key);
  auto it = ConfigDict_->find(fullKey);
  if (it == ConfigDict_->end()) {
    return default_value;
  }

  std::string value = it->second;
  if (value == "true" || value == "True" || value == "1") {
    return true;
  } else if (value == "false" || value == "False" || value == "0") {
    return false;
  }
  return default_value;
}

// Specialization for int
template <>
int Config::get(const char *module, const char *key, int default_value) const {
  std::string fullKey = std::string(module) + "." + std::string(key);
  auto it = ConfigDict_->find(fullKey);
  if (it == ConfigDict_->end()) {
    return default_value;
  }

  try {
    return std::stoi(it->second);
  } catch (...) {
    return default_value;
  }
}

void Config::set(const char *module, const char *key, llvm::StringRef value) {
  std::string fullKey = std::string(module) + "." + std::string(key);
  (*ConfigDict_)[fullKey] = value.str();
}

void Config::set(const char *module, const char *key, int value) {
  set(module, key, llvm::StringRef(std::to_string(value)));
}

void Config::set(const char *module, const char *key, bool value) {
  set(module, key, llvm::StringRef(value ? "true" : "false"));
}

} // namespace configparser
} // namespace symabs_ai
