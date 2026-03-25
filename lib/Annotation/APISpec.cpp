#include "Annotation/APISpec.h"

#include <cctype>
#include <fstream>
#include <sstream>

#include <llvm/Support/raw_ostream.h>

using namespace lotus;

namespace {
// Trims whitespace from both ends of a string.
static inline std::string trim(const std::string &s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
    ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
    --e;
  return s.substr(b, e - b);
}

// Returns true if the line is a comment (starts with #) or blank.
static inline bool isCommentOrBlank(const std::string &line) {
  for (char c : line) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      return c == '#';
    }
  }
  return true;
}
} // namespace

// Converts a string token to a SpecOpKind enum value.
// Returns false via the out-parameter if the token is unrecognised.
static bool toOpKind(const std::string &tok, SpecOpKind &out) {
  if (tok == "IGNORE") {
    out = SpecOpKind::Ignore;
    return true;
  }
  if (tok == "ALLOC") {
    out = SpecOpKind::Alloc;
    return true;
  }
  if (tok == "DEALLOC") {
    out = SpecOpKind::Dealloc;
    return true;
  }
  if (tok == "COPY") {
    out = SpecOpKind::Copy;
    return true;
  }
  if (tok == "EXIT") {
    out = SpecOpKind::Exit;
    return true;
  }
  if (tok == "MOD") {
    out = SpecOpKind::Mod;
    return true;
  }
  if (tok == "REF") {
    out = SpecOpKind::Ref;
    return true;
  }
  return false;
}

// Converts a string to a QualifierKind enum value.
static QualifierKind toQualifier(const std::string &q) {
  if (q == "V")
    return QualifierKind::Value;
  if (q == "R")
    return QualifierKind::Region;
  if (q == "D")
    return QualifierKind::Data;
  return QualifierKind::Unknown;
}

// Parses a line from the API specification file.
// Returns true if parsing was successful, false otherwise.
bool APISpec::parseLine(const std::string &line, std::string &outFunc,
                        SpecOpKind &outOp,
                        std::vector<std::string> &outTokens) {
  outFunc.clear();
  outTokens.clear();

  std::string s = trim(line);
  if (s.empty() || isCommentOrBlank(s))
    return false;

  std::istringstream iss(s);
  std::string func, op;
  if (!(iss >> func))
    return false;
  if (!(iss >> op))
    return false;

  if (!toOpKind(op, outOp)) {
    llvm::errs() << "[APISpec] Warning: unrecognised op '" << op
                 << "' for function '" << func << "' — line skipped\n";
    return false;
  }

  outFunc = func;

  std::string tok;
  while (iss >> tok)
    outTokens.push_back(tok);
  return true;
}

// Parses a value selector token (e.g., "Ret", "Arg0", "AfterArg2").
// Returns an invalid selector (isValid=false) on any parse error.
ValueSelector APISpec::parseSelector(const std::string &token) {
  if (token == "Ret")
    return ValueSelector{SelectorKind::Ret, -1, true};
  if (token == "STATIC")
    return ValueSelector{SelectorKind::Static, -1, true};
  if (token == "NULL")
    return ValueSelector{SelectorKind::Null, -1, true};

  auto parseArgIndex = [](const std::string &suffix, int &idx) -> bool {
    if (suffix.empty())
      return false;
    try {
      size_t pos = 0;
      long val = std::stol(suffix, &pos);
      if (pos != suffix.size())
        return false; // trailing garbage
      if (val < 0 || val > 255) {
        llvm::errs() << "[APISpec] Warning: argument index " << val
                     << " out of range [0,255]\n";
        return false;
      }
      idx = static_cast<int>(val);
      return true;
    } catch (...) {
      return false;
    }
  };

  if (token.rfind("AfterArg", 0) == 0) {
    int idx = -1;
    if (!parseArgIndex(token.substr(8), idx)) {
      llvm::errs() << "[APISpec] Warning: malformed selector '" << token
                   << "'\n";
      return ValueSelector{SelectorKind::Ret, -1, false};
    }
    return ValueSelector{SelectorKind::AfterArg, idx, true};
  }

  if (token.rfind("Arg", 0) == 0) {
    int idx = -1;
    if (!parseArgIndex(token.substr(3), idx)) {
      llvm::errs() << "[APISpec] Warning: malformed selector '" << token
                   << "'\n";
      return ValueSelector{SelectorKind::Ret, -1, false};
    }
    return ValueSelector{SelectorKind::Arg, idx, true};
  }

  llvm::errs() << "[APISpec] Warning: unrecognised selector '" << token
               << "'\n";
  return ValueSelector{SelectorKind::Ret, -1, false};
}

