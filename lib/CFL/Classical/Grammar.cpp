#include "CFL/Classical/Grammar.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
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
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Failed to open grammar file: " + path);
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();

  Grammar grammar;
  grammar.loadFromText(buffer.str());
  grammar.buildIndices();
  return grammar;
}

Grammar Grammar::parseFromText(const std::string &text) {
  Grammar grammar;
  grammar.loadFromText(text);
  grammar.buildIndices();
  return grammar;
}

void Grammar::loadFromText(const std::string &text) {
  const auto productions_pos = text.find("Productions:");
  if (productions_pos == std::string::npos) {
    throw std::invalid_argument(
        "Grammar file is missing a Productions section");
  }

  const auto production_blob =
      text.substr(productions_pos + std::string("Productions:").size());
  const auto raw_rules = split(production_blob, ';');
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
    const auto alternatives = split(rule_text.substr(arrow_pos + 2), '|');
    for (const auto &alternative : alternatives) {
      productions_[head].push_back(tokenize(alternative));
    }
  }

  std::unordered_map<std::string, std::string> replacement_cache;
  for (const auto &sign : {std::string("*"), std::string("?")}) {
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
            productions_[nonterminal] = {{kEpsilonSymbol}, expanded};
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
}

} // namespace lotus::cfl::classical
