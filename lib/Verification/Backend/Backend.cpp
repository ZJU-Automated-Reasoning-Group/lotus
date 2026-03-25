#include "Verification/Backend/Backend.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cctype>
#include <string>

using namespace llvm;

namespace lotus {
namespace verification {
namespace backend {

namespace {
static std::string toLower(const std::string &s) {
  std::string result = s;
  std::transform(
      result.begin(), result.end(), result.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

static bool hasToken(StringRef haystack, StringRef needle) {
  size_t pos = haystack.find(needle);
  while (pos != StringRef::npos) {
    const bool left_ok =
        pos == 0 || !std::isalnum(static_cast<unsigned char>(haystack[pos - 1]));
    const size_t end = pos + needle.size();
    const bool right_ok =
        end >= haystack.size() ||
        !std::isalnum(static_cast<unsigned char>(haystack[end]));
    if (left_ok && right_ok) {
      return true;
    }
    pos = haystack.find(needle, pos + 1);
  }
  return false;
}

static bool hasToken(const std::string &haystack, StringRef needle) {
  return hasToken(StringRef(haystack), needle);
}

class SeahornBackend final : public IBackend {
public:
  const char *name() const override { return "seahorn"; }

  bool supports(PropertyClass) const override { return true; }

  std::vector<std::string>
  buildCommand(const VerificationTask &task) const override {
    std::vector<std::string> cmd = {"seahorn"};
    switch (task.property) {
    case PropertyClass::MemSafety:
      cmd.push_back("--track=mem");
      break;
    case PropertyClass::Termination:
      cmd.push_back("--termination");
      break;
    default:
      break;
    }
    if (task.timeoutSeconds > 0)
      cmd.push_back("--cpu=" + std::to_string(task.timeoutSeconds));
    cmd.push_back(task.inputBitcode);
    cmd.insert(cmd.end(), task.extraArgs.begin(), task.extraArgs.end());
    return cmd;
  }

  VerificationResultInfo parseResult(const std::string &output,
                                     int exitCode) const override {
    VerificationResultInfo info;
    info.exitCode = exitCode;

    std::string lower = toLower(output);

    // SeaHorn typically outputs "sat" (error found) or "unsat" (safe)
    if (hasToken(lower, "unsafe") || hasToken(lower, "false") ||
        hasToken(lower, "error found") ||
        hasToken(lower, "__verifier_error") ||
        hasToken(lower, "counterexample") || hasToken(lower, "sat")) {
      info.result = VerificationResult::False;
      info.message = "Property violated (error found)";
      // Try to extract error trace
      size_t traceStart = lower.find("error trace");
      if (traceStart != std::string::npos) {
        info.errorTrace = output.substr(traceStart);
      }
    } else if (hasToken(lower, "unsat") || hasToken(lower, "safe") ||
               hasToken(lower, "true") ||
               hasToken(lower, "verification successful")) {
      info.result = VerificationResult::True;
      info.message = "Property holds (no error found)";
    } else if (exitCode == 124 || hasToken(lower, "timeout")) {
      info.result = VerificationResult::Timeout;
      info.message = "Verification timed out";
    } else if (exitCode != 0) {
      info.result = VerificationResult::Error;
      info.message = "Verification tool error (exit code " +
                     std::to_string(exitCode) + ")";
    } else {
      info.result = VerificationResult::Unknown;
      info.message = "Could not determine verification result";
    }

    return info;
  }
};

class SifaBackend final : public IBackend {
public:
  const char *name() const override { return "sifa"; }

  bool supports(PropertyClass property) const override {
    return property == PropertyClass::Reachability ||
           property == PropertyClass::Unknown;
  }

  std::vector<std::string>
  buildCommand(const VerificationTask &task) const override {
    std::vector<std::string> cmd = {"sifa", task.inputBitcode};
    cmd.insert(cmd.end(), task.extraArgs.begin(), task.extraArgs.end());
    return cmd;
  }

  VerificationResultInfo parseResult(const std::string &output,
                                     int exitCode) const override {
    VerificationResultInfo info;
    info.exitCode = exitCode;

    std::string lower = toLower(output);

    if (hasToken(lower, "reachable") || hasToken(lower, "true")) {
      info.result = VerificationResult::True;
      info.message = "Target is reachable";
    } else if (hasToken(lower, "not reachable") ||
               hasToken(lower, "unreachable") || hasToken(lower, "false")) {
      info.result = VerificationResult::False;
      info.message = "Target is not reachable";
    } else if (exitCode != 0) {
      info.result = VerificationResult::Error;
      info.message = "Sifa error (exit code " + std::to_string(exitCode) + ")";
    } else {
      info.result = VerificationResult::Unknown;
      info.message = "Could not determine result";
    }

    return info;
  }
};

class SymAbsAIBackend final : public IBackend {
public:
  const char *name() const override { return "symabs_ai"; }

  bool supports(PropertyClass property) const override {
    return property == PropertyClass::Reachability ||
           property == PropertyClass::MemSafety ||
           property == PropertyClass::Overflow ||
           property == PropertyClass::Unknown;
  }

  std::vector<std::string>
  buildCommand(const VerificationTask &task) const override {
    std::vector<std::string> cmd = {"symabs_ai", task.inputBitcode};
    cmd.insert(cmd.end(), task.extraArgs.begin(), task.extraArgs.end());
    return cmd;
  }

  VerificationResultInfo parseResult(const std::string &output,
                                     int exitCode) const override {
    VerificationResultInfo info;
    info.exitCode = exitCode;

    std::string lower = toLower(output);

    if (hasToken(lower, "unsafe") || hasToken(lower, "false") ||
        hasToken(lower, "violation") || hasToken(lower, "error")) {
      info.result = VerificationResult::False;
      info.message = "Property violated";
    } else if (hasToken(lower, "safe") || hasToken(lower, "true") ||
               hasToken(lower, "property holds")) {
      info.result = VerificationResult::True;
      info.message = "Property holds";
    } else if (exitCode != 0) {
      info.result = VerificationResult::Error;
      info.message =
          "SymAbsAI error (exit code " + std::to_string(exitCode) + ")";
    } else {
      info.result = VerificationResult::Unknown;
      info.message = "Could not determine result";
    }

    return info;
  }
};

class ClamBackend final : public IBackend {
public:
  const char *name() const override { return "clam"; }

  bool supports(PropertyClass property) const override {
    return property == PropertyClass::MemSafety ||
           property == PropertyClass::Overflow ||
           property == PropertyClass::Reachability ||
           property == PropertyClass::Unknown;
  }

  std::vector<std::string>
  buildCommand(const VerificationTask &task) const override {
    std::vector<std::string> cmd = {"clam"};
    if (task.property == PropertyClass::MemSafety)
      cmd.push_back("--crab-check=null");
    if (task.property == PropertyClass::Overflow)
      cmd.push_back("--crab-check=assert");
    cmd.push_back(task.inputBitcode);
    cmd.insert(cmd.end(), task.extraArgs.begin(), task.extraArgs.end());
    return cmd;
  }

  VerificationResultInfo parseResult(const std::string &output,
                                     int exitCode) const override {
    VerificationResultInfo info;
    info.exitCode = exitCode;

    std::string lower = toLower(output);

    // CLAM typically outputs "safe" or "unsafe"
    if (hasToken(lower, "unsafe") || hasToken(lower, "false") ||
        hasToken(lower, "violation")) {
      info.result = VerificationResult::False;
      info.message = "Property violated (unsafe)";
    } else if (hasToken(lower, "safe") || hasToken(lower, "true") ||
               hasToken(lower, "no error")) {
      info.result = VerificationResult::True;
      info.message = "Property holds (safe)";
    } else if (exitCode != 0) {
      info.result = VerificationResult::Error;
      info.message = "CLAM error (exit code " + std::to_string(exitCode) + ")";
    } else {
      info.result = VerificationResult::Unknown;
      info.message = "Could not determine result";
    }

    return info;
  }
};

static bool eqLower(StringRef a, StringRef b) {
  return a.equals_insensitive(b);
}

} // namespace

BackendRegistry &BackendRegistry::instance() {
  static BackendRegistry R;
  return R;
}

std::vector<std::string> BackendRegistry::availableBackends() const {
  return {"seahorn", "sifa", "symabs_ai", "clam"};
}

std::unique_ptr<IBackend>
BackendRegistry::create(const std::string &name) const {
  if (eqLower(name, "seahorn"))
    return std::unique_ptr<IBackend>(new SeahornBackend());
  if (eqLower(name, "sifa"))
    return std::unique_ptr<IBackend>(new SifaBackend());
  if (eqLower(name, "symabs_ai") || eqLower(name, "symbolic_abstraction") ||
      eqLower(name, "symabs"))
    return std::unique_ptr<IBackend>(new SymAbsAIBackend());
  if (eqLower(name, "clam"))
    return std::unique_ptr<IBackend>(new ClamBackend());
  return nullptr;
}

std::vector<std::string>
BackendRegistry::recommend(PropertyClass property) const {
  std::vector<std::string> out;
  for (const std::string &name : availableBackends()) {
    std::unique_ptr<IBackend> b = create(name);
    if (b && b->supports(property))
      out.push_back(name);
  }
  return out;
}

PropertyClass parsePropertyClass(const std::string &name) {
  StringRef N(name);
  if (N.empty())
    return PropertyClass::Unknown;
  if (N.equals_insensitive("unreach-call") ||
      N.equals_insensitive("reachability"))
    return PropertyClass::Reachability;
  if (N.equals_insensitive("memsafety") ||
      N.equals_insensitive("valid-memsafety"))
    return PropertyClass::MemSafety;
  if (N.equals_insensitive("no-overflow") || N.equals_insensitive("overflow"))
    return PropertyClass::Overflow;
  if (N.equals_insensitive("termination"))
    return PropertyClass::Termination;
  return PropertyClass::Unknown;
}

std::string toString(PropertyClass property) {
  switch (property) {
  case PropertyClass::Reachability:
    return "reachability";
  case PropertyClass::MemSafety:
    return "memsafety";
  case PropertyClass::Overflow:
    return "overflow";
  case PropertyClass::Termination:
    return "termination";
  case PropertyClass::Unknown:
  default:
    return "unknown";
  }
}

VerificationResult parseResultFromString(const std::string &str) {
  std::string lower = toLower(str);
  if (hasToken(lower, "false") || hasToken(lower, "unsafe") ||
      hasToken(lower, "sat"))
    return VerificationResult::False;
  if (hasToken(lower, "true") || hasToken(lower, "safe") ||
      hasToken(lower, "unsat"))
    return VerificationResult::True;
  if (hasToken(lower, "timeout"))
    return VerificationResult::Timeout;
  if (hasToken(lower, "error"))
    return VerificationResult::Error;
  return VerificationResult::Unknown;
}

std::string toString(VerificationResult result) {
  switch (result) {
  case VerificationResult::True:
    return "true";
  case VerificationResult::False:
    return "false";
  case VerificationResult::Unknown:
    return "unknown";
  case VerificationResult::Error:
    return "error";
  case VerificationResult::Timeout:
    return "timeout";
  default:
    return "unknown";
  }
}

} // namespace backend
} // namespace verification
} // namespace lotus
