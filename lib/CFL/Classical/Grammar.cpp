#include "CFL/Classical/Grammar.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace lotus::cfl::classical {
namespace {

std::string trim(const std::string &text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }

  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

std::vector<std::string> split(const std::string &text, char delimiter) {
  std::vector<std::string> parts;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, delimiter)) {
    parts.push_back(item);
  }
  return parts;
}

std::vector<std::string> tokenize(const std::string &text) {
  std::vector<std::string> tokens;
  std::stringstream stream(text);
  std::string token;
  while (stream >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

bool isEpsilon(const std::string &token) {
  return token == "epsilon" || token == "e" || token == Grammar::kEpsilonSymbol;
}

std::optional<char> attributeVariable(const std::string &token) {
  if (token.size() < 3 || token[token.size() - 2] != '_' ||
      !std::islower(static_cast<unsigned char>(token.back()))) {
    return std::nullopt;
  }
  return token.back();
}

std::vector<std::uint32_t> normalizedDomain(std::vector<std::uint32_t> domain) {
  std::sort(domain.begin(), domain.end());
  domain.erase(std::unique(domain.begin(), domain.end()), domain.end());
  return domain;
}

std::vector<std::uint32_t> attributeDomain(const std::vector<std::string> &rule,
                                           char variable,
                                           const GrammarParseOptions &options) {
  if (const auto it = options.variable_attributes.find(variable);
      it != options.variable_attributes.end()) {
    return normalizedDomain(it->second);
  }

  std::vector<std::vector<std::uint32_t>> observed_domains;
  for (const std::string &token : rule) {
    if (attributeVariable(token) != variable) {
      continue;
    }
    const std::string kind = token.substr(0, token.size() - 2);
    if (const auto it = options.symbol_attributes.find(kind);
        it != options.symbol_attributes.end() && !it->second.empty()) {
      observed_domains.push_back(normalizedDomain(it->second));
    }
  }

  if (observed_domains.empty()) {
    return normalizedDomain(options.attributes);
  }

  std::vector<std::uint32_t> domain = observed_domains.front();
  for (std::size_t index = 1; index < observed_domains.size(); ++index) {
    std::vector<std::uint32_t> intersection;
    std::set_intersection(
        domain.begin(), domain.end(), observed_domains[index].begin(),
        observed_domains[index].end(), std::back_inserter(intersection));
    domain = std::move(intersection);
  }
  return domain;
}

std::vector<std::vector<std::string>>
expandAttributes(const std::vector<std::string> &rule,
                 const GrammarParseOptions &options) {
  std::vector<char> variables;
  for (const std::string &token : rule) {
    const auto variable = attributeVariable(token);
    if (variable && std::find(variables.begin(), variables.end(), *variable) ==
                        variables.end()) {
      variables.push_back(*variable);
    }
  }
  if (variables.empty()) {
    return {rule};
  }

  std::vector<std::vector<std::string>> expanded{rule};
  for (char variable : variables) {
    const std::vector<std::uint32_t> domain =
        attributeDomain(rule, variable, options);
    if (domain.empty()) {
      throw std::invalid_argument(
          std::string("No attribute domain for grammar variable '") + variable +
          "'");
    }

    std::vector<std::vector<std::string>> next;
    for (const auto &candidate : expanded) {
      for (std::uint32_t attribute : domain) {
        auto instantiated = candidate;
        for (std::string &token : instantiated) {
          if (attributeVariable(token) == variable) {
            token.replace(token.size() - 1, 1, std::to_string(attribute));
          }
        }
        next.push_back(std::move(instantiated));
      }
    }
    expanded = std::move(next);
  }
  return expanded;
}

void parseDeclarationSections(const std::string &text, std::string &start,
                              std::unordered_set<std::string> &terminals,
                              std::unordered_set<std::string> &variables) {
  enum class Section { None, Start, Terminals, Variables };
  Section section = Section::None;
  std::stringstream stream(text.substr(0, text.find("Productions:")));
  std::string line;
  while (std::getline(stream, line)) {
    line = trim(line);
    if (line.empty()) {
      continue;
    }
    if (line == "Start:") {
      section = Section::Start;
      continue;
    }
    if (line == "Terminal:" || line == "Terminals:") {
      section = Section::Terminals;
      continue;
    }
    if (line == "Variable:" || line == "Variables:" ||
        line == "Nonterminals:") {
      section = Section::Variables;
      continue;
    }

    for (const std::string &token : tokenize(line)) {
      if (section == Section::Start && start.empty()) {
        start = token;
      } else if (section == Section::Terminals) {
        terminals.insert(token);
      } else if (section == Section::Variables) {
        variables.insert(token);
      }
    }
  }
}

std::size_t findMatchingLeftParen(const std::vector<std::string> &rule,
                                  std::size_t right_paren_index) {
  std::size_t depth = 0;
  for (std::size_t index = right_paren_index + 1; index > 0; --index) {
    const auto &token = rule[index - 1];
    if (token == ")") {
      ++depth;
    } else if (token == "(") {
      if (depth == 1) {
        return index - 1;
      }
      if (depth == 0) {
        break;
      }
      --depth;
    }
  }

  throw std::invalid_argument("Unmatched parenthesis in EBNF production");
}

std::string serializeRule(const std::vector<std::string> &rule) {
  std::ostringstream stream;
  for (std::size_t i = 0; i < rule.size(); ++i) {
    if (i != 0) {
      stream << ' ';
    }
    stream << rule[i];
  }
  return stream.str();
}

} // namespace

Grammar Grammar::parseFromFile(const std::string &path) {
  return parseFromFile(path, {});
}

Grammar Grammar::parseFromFile(const std::string &path,
                               const GrammarParseOptions &options) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Failed to open grammar file: " + path);
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();

  Grammar grammar;
  grammar.loadFromText(buffer.str(), options);
  grammar.buildIndices();
  return grammar;
}

