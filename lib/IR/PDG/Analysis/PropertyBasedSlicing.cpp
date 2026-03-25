#include "IR/PDG/Analysis/PropertyBasedSlicing.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

using namespace llvm;

namespace pdg {

namespace {

static std::string trim(const std::string &s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
    ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
    --e;
  return s.substr(b, e - b);
}

static std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static bool hasToken(StringRef line, StringRef token) {
  return line.find(token) != StringRef::npos;
}

static bool startsWithCI(StringRef s, StringRef prefix) {
  if (s.size() < prefix.size())
    return false;
  return toLower(s.take_front(prefix.size()).str()) == toLower(prefix.str());
}

static bool hasKeyword(StringRef text, StringRef keyword) {
  std::string lowerText = toLower(text.str());
  std::string lowerKeyword = toLower(keyword.str());
  return hasToken(lowerText, lowerKeyword);
}

static bool containsInitMain(StringRef text) {
  std::string compact;
  compact.reserve(text.size());
  for (char c : text) {
    if (!std::isspace(static_cast<unsigned char>(c)))
      compact.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return hasToken(compact, "init(main())");
}

static bool extractBalancedContent(StringRef original, StringRef lower,
                                   StringRef keywordWithParen,
                                   std::string &out) {
  size_t pos = lower.find(keywordWithParen);
  if (pos == StringRef::npos)
    return false;

  size_t i = pos + keywordWithParen.size();
  int depth = 1;
  size_t begin = i;
  while (i < original.size()) {
    char c = original[i];
    if (c == '(')
      ++depth;
    else if (c == ')') {
      --depth;
      if (depth == 0) {
        out = trim(original.substr(begin, i - begin).str());
        return true;
      }
    }
    ++i;
  }
  return false;
}

static bool parseCallTarget(StringRef text, std::string &targetOut) {
  std::string s = text.str();
  std::string lower = toLower(s);
  size_t pos = 0;

  while (true) {
    pos = lower.find("call", pos);
    if (pos == std::string::npos)
      return false;

    if (pos > 0) {
      char prev = lower[pos - 1];
      if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_') {
        ++pos;
        continue;
      }
    }

    size_t i = pos + 4;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
      ++i;
    if (i >= s.size() || s[i] != '(') {
      ++pos;
      continue;
    }
    ++i;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
      ++i;

    std::string target;
    if (i < s.size() && s[i] == '"') {
      ++i;
      size_t start = i;
      while (i < s.size() && s[i] != '"')
        ++i;
      if (i >= s.size())
        return false;
      target = s.substr(start, i - start);
      ++i;
    } else {
      size_t start = i;
      while (i < s.size()) {
        char c = s[i];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
            c == '.' || c == '$')
          ++i;
        else
          break;
      }
      if (i == start)
        return false;
      target = s.substr(start, i - start);
    }

    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
      ++i;

    if (i < s.size() && s[i] == '(') {
      ++i;
      while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
      if (i >= s.size() || s[i] != ')')
        return false;
      ++i;
      while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    }

    if (i >= s.size() || s[i] != ')')
      return false;

    targetOut = trim(target);
    return !targetOut.empty();
  }
}

static bool isStructuredPropertyLine(StringRef line) {
  std::string lower = toLower(trim(line.str()));
  return startsWithCI(lower, "check(") || startsWithCI(lower, "cover(");
}

static PropertyKind parsePropertyName(StringRef name) {
  std::string lower = toLower(name.str());
  if (lower == "valid-deref" || lower == "valid-free" ||
      lower == "valid-memtrack" || lower == "valid-memsafety" ||
      lower == "memsafety")
    return PropertyKind::MemSafety;
  if (lower == "overflow" || lower == "no-overflow" ||
      lower == "signed-overflow")
    return PropertyKind::NoOverflow;
  if (lower == "termination" || lower == "end")
    return PropertyKind::Termination;
  if (lower == "null-deref")
    return PropertyKind::NullDeref;
  if (lower == "def-behavior" || lower == "undefined-behavior" ||
      lower == "undef-behavior" || lower == "undefined")
    return PropertyKind::DefBehavior;
  if (lower == "valid-memcleanup" || lower == "memcleanup")
    return PropertyKind::MemCleanup;
  if (lower == "coverage-error-call" || lower == "cover-error")
    return PropertyKind::CoverageErrorCall;
  if (lower == "coverage-branches" || lower == "@decisionedge" ||
      lower == "cover-branches")
    return PropertyKind::CoverageBranches;
  if (lower == "coverage-statements" || lower == "@basicblockentry" ||
      lower == "cover-statements" || lower == "coverage")
    return PropertyKind::CoverageStatements;
  if (lower == "coverage-conditions" || lower == "@conditionedge" ||
      lower == "cover-conditions")
    return PropertyKind::CoverageConditions;
  // Note: "assert" and "assertions" map to PropertyKind::Assertions.
  // This is handled separately in parseFromString fallback parsing.
  return PropertyKind::Unknown;
}

static bool parseLTLLine(const std::string &line, PropertyRule &rule,
                         std::string &error) {
  const std::string lineTrim = trim(line);
  std::string lowerLine = toLower(lineTrim);

  bool isCheck = startsWithCI(lowerLine, "check(");
  bool isCover = startsWithCI(lowerLine, "cover(");
  if (!isCheck && !isCover) {
    error = "expected CHECK or COVER";
    return false;
  }
  if (!containsInitMain(lineTrim)) {
    error = "missing init(main())";
    return false;
  }

  rule.type = isCheck ? PropertyType::CHECK : PropertyType::COVER;

  std::string content;
  bool isFQL = false;
  if (extractBalancedContent(lineTrim, lowerLine, "fql(", content)) {
    isFQL = true;
  } else if (!extractBalancedContent(lineTrim, lowerLine, "ltl(", content)) {
    error = "missing LTL(...) or FQL(...) expression";
    return false;
  }

  const std::string lowerContent = toLower(content);
  rule.negated = hasToken(lowerContent, "!");

  if (isFQL) {
    if (hasKeyword(lowerContent, "@basicblockentry") ||
        hasKeyword(lowerContent, "coverage-statements")) {
      rule.kind = PropertyKind::CoverageStatements;
      return true;
    }
    if (hasKeyword(lowerContent, "@decisionedge") ||
        hasKeyword(lowerContent, "coverage-branches")) {
      rule.kind = PropertyKind::CoverageBranches;
      return true;
    }
    if (hasKeyword(lowerContent, "@conditionedge") ||
        hasKeyword(lowerContent, "coverage-conditions")) {
      rule.kind = PropertyKind::CoverageConditions;
      return true;
    }

    std::string callTarget;
    if (parseCallTarget(content, callTarget)) {
      rule.kind = PropertyKind::CoverageErrorCall;
      rule.target = callTarget;
      return true;
    }
  } else {
    std::string callTarget;
    if (parseCallTarget(content, callTarget)) {
      rule.kind = PropertyKind::UnreachCall;
      rule.target = callTarget;
      return true;
    }

    for (const auto &pair : {
             std::pair<std::string, PropertyKind>{"valid-deref",
                                                  PropertyKind::MemSafety},
             std::pair<std::string, PropertyKind>{"valid-free",
                                                  PropertyKind::MemSafety},
             std::pair<std::string, PropertyKind>{"valid-memtrack",
                                                  PropertyKind::MemSafety},
             std::pair<std::string, PropertyKind>{"valid-memsafety",
                                                  PropertyKind::MemSafety},
             std::pair<std::string, PropertyKind>{"overflow",
                                                  PropertyKind::NoOverflow},
             std::pair<std::string, PropertyKind>{"termination",
                                                  PropertyKind::Termination},
             std::pair<std::string, PropertyKind>{"end",
                                                  PropertyKind::Termination},
             std::pair<std::string, PropertyKind>{"null-deref",
                                                  PropertyKind::NullDeref},
             std::pair<std::string, PropertyKind>{"def-behavior",
                                                  PropertyKind::DefBehavior},
             std::pair<std::string, PropertyKind>{"valid-memcleanup",
                                                  PropertyKind::MemCleanup},
         }) {
      if (hasKeyword(lowerContent, pair.first)) {
        rule.kind = pair.second;
        return true;
      }
    }
  }

  error = "could not identify property kind";
  return false;
}

static bool isMemSafetyInstruction(const Instruction &I) {
  if (isa<LoadInst>(&I) || isa<StoreInst>(&I))
    return true;

  const auto *CB = dyn_cast<CallBase>(&I);
  if (!CB)
    return false;
  const Function *Callee = CB->getCalledFunction();
  if (!Callee)
    return false;

  const StringRef N = Callee->getName();
  return N == "malloc" || N == "calloc" || N == "realloc" || N == "free" ||
         N == "__VERIFIER_error";
}

static bool isNullDerefInstruction(const Instruction &I) {
  if (isa<LoadInst>(&I) || isa<StoreInst>(&I)) {
    // Check if pointer operand might be null
    return true;
  }
  const auto *CB = dyn_cast<CallBase>(&I);
  if (!CB)
    return false;
  const Function *Callee = CB->getCalledFunction();
  if (!Callee)
    return false;
  const StringRef N = Callee->getName();
  return N == "__VERIFIER_error";
}

static bool isSignedArithInstruction(const Instruction &I) {
  const auto *BO = dyn_cast<BinaryOperator>(&I);
  if (!BO)
    return false;
  if (!BO->getType()->isIntegerTy() || BO->getType()->isIntegerTy(1))
    return false;
  const unsigned Op = BO->getOpcode();
  return Op == Instruction::Add || Op == Instruction::Sub ||
         Op == Instruction::Mul;
}

static bool isDefBehaviorInstruction(const Instruction &I) {
  // Undefined behavior can occur at various instructions
  const auto *BO = dyn_cast<BinaryOperator>(&I);
  if (BO) {
    // Division by zero, shift overflow, etc.
    if (BO->getOpcode() == Instruction::UDiv ||
        BO->getOpcode() == Instruction::SDiv ||
        BO->getOpcode() == Instruction::URem ||
        BO->getOpcode() == Instruction::SRem)
      return true;
    if (BO->getOpcode() == Instruction::Shl ||
        BO->getOpcode() == Instruction::LShr ||
        BO->getOpcode() == Instruction::AShr)
      return true;
  }
  return isNullDerefInstruction(I) || isSignedArithInstruction(I);
}

static bool moduleHasAnyCall(const Module &M,
                             const std::vector<std::string> &funcNames) {
  for (const Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        const auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;
        const Function *Callee = CB->getCalledFunction();
        if (!Callee)
          continue;
        const std::string calleeName = toLower(Callee->getName().str());
        for (const std::string &name : funcNames) {
          if (calleeName == toLower(name))
            return true;
        }
      }
    }
  }
  return false;
}

} // namespace

