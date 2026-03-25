#pragma once

#include <string>
#include <vector>

namespace pdg {

enum class PropertyKind {
  UnreachCall,
  Assertions,
  MemSafety,
  NoOverflow,
  Termination,
  CoverageErrorCall,
  CoverageBranches,
  CoverageStatements,
  CoverageConditions,
  NullDeref,
  DefBehavior,
  MemCleanup,
  Unknown
};

enum class PropertyType {
  CHECK,
  COVER
};

struct PropertyRule {
  PropertyType type = PropertyType::CHECK;
  PropertyKind kind = PropertyKind::Unknown;
  std::string target;
  bool negated = false;
};

class PropertySpec {
public:
  static bool parseFromFile(const std::string &path, PropertySpec &out,
                            std::string &error);
  static bool parseFromString(const std::string &content, PropertySpec &out,
                              std::string &error);

  const std::vector<PropertyRule> &rules() const { return _rules; }
  bool empty() const { return _rules.empty(); }
  PropertyType getType() const { return _type; }

private:
  std::vector<PropertyRule> _rules;
  PropertyType _type = PropertyType::CHECK;

  friend class PropertyParser;
};

} // namespace pdg