// Parses a qualifier token (V, R, D).
QualifierKind APISpec::parseQualifier(const std::string &token) {
  return toQualifier(token);
}

// Applies ALLOC operation to a function specification.
void APISpec::applyAlloc(FunctionSpec &spec,
                         const std::vector<std::string> &tokens) {
  spec.isAllocator = true;
  AllocEffect eff;
  eff.sizeArgIndex = -1;
  // Some ALLOC lines provide an ArgN hint, e.g., "malloc ALLOC Arg0".
  if (!tokens.empty() && tokens[0].rfind("Arg", 0) == 0) {
    try {
      const std::string suffix = tokens[0].substr(3);
      size_t pos = 0;
      long val = std::stol(suffix, &pos);
      // pos must equal the full suffix length (no trailing garbage),
      // and the index must fit in uint8_t.
      if (pos == suffix.size() && val >= 0 && val <= 255)
        eff.sizeArgIndex = static_cast<int>(val);
    } catch (...) {
      eff.sizeArgIndex = -1;
    }
  }
  spec.allocs.push_back(eff);
}

// Applies COPY operation to a function specification.
// Returns false and emits a warning if the token count is insufficient.
bool APISpec::applyCopy(FunctionSpec &spec,
                        const std::vector<std::string> &tokens) {
  // Expect pattern: COPY <DstSel> <DstQual> <SrcSel> <SrcQual>
  if (tokens.size() < 4) {
    llvm::errs() << "[APISpec] Warning: COPY for '" << spec.functionName
                 << "' requires 4 tokens (DstSel DstQual SrcSel SrcQual), got "
                 << tokens.size() << " — entry skipped\n";
    return false;
  }
  ValueSelector dstSel = parseSelector(tokens[0]);
  auto dstQ = parseQualifier(tokens[1]);
  ValueSelector srcSel = parseSelector(tokens[2]);
  auto srcQ = parseQualifier(tokens[3]);
  if (!dstSel.isValid || !srcSel.isValid) {
    llvm::errs() << "[APISpec] Warning: COPY for '" << spec.functionName
                 << "' has invalid selector — entry skipped\n";
    return false;
  }
  spec.copies.push_back(CopyEffect{dstSel, dstQ, srcSel, srcQ});
  return true;
}

// Marks a function as ignored in the specification.
void APISpec::applyIgnore(FunctionSpec &spec) { spec.isIgnored = true; }

// Marks a function as a deallocator in the specification.
void APISpec::applyDealloc(FunctionSpec &spec) { spec.isDeallocator = true; }

// Marks a function as an exit function in the specification.
void APISpec::applyExit(FunctionSpec &spec) { spec.isExit = true; }

// Applies MOD or REF operation to a function specification.
// Returns false and emits a warning if the token count is insufficient.
bool APISpec::applyModRef(FunctionSpec &spec, SpecOpKind op,
                          const std::vector<std::string> &tokens) {
  // Expect: (MOD|REF) <Sel> <Qual>
  if (tokens.size() < 2) {
    llvm::errs() << "[APISpec] Warning: MOD/REF for '" << spec.functionName
                 << "' requires 2 tokens (Sel Qual), got " << tokens.size()
                 << " — entry skipped\n";
    return false;
  }
  ValueSelector sel = parseSelector(tokens[0]);
  auto q = parseQualifier(tokens[1]);
  if (!sel.isValid) {
    llvm::errs() << "[APISpec] Warning: MOD/REF for '" << spec.functionName
                 << "' has invalid selector — entry skipped\n";
    return false;
  }
  spec.modref.push_back(ModRefEffect{op, sel, q});
  return true;
}