bool PropertySpec::parseFromFile(const std::string &path, PropertySpec &out,
                                 std::string &error) {
  std::ifstream in(path);
  if (!in.is_open()) {
    error = "cannot open property file: " + path;
    return false;
  }

  std::stringstream buffer;
  buffer << in.rdbuf();
  return parseFromString(buffer.str(), out, error);
}

bool PropertySpec::parseFromString(const std::string &content,
                                   PropertySpec &out, std::string &error) {
  PropertySpec parsed;
  std::istringstream stream(content);
  std::string line;
  size_t lineno = 0;

  while (std::getline(stream, line)) {
    ++lineno;
    line = trim(line);
    if (line.empty() || line[0] == '#')
      continue;

    PropertyRule rule;
    std::string ruleError;
    if (!parseLTLLine(line, rule, ruleError)) {
      if (isStructuredPropertyLine(line)) {
        error = "line " + std::to_string(lineno) + ": " + ruleError;
        return false;
      }

      // Try fallback simple parsing
      const std::string lower = toLower(line);

      // Handle "assert" and "assertions" keywords exactly.
      const std::string simple = trim(lower);
      if (simple == "assert" || simple == "assertions") {
        rule.type = PropertyType::CHECK;
        rule.kind = PropertyKind::Assertions;
        parsed._rules.push_back(rule);
        continue;
      }

      // Try parsing as property name
      PropertyKind fallbackKind = parsePropertyName(simple);
      if (fallbackKind != PropertyKind::Unknown) {
        rule.type = PropertyType::CHECK;
        rule.kind = fallbackKind;
        parsed._rules.push_back(rule);
        continue;
      }

      // Handle call() pattern
      if (hasToken(lower, "call(")) {
        rule.type = PropertyType::CHECK;
        if (!parseCallTarget(lower, rule.target)) {
          error = "invalid call() target at line " + std::to_string(lineno);
          return false;
        }
        rule.kind = PropertyKind::UnreachCall;
        parsed._rules.push_back(rule);
        continue;
      }

      error =
          "line " + std::to_string(lineno) + ": unsupported property syntax";
      return false;
    }

    parsed._rules.push_back(rule);
    if (parsed._type == PropertyType::CHECK && rule.type == PropertyType::COVER)
      parsed._type = PropertyType::COVER;
    else if (parsed._type == PropertyType::COVER &&
             rule.type == PropertyType::CHECK) {
      error = "mixing CHECK and COVER properties not supported";
      return false;
    }
  }

  if (parsed._rules.empty()) {
    error = "property file has no recognized rules";
    return false;
  }

  out = std::move(parsed);
  return true;
}

} // namespace pdg
