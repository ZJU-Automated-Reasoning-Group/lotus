#include "Utils/LLVM/Demangle.h"

#include <regex>

namespace DemangleUtils {

bool isMangled(const std::string &name) {
  // Check for C++ mangled names (usually start with _Z)
  if (name.size() > 2 && name[0] == '_' && name[1] == 'Z') {
    return true;
  }

  // Check for Rust v0 mangled names (start with _R)
  if (name.size() > 2 && name[0] == '_' && name[1] == 'R') {
    return true;
  }

  // Check for Rust legacy mangled names (contain hash patterns)
  std::regex rustPattern(".*h[0-9a-fA-F]{16}.*");
  if (std::regex_match(name, rustPattern)) {
    return true;
  }

  // Check for special encodings like $LT$, $GT$, etc.
  if (name.find("$LT$") != std::string::npos ||
      name.find("$GT$") != std::string::npos ||
      name.find("$u20$") != std::string::npos) {
    return true;
  }

  return false;
}

std::string demangle(const std::string &mangledName) {
  if (mangledName.empty()) {
    return mangledName;
  }

  // Use LLVM's demangle function
  std::string demangled = llvm::demangle(mangledName);

  // If demangling didn't change the name and it doesn't look mangled,
  // just return the original
  if (demangled == mangledName && !isMangled(mangledName)) {
    return mangledName;
  }

  return demangled;
}

std::string demangleWithCleanup(const std::string &mangledName) {
  if (mangledName.empty()) {
    return mangledName;
  }

  std::string demangled = demangle(mangledName);

  // Remove Rust hash suffix: keep up to the last "::" before a hash pattern
  // This handles cases like "std::thread::spawn::hc6f148c1a1888888"
  std::regex hashSuffixPattern("::h[0-9a-fA-F]{16}$");
  demangled = std::regex_replace(demangled, hashSuffixPattern, "");

  // Replace special Rust encodings
  // Replace $LT$ with <
  size_t pos = 0;
  while ((pos = demangled.find("$LT$", pos)) != std::string::npos) {
    demangled.replace(pos, 4, "<");
    pos += 1;
  }

  // Replace $GT$ with >
  pos = 0;
  while ((pos = demangled.find("$GT$", pos)) != std::string::npos) {
    demangled.replace(pos, 4, ">");
    pos += 1;
  }

  // Replace .. with ::
  pos = 0;
  while ((pos = demangled.find("..", pos)) != std::string::npos) {
    demangled.replace(pos, 2, "::");
    pos += 2;
  }

  // Replace $u20$ with space (e.g., "xxx as xxx")
  pos = 0;
  while ((pos = demangled.find("$u20$", pos)) != std::string::npos) {
    demangled.replace(pos, 5, " ");
    pos += 1;
  }

  // Replace $u7b$ with {
  pos = 0;
  while ((pos = demangled.find("$u7b$", pos)) != std::string::npos) {
    demangled.replace(pos, 5, "{");
    pos += 1;
  }

  // Replace $u7d$ with }
  pos = 0;
  while ((pos = demangled.find("$u7d$", pos)) != std::string::npos) {
    demangled.replace(pos, 5, "}");
    pos += 1;
  }

  // Replace $u27$ with '
  pos = 0;
  while ((pos = demangled.find("$u27$", pos)) != std::string::npos) {
    demangled.replace(pos, 5, "'");
    pos += 1;
  }

  // Replace $u5b$ with [
  pos = 0;
  while ((pos = demangled.find("$u5b$", pos)) != std::string::npos) {
    demangled.replace(pos, 5, "[");
    pos += 1;
  }

  // Replace $u5d$ with ]
  pos = 0;
  while ((pos = demangled.find("$u5d$", pos)) != std::string::npos) {
    demangled.replace(pos, 5, "]");
    pos += 1;
  }

  // Remove leading underscore if present (common in mangled names)
  if (!demangled.empty() && demangled[0] == '_' &&
      demangled != mangledName) { // Only if demangling actually occurred
    // Check if it's not a normal identifier that happens to start with _
    if (isMangled(mangledName)) {
      demangled.erase(0, 1);
    }
  }

  return demangled;
}

} // namespace DemangleUtils
