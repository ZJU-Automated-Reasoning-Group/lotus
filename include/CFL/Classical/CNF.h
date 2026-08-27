#pragma once

#include <string>
#include <vector>

namespace lotus::cfl::classical {

struct CNFRule {
  std::string lhs;
  std::vector<std::string> rhs;

  bool operator==(const CNFRule &other) const {
    return lhs == other.lhs && rhs == other.rhs;
  }
};

class CNFGrammar {
public:
  static CNFGrammar loadFromFile(const std::string &path);
  static CNFGrammar transformToSTBDU(const std::string &path);

  const std::vector<std::string> &terminals() const { return terminals_; }
  const std::vector<std::string> &variables() const { return variables_; }
  const std::vector<CNFRule> &productions() const { return productions_; }

  void startTransform();
  void termTransform();
  void binTransform();
  void deleteEpsilonTransform();
  void unitTransform();

private:
  static std::vector<std::string> cleanAlphabet(const std::string &expression);
  static std::vector<CNFRule> cleanProduction(const std::string &expression);

  std::vector<std::string> terminals_;
  std::vector<std::string> variables_;
  std::vector<CNFRule> productions_;
  std::vector<std::string> variable_jar_;
};

} // namespace lotus::cfl::classical
