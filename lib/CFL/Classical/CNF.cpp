#include "CFL/Classical/CNF.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

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

bool isEpsilon(const std::vector<std::string> &rhs) {
  return rhs.size() == 1 && (rhs.front() == "e" || rhs.front() == "epsilon" ||
                             rhs.front() == "<epsilon>");
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
  grammar.terminals_ = cleanAlphabet(
      content.substr(std::string("Terminals:\n").size(),
                     variables_pos - std::string("Terminals:\n").size()));
  grammar.variables_ = cleanAlphabet(content.substr(
      variables_pos + std::string("Variables:\n").size(),
      productions_pos - (variables_pos + std::string("Variables:\n").size())));
  grammar.productions_ = cleanProduction(
      content.substr(productions_pos + std::string("Productions:\n").size()));
  if (grammar.variables_.empty()) {
    throw std::invalid_argument("CNF grammar has no start variable");
  }
  grammar.start_variable_ = grammar.variables_.front();

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
  const std::string original_start = start_variable_;
  std::string new_start = "S0";
  if (contains(variables_, new_start) || contains(terminals_, new_start)) {
    new_start = freshVariable();
  } else {
    variables_.push_back(new_start);
  }
  start_variable_ = new_start;
  productions_.insert(productions_.begin(),
                      CNFRule{new_start, {original_start}});
}

std::string CNFGrammar::freshVariable() {
  while (true) {
    const std::string candidate =
        "__cnf_" + std::to_string(++next_variable_id_);
    if (!contains(variables_, candidate) && !contains(terminals_, candidate)) {
      variables_.push_back(candidate);
      return candidate;
    }
  }
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
        const auto fresh = freshVariable();
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

    std::vector<std::string> fresh;
    fresh.reserve(size - 2);
    for (std::size_t i = 0; i < size - 2; ++i) {
      fresh.push_back(freshVariable());
    }
    transformed.push_back({production.lhs, {production.rhs[0], fresh[0]}});

    for (std::size_t i = 1; i < size - 2; ++i) {
      transformed.push_back({fresh[i - 1], {production.rhs[i], fresh[i]}});
    }

    transformed.push_back(
        {fresh.back(), {production.rhs[size - 2], production.rhs[size - 1]}});
  }

  productions_ = std::move(transformed);
}

void CNFGrammar::deleteEpsilonTransform() {
  std::unordered_set<std::string> nullable;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const CNFRule &production : productions_) {
      if (nullable.count(production.lhs) != 0) {
        continue;
      }
      const bool nullable_rhs =
          isEpsilon(production.rhs) ||
          (!production.rhs.empty() &&
           std::all_of(production.rhs.begin(), production.rhs.end(),
                       [&](const std::string &symbol) {
                         return nullable.count(symbol) != 0;
                       }));
      if (nullable_rhs) {
        nullable.insert(production.lhs);
        changed = true;
      }
    }
  }

  std::vector<CNFRule> transformed;
  for (const CNFRule &production : productions_) {
    if (isEpsilon(production.rhs)) {
      continue;
    }

    std::vector<std::string> rhs;
    std::function<void(std::size_t)> generate = [&](std::size_t index) {
      if (index == production.rhs.size()) {
        if (!rhs.empty()) {
          appendUnique(transformed, {production.lhs, rhs});
        } else if (production.lhs == start_variable_) {
          appendUnique(transformed, {production.lhs, {"e"}});
        }
        return;
      }
      rhs.push_back(production.rhs[index]);
      generate(index + 1);
      rhs.pop_back();
      if (nullable.count(production.rhs[index]) != 0) {
        generate(index + 1);
      }
    };
    generate(0);
  }

  if (nullable.count(start_variable_) != 0) {
    appendUnique(transformed, {start_variable_, {"e"}});
  }
  productions_ = std::move(transformed);
}

void CNFGrammar::unitTransform() {
  std::unordered_map<std::string, std::vector<std::string>> unit_edges;
  std::unordered_map<std::string, std::vector<CNFRule>> non_unit_by_lhs;
  for (const CNFRule &rule : productions_) {
    if (rule.rhs.size() == 1 && contains(variables_, rule.rhs.front())) {
      unit_edges[rule.lhs].push_back(rule.rhs.front());
    } else {
      non_unit_by_lhs[rule.lhs].push_back(rule);
    }
  }

  std::vector<CNFRule> transformed;
  for (const std::string &source : variables_) {
    std::deque<std::string> worklist{source};
    std::unordered_set<std::string> reachable{source};
    while (!worklist.empty()) {
      const std::string current = std::move(worklist.front());
      worklist.pop_front();
      if (const auto it = unit_edges.find(current); it != unit_edges.end()) {
        for (const std::string &target : it->second) {
          if (reachable.insert(target).second) {
            worklist.push_back(target);
          }
        }
      }
    }
    for (const std::string &target : reachable) {
      if (const auto it = non_unit_by_lhs.find(target);
          it != non_unit_by_lhs.end()) {
        for (const CNFRule &rule : it->second) {
          appendUnique(transformed, {source, rule.rhs});
        }
      }
    }
  }
  productions_ = std::move(transformed);
}

} // namespace lotus::cfl::classical