Grammar Grammar::parseFromText(const std::string &text) {
  return parseFromText(text, {});
}

Grammar Grammar::parseFromText(const std::string &text,
                               const GrammarParseOptions &options) {
  Grammar grammar;
  grammar.loadFromText(text, options);
  grammar.buildIndices();
  return grammar;
}

void Grammar::loadFromText(const std::string &text,
                           const GrammarParseOptions &options) {
  const auto productions_pos = text.find("Productions:");
  if (productions_pos == std::string::npos) {
    throw std::invalid_argument(
        "Grammar file is missing a Productions section");
  }

  parseDeclarationSections(text, start_symbol_, terminals_, nonterminals_);
  for (const auto &[kind, domain] : options.symbol_attributes) {
    for (std::uint32_t attribute : normalizedDomain(domain)) {
      terminals_.insert(kind + '_' + std::to_string(attribute));
    }
  }

  const auto production_blob =
      text.substr(productions_pos + std::string("Productions:").size());
  const auto raw_rules = split(production_blob, ';');
  std::string first_head;
  for (const auto &raw_rule : raw_rules) {
    const auto rule_text = trim(raw_rule);
    if (rule_text.empty()) {
      continue;
    }

    const auto arrow_pos = rule_text.find("->");
    if (arrow_pos == std::string::npos) {
      throw std::invalid_argument("Invalid production rule: " + rule_text);
    }

    const auto head = trim(rule_text.substr(0, arrow_pos));
    if (head.empty()) {
      throw std::invalid_argument("Production has an empty head");
    }
    const auto alternatives = split(rule_text.substr(arrow_pos + 2), '|');
    for (const auto &alternative : alternatives) {
      auto rule = tokenize(alternative);
      for (std::string &token : rule) {
        if (isEpsilon(token)) {
          token = kEpsilonSymbol;
        }
      }
      std::vector<std::string> production{head};
      production.insert(production.end(), rule.begin(), rule.end());
      for (auto &expanded : expandAttributes(production, options)) {
        const std::string expanded_head = expanded.front();
        if (first_head.empty()) {
          first_head = expanded_head;
        }
        nonterminals_.insert(expanded_head);
        productions_[expanded_head].emplace_back(expanded.begin() + 1,
                                                 expanded.end());
      }
    }
  }
  if (start_symbol_.empty()) {
    start_symbol_ = first_head;
  }

  std::unordered_map<std::string, std::string> replacement_cache;
  for (const auto &sign : {std::string("*"), std::string("?")}) {
    ProductionMap pending_productions;
    for (auto &[head, rules] : productions_) {
      (void)head;
      for (auto &rule : rules) {
        for (std::size_t i = 0; i < rule.size(); ++i) {
          if (rule[i] != sign) {
            continue;
          }
          if (i == 0) {
            throw std::invalid_argument("Malformed EBNF postfix operator");
          }

          std::size_t start = i - 1;
          if (rule[i - 1] == ")") {
            start = findMatchingLeftParen(rule, i - 1);
          }

          const std::vector<std::string> repeated(
              rule.begin() + static_cast<long>(start),
              rule.begin() + static_cast<long>(i + 1));
          const auto repeated_key = serializeRule(repeated);

          auto it = replacement_cache.find(repeated_key);
          std::string nonterminal;
          if (it == replacement_cache.end()) {
            nonterminal = "X" + std::to_string(++next_nonterminal_id_);
            replacement_cache.emplace(repeated_key, nonterminal);

            std::vector<std::string> expanded;
            if (sign == "*") {
              expanded.push_back(nonterminal);
            }
            expanded.insert(expanded.end(), repeated.begin(),
                            repeated.end() - 1);
            pending_productions[nonterminal] = {{kEpsilonSymbol}, expanded};
          } else {
            nonterminal = it->second;
          }

          rule.erase(rule.begin() + static_cast<long>(start),
                     rule.begin() + static_cast<long>(i + 1));
          rule.insert(rule.begin() + static_cast<long>(start), nonterminal);
          i = start;
        }
      }
    }
    for (auto &[head, rules] : pending_productions) {
      productions_[head] = std::move(rules);
    }
  }

  for (auto &[head, rules] : productions_) {
    (void)head;
    for (auto &rule : rules) {
      rule.erase(std::remove_if(rule.begin(), rule.end(),
                                [](const std::string &token) {
                                  return token == "(" || token == ")";
                                }),
                 rule.end());
    }
  }

  ProductionMap binary_productions;
  for (const auto &[head, rules] : productions_) {
    for (const auto &rule : rules) {
      if (rule.size() <= 2) {
        binary_productions[head].push_back(rule);
        continue;
      }

      std::string current_head = head;
      for (std::size_t i = 0; i + 2 < rule.size(); ++i) {
        const auto fresh = "X" + std::to_string(++next_nonterminal_id_);
        binary_productions[current_head].push_back({rule[i], fresh});
        current_head = fresh;
      }
      binary_productions[current_head].push_back(
          {rule[rule.size() - 2], rule[rule.size() - 1]});
    }
  }

  productions_ = std::move(binary_productions);

  for (const auto &[head, rules] : productions_) {
    nonterminals_.insert(head);
    for (const auto &rule : rules) {
      for (const std::string &symbol : rule) {
        if (symbol != kEpsilonSymbol && nonterminals_.count(symbol) == 0) {
          terminals_.insert(symbol);
        }
      }
    }
  }

  nullable_symbols_.clear();
  for (const auto &[head, rules] : productions_) {
    for (const auto &rule : rules) {
      if (rule.size() == 1 && rule.front() == kEpsilonSymbol) {
        nullable_symbols_.push_back(head);
        break;
      }
    }
  }
}

