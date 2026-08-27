#include "CFL/Classical/CNF.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

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

std::vector<std::string> split(const std::string &text,
                               const std::string &delimiter) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (true) {
    const auto pos = text.find(delimiter, start);
    if (pos == std::string::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, pos - start));
    start = pos + delimiter.size();
  }
}

bool contains(const std::vector<std::string> &items, const std::string &value) {
  return std::find(items.begin(), items.end(), value) != items.end();
}

void appendUnique(std::vector<CNFRule> &rules, const CNFRule &rule) {
  if (std::find(rules.begin(), rules.end(), rule) == rules.end()) {
    rules.push_back(rule);
  }
}

std::vector<std::string> buildVariableJar() {
  std::vector<std::string> jar;
  for (char ch = 'A'; ch <= 'Z'; ++ch) {
    if (ch == 'V') {
      continue;
    }
    jar.push_back(std::string(1, ch));
  }
  return jar;
}

} // namespace

CNFGrammar CNFGrammar::loadFromFile(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Failed to open CNF grammar file: " + path);
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  const auto content = buffer.str();

  const auto variables_pos = content.find("Variables:\n");
  const auto productions_pos = content.find("Productions:\n");
  if (variables_pos == std::string::npos ||
      productions_pos == std::string::npos) {
    throw std::invalid_argument(
        "Malformed grammar model for CNF transformation");
  }

  CNFGrammar grammar;
  grammar.variable_jar_ = buildVariableJar();
  grammar.terminals_ = cleanAlphabet(
      content.substr(std::string("Terminals:\n").size(),
                     variables_pos - std::string("Terminals:\n").size()));
  grammar.variables_ = cleanAlphabet(content.substr(
      variables_pos + std::string("Variables:\n").size(),
      productions_pos - (variables_pos + std::string("Variables:\n").size())));
  grammar.productions_ = cleanProduction(
      content.substr(productions_pos + std::string("Productions:\n").size()));

  for (const auto &variable : grammar.variables_) {
    grammar.variable_jar_.erase(std::remove(grammar.variable_jar_.begin(),
                                            grammar.variable_jar_.end(),
                                            variable),
                                grammar.variable_jar_.end());
  }

  return grammar;
}

CNFGrammar CNFGrammar::transformToSTBDU(const std::string &path) {
  auto grammar = loadFromFile(path);
  grammar.startTransform();
  grammar.termTransform();
  grammar.binTransform();
  grammar.deleteEpsilonTransform();
  grammar.unitTransform();
  return grammar;
}

