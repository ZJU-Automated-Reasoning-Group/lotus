#pragma once

#include "CFL/Classical/Relation.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::cfl::classical {

struct BinaryRule {
  std::string lhs;
  std::string first;
  std::string second;
};

struct BinaryRuleId {
  SymbolId lhs = 0;
  SymbolId first = 0;
  SymbolId second = 0;
};

struct GrammarParseOptions {
  /// Global default domain used only when no variable or symbol-specific
  /// domain is available.
  std::vector<std::uint32_t> attributes;
  /// Explicit domain for a grammar variable such as i in call_i/ret_i.
  std::unordered_map<char, std::vector<std::uint32_t>> variable_attributes;
  /// Observed domain per symbol kind, e.g. call -> {1, 2}, gep -> {0, 4}.
  std::unordered_map<std::string, std::vector<std::uint32_t>> symbol_attributes;
};

enum class GrammarIssueSeverity {
  Warning,
  Error,
};

struct GrammarIssue {
  GrammarIssueSeverity severity = GrammarIssueSeverity::Error;
  std::string message;
};

class Grammar {
public:
  using ProductionMap =
      std::unordered_map<std::string, std::vector<std::vector<std::string>>>;

  static constexpr const char *kEpsilonSymbol = "<epsilon>";

  static Grammar parseFromFile(const std::string &path);
  static Grammar parseFromFile(const std::string &path,
                               const GrammarParseOptions &options);
  static Grammar parseFromText(const std::string &text);
  static Grammar parseFromText(const std::string &text,
                               const GrammarParseOptions &options);

  const std::string &startSymbol() const { return start_symbol_; }
  SymbolId startSymbolId() const { return start_symbol_id_; }
  bool isTerminal(const std::string &symbol) const;
  bool isNonterminal(const std::string &symbol) const;
  bool hasSymbol(const std::string &symbol) const;
  SymbolId symbolId(const std::string &symbol) const;
  const std::string &symbolName(SymbolId symbol) const;
  std::size_t symbolCount() const { return symbol_names_.size(); }
  std::size_t productionCount() const;
  const std::unordered_set<std::string> &terminals() const {
    return terminals_;
  }
  const std::unordered_set<std::string> &nonterminals() const {
    return nonterminals_;
  }
  std::vector<GrammarIssue> validate() const;

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
  const std::vector<SymbolId> &nullableSymbolIds() const {
    return nullable_symbol_ids_;
  }
  const std::unordered_map<SymbolId, std::vector<SymbolId>> &
  unaryByRhsId() const {
    return unary_by_rhs_id_;
  }
  const std::unordered_map<SymbolId, std::vector<BinaryRuleId>> &
  binaryByFirstId() const {
    return binary_by_first_id_;
  }
  const std::unordered_map<SymbolId, std::vector<BinaryRuleId>> &
  binaryBySecondId() const {
    return binary_by_second_id_;
  }
  const std::unordered_set<SymbolId> &transitiveSymbols() const {
    return transitive_symbols_;
  }

private:
  Grammar() = default;

  void loadFromText(const std::string &text,
                    const GrammarParseOptions &options);
  void buildIndices();
  void buildSymbolTable();

  ProductionMap productions_;
  std::vector<std::string> nullable_symbols_;
  std::unordered_map<std::string, std::vector<std::string>> unary_by_rhs_;
  std::unordered_map<std::string, std::vector<BinaryRule>> binary_by_first_;
  std::unordered_map<std::string, std::vector<BinaryRule>> binary_by_second_;
  std::string start_symbol_;
  std::unordered_set<std::string> terminals_;
  std::unordered_set<std::string> nonterminals_;
  std::vector<std::string> symbol_names_;
  std::unordered_map<std::string, SymbolId> symbol_ids_;
  SymbolId start_symbol_id_ = 0;
  std::vector<SymbolId> nullable_symbol_ids_;
  std::unordered_map<SymbolId, std::vector<SymbolId>> unary_by_rhs_id_;
  std::unordered_map<SymbolId, std::vector<BinaryRuleId>> binary_by_first_id_;
  std::unordered_map<SymbolId, std::vector<BinaryRuleId>> binary_by_second_id_;
  std::unordered_set<SymbolId> transitive_symbols_;
  unsigned next_nonterminal_id_ = 0;
};

} // namespace lotus::cfl::classical