// Checks for conflicting flags on a FunctionSpec and warns.
static void warnConflicts(const FunctionSpec &spec) {
  if (spec.isIgnored &&
      (spec.isAllocator || spec.isDeallocator || spec.isExit ||
       !spec.copies.empty() || !spec.modref.empty())) {
    llvm::errs() << "[APISpec] Warning: function '" << spec.functionName
                 << "' is marked IGNORE but also has other effects — "
                    "IGNORE will take precedence for isIgnored queries\n";
  }
}

// Loads API specification from a single file.
bool APISpec::loadFile(const std::string &path, std::string &errorMessage) {
  errorMessage.clear();
  std::ifstream in(path);
  if (!in.is_open()) {
    errorMessage = "Failed to open spec file: " + path;
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    std::string func;
    SpecOpKind op;
    std::vector<std::string> toks;
    if (!parseLine(line, func, op, toks))
      continue;
    auto &spec = nameToSpec[func];
    if (spec.functionName.empty())
      spec.functionName = func;
    switch (op) {
    case SpecOpKind::Ignore:
      applyIgnore(spec);
      break;
    case SpecOpKind::Exit:
      applyExit(spec);
      break;
    case SpecOpKind::Alloc:
      applyAlloc(spec, toks);
      break;
    case SpecOpKind::Dealloc:
      applyDealloc(spec);
      break;
    case SpecOpKind::Copy:
      applyCopy(spec, toks);
      break;
    case SpecOpKind::Mod:
    case SpecOpKind::Ref:
      applyModRef(spec, op, toks);
      break;
    }
    warnConflicts(spec);
  }
  return true;
}

// Loads API specifications from multiple files.
bool APISpec::loadFiles(const std::vector<std::string> &paths,
                        std::string &errorMessage) {
  for (const auto &p : paths) {
    std::string err;
    if (!loadFile(p, err)) {
      errorMessage = err;
      return false;
    }
  }
  return true;
}

// Returns the function specification for the given function name.
const FunctionSpec *APISpec::get(const std::string &functionName) const {
  auto it = nameToSpec.find(functionName);
  if (it == nameToSpec.end())
    return nullptr;
  return &it->second;
}

// Returns true if the function is marked as ignored.
bool APISpec::isIgnored(const std::string &functionName) const {
  auto *s = get(functionName);
  return s && s->isIgnored;
}

// Returns true if the function is marked as an exit function.
bool APISpec::isExitLike(const std::string &functionName) const {
  auto *s = get(functionName);
  return s && s->isExit;
}

// Returns true if the function is marked as an allocator.
bool APISpec::isAllocatorLike(const std::string &functionName) const {
  auto *s = get(functionName);
  return s && s->isAllocator;
}

// Returns true if the function is marked as a deallocator.
bool APISpec::isDeallocatorLike(const std::string &functionName) const {
  auto *s = get(functionName);
  return s && s->isDeallocator;
}

// Returns the copy effects for the given function.
std::vector<CopyEffect>
APISpec::getCopies(const std::string &functionName) const {
  auto *s = get(functionName);
  if (!s)
    return {};
  return s->copies;
}

// Returns the mod/ref effects for the given function.
std::vector<ModRefEffect>
APISpec::getModRefs(const std::string &functionName) const {
  auto *s = get(functionName);
  if (!s)
    return {};
  return s->modref;
}

void APISpec::addOrReplaceSpec(const FunctionSpec &spec) {
  if (spec.functionName.empty())
    return;
  nameToSpec[spec.functionName] = spec;
}