std::vector<std::string>
CNFGrammar::cleanAlphabet(const std::string &expression) {
  std::stringstream stream(trim(expression));
  std::vector<std::string> tokens;
  std::string token;
  while (stream >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

std::vector<CNFRule>
CNFGrammar::cleanProduction(const std::string &expression) {
  std::vector<CNFRule> rules;
  for (const auto &raw_rule : split(expression, ";")) {
    const auto rule = trim(raw_rule);
    if (rule.empty()) {
      continue;
    }

    const auto arrow_pos = rule.find(" -> ");
    if (arrow_pos == std::string::npos) {
      throw std::invalid_argument("Malformed production: " + rule);
    }

    const auto lhs = trim(rule.substr(0, arrow_pos));
    for (const auto &raw_rhs : split(rule.substr(arrow_pos + 4), " | ")) {
      std::stringstream stream(trim(raw_rhs));
      std::vector<std::string> rhs;
      std::string symbol;
      while (stream >> symbol) {
        rhs.push_back(symbol);
      }
      rules.push_back({lhs, rhs});
    }
  }
  return rules;
}

void CNFGrammar::startTransform() {
  variables_.push_back("S0");
  productions_.insert(productions_.begin(),
                      CNFRule{"S0", {variables_.front()}});
}

void CNFGrammar::termTransform() {
  std::unordered_map<std::string, std::string> dictionary;
  for (const auto &production : productions_) {
    if (production.rhs.size() == 1 && contains(variables_, production.lhs) &&
        contains(terminals_, production.rhs.front())) {
      dictionary[production.rhs.front()] = production.lhs;
    }
  }

  std::vector<CNFRule> rewritten;
  for (auto production : productions_) {
    const bool is_simple = production.rhs.size() == 1 &&
                           contains(variables_, production.lhs) &&
                           contains(terminals_, production.rhs.front());
    if (is_simple) {
      rewritten.push_back(production);
      continue;
    }

    for (auto &symbol : production.rhs) {
      if (!contains(terminals_, symbol)) {
        continue;
      }

      auto it = dictionary.find(symbol);
      if (it == dictionary.end()) {
        if (variable_jar_.empty()) {
          throw std::runtime_error("CNF variable jar exhausted");
        }

        const auto fresh = variable_jar_.back();
        variable_jar_.pop_back();
        variables_.push_back(fresh);
        rewritten.push_back({fresh, {symbol}});
        dictionary[symbol] = fresh;
        symbol = fresh;
      } else {
        symbol = it->second;
      }
    }

    rewritten.push_back(production);
  }

  productions_ = std::move(rewritten);
}

void CNFGrammar::binTransform() {
  std::vector<CNFRule> transformed;
  for (const auto &production : productions_) {
    const auto size = production.rhs.size();
    if (size <= 2) {
      transformed.push_back(production);
      continue;
    }

    if (variable_jar_.empty()) {
      throw std::runtime_error("CNF variable jar exhausted");
    }

    const auto base = variable_jar_.front();
    variable_jar_.erase(variable_jar_.begin());
    variables_.push_back(base + "1");
    transformed.push_back({production.lhs, {production.rhs[0], base + "1"}});

    for (std::size_t i = 1; i < size - 2; ++i) {
      const auto current = base + std::to_string(i);
      const auto next = base + std::to_string(i + 1);
      variables_.push_back(next);
      transformed.push_back({current, {production.rhs[i], next}});
    }

    transformed.push_back(
        {base + std::to_string(size - 2),
         {production.rhs[size - 2], production.rhs[size - 1]}});
  }

  productions_ = std::move(transformed);
}

void CNFGrammar::deleteEpsilonTransform() {
  std::vector<std::string> outlaws;
  std::vector<CNFRule> preserved;
  for (const auto &production : productions_) {
    if (production.rhs.size() == 1 && production.rhs.front() == "e") {
      outlaws.push_back(production.lhs);
    } else {
      preserved.push_back(production);
    }
  }

  std::vector<CNFRule> generated;
  for (const auto &outlaw : outlaws) {
    std::vector<CNFRule> candidates = preserved;
    candidates.insert(candidates.end(), generated.begin(), generated.end());
    for (const auto &production : candidates) {
      std::vector<std::size_t> positions;
      for (std::size_t i = 0; i < production.rhs.size(); ++i) {
        if (production.rhs[i] == outlaw) {
          positions.push_back(i);
        }
      }

      const std::size_t combinations = std::size_t{1} << positions.size();
      for (std::size_t mask = 0; mask < combinations; ++mask) {
        std::vector<std::string> rhs;
        for (std::size_t i = 0; i < production.rhs.size(); ++i) {
          auto it = std::find(positions.begin(), positions.end(), i);
          if (it != positions.end()) {
            const auto bit =
                static_cast<std::size_t>(std::distance(positions.begin(), it));
            if (((mask >> bit) & 1U) != 0U) {
              continue;
            }
          }
          rhs.push_back(production.rhs[i]);
        }
        if (!rhs.empty()) {
          appendUnique(generated, {production.lhs, rhs});
        }
      }
    }
  }

  productions_ = generated;
  for (const auto &production : preserved) {
    appendUnique(productions_, production);
  }
}

void CNFGrammar::unitTransform() {
  auto unit_routine = [&](const std::vector<CNFRule> &rules) {
    std::vector<std::pair<std::string, std::string>> unit_rules;
    std::vector<CNFRule> result;
    for (const auto &rule : rules) {
      if (rule.rhs.size() == 1 && contains(variables_, rule.lhs) &&
          contains(variables_, rule.rhs.front())) {
        unit_rules.push_back({rule.lhs, rule.rhs.front()});
      } else {
        result.push_back(rule);
      }
    }

    for (const auto &[lhs, rhs] : unit_rules) {
      for (const auto &rule : rules) {
        if (rule.lhs == rhs && lhs != rule.lhs) {
          result.push_back({lhs, rule.rhs});
        }
      }
    }
    return result;
  };

  auto result = unit_routine(productions_);
  auto next = unit_routine(result);
  for (int i = 0; i < 1000 && result != next; ++i) {
    result = unit_routine(next);
    next = unit_routine(result);
  }
  productions_ = std::move(next);
}

} // namespace lotus::cfl::classical
