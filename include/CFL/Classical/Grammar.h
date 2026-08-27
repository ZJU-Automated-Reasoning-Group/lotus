#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace lotus::cfl::classical {

struct BinaryRule {
  std::string lhs;
  std::string first;
  std::string second;
};

class Grammar {
public:
  using ProductionMap =
      std::unordered_map<std::string, std::vector<std::vector<std::string>>>;

  static constexpr const char *kEpsilonSymbol = "<epsilon>";

  static Grammar parseFromFile(const std::string &path);
  static Grammar parseFromText(const std::string &text);

  const ProductionMap &productions() const { return productions_; }
  const std::vector<std::string> &nullableSymbols() const {
    return nullable_symbols_;
  }
  const std::unordered_map<std::string, std::vector<std::string>> &
  unaryByRhs() const {
    return unary_by_rhs_;
  }
  const std::unordered_map<std::string, std::vector<BinaryRule>> &
  binaryByFirst() const {
    return binary_by_first_;
  }
  const std::unordered_map<std::string, std::vector<BinaryRule>> &
  binaryBySecond() const {
    return binary_by_second_;
  }

private:
  Grammar() = default;

  void loadFromText(const std::string &text);
  void buildIndices();

  ProductionMap productions_;
  std::vector<std::string> nullable_symbols_;
  std::unordered_map<std::string, std::vector<std::string>> unary_by_rhs_;
  std::unordered_map<std::string, std::vector<BinaryRule>> binary_by_first_;
  std::unordered_map<std::string, std::vector<BinaryRule>> binary_by_second_;
  unsigned next_nonterminal_id_ = 0;
};

} // namespace lotus::cfl::classical