void Grammar::buildIndices() {
  unary_by_rhs_.clear();
  binary_by_first_.clear();
  binary_by_second_.clear();

  for (const auto &[head, rules] : productions_) {
    for (const auto &rule : rules) {
      if (rule.size() == 1 && rule.front() != kEpsilonSymbol) {
        unary_by_rhs_[rule.front()].push_back(head);
        continue;
      }

      if (rule.size() == 2) {
        BinaryRule binary_rule{head, rule[0], rule[1]};
        binary_by_first_[rule[0]].push_back(binary_rule);
        binary_by_second_[rule[1]].push_back(binary_rule);
      }
    }
  }

  buildSymbolTable();
}

void Grammar::buildSymbolTable() {
  symbol_names_.clear();
  symbol_ids_.clear();
  nullable_symbol_ids_.clear();
  unary_by_rhs_id_.clear();
  binary_by_first_id_.clear();
  binary_by_second_id_.clear();
  transitive_symbols_.clear();

  auto intern = [&](const std::string &name) {
    if (symbol_ids_.count(name) != 0) {
      return;
    }
    const auto id = static_cast<SymbolId>(symbol_names_.size());
    symbol_ids_.emplace(name, id);
    symbol_names_.push_back(name);
  };
  for (const std::string &symbol : nonterminals_) {
    intern(symbol);
  }
  for (const std::string &symbol : terminals_) {
    intern(symbol);
  }
  if (!start_symbol_.empty()) {
    intern(start_symbol_);
    start_symbol_id_ = symbol_ids_.at(start_symbol_);
  }

  for (const std::string &symbol : nullable_symbols_) {
    nullable_symbol_ids_.push_back(symbol_ids_.at(symbol));
  }
  for (const auto &[rhs, heads] : unary_by_rhs_) {
    auto &compiled = unary_by_rhs_id_[symbol_ids_.at(rhs)];
    for (const std::string &head : heads) {
      compiled.push_back(symbol_ids_.at(head));
    }
  }
  for (const auto &[first, rules] : binary_by_first_) {
    auto &compiled = binary_by_first_id_[symbol_ids_.at(first)];
    for (const BinaryRule &rule : rules) {
      BinaryRuleId value{symbol_ids_.at(rule.lhs), symbol_ids_.at(rule.first),
                         symbol_ids_.at(rule.second)};
      compiled.push_back(value);
      if (value.lhs == value.first && value.lhs == value.second) {
        transitive_symbols_.insert(value.lhs);
      }
    }
  }
  for (const auto &[second, rules] : binary_by_second_) {
    auto &compiled = binary_by_second_id_[symbol_ids_.at(second)];
    for (const BinaryRule &rule : rules) {
      compiled.push_back({symbol_ids_.at(rule.lhs), symbol_ids_.at(rule.first),
                          symbol_ids_.at(rule.second)});
    }
  }
}

bool Grammar::isTerminal(const std::string &symbol) const {
  return terminals_.count(symbol) != 0;
}

bool Grammar::isNonterminal(const std::string &symbol) const {
  return nonterminals_.count(symbol) != 0;
}

bool Grammar::hasSymbol(const std::string &symbol) const {
  return symbol_ids_.count(symbol) != 0;
}

SymbolId Grammar::symbolId(const std::string &symbol) const {
  const auto it = symbol_ids_.find(symbol);
  if (it == symbol_ids_.end()) {
    throw std::out_of_range("Unknown grammar symbol: " + symbol);
  }
  return it->second;
}

const std::string &Grammar::symbolName(SymbolId symbol) const {
  return symbol_names_.at(symbol);
}

std::size_t Grammar::productionCount() const {
  std::size_t count = 0;
  for (const auto &[_, rules] : productions_) {
    count += rules.size();
  }
  return count;
}

std::vector<GrammarIssue> Grammar::validate() const {
  std::vector<GrammarIssue> issues;
  if (start_symbol_.empty()) {
    issues.push_back(
        {GrammarIssueSeverity::Error, "Grammar has no start symbol"});
  } else if (!isNonterminal(start_symbol_)) {
    issues.push_back({GrammarIssueSeverity::Error,
                      "Start symbol is not a nonterminal: " + start_symbol_});
  }
  if (productions_.empty()) {
    issues.push_back(
        {GrammarIssueSeverity::Error, "Grammar has no productions"});
  }
  for (const std::string &terminal : terminals_) {
    if (attributeVariable(terminal)) {
      issues.push_back({GrammarIssueSeverity::Error,
                        "Uninstantiated attributed symbol: " + terminal});
    }
  }
  return issues;
}

} // namespace lotus::cfl::classical
