#include "IR/PDG/Analysis/CypherQuery.h"

#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include "IR/PDG/Analysis/PDGQuery.h"
#include "IR/PDG/Support/DebugInfoUtils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <sstream>

namespace pdg {

static std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static std::string unquoteStringToken(std::string token) {
  if (token.size() >= 2 && ((token.front() == '"' && token.back() == '"') ||
                            (token.front() == '\'' && token.back() == '\''))) {
    token = token.substr(1, token.size() - 2);
  }
  return token;
}

static const char *graphNodeTypeName(GraphNodeType t) {
  switch (t) {
  case GraphNodeType::INST_FUNCALL:
    return "INST_FUNCALL";
  case GraphNodeType::INST_RET:
    return "INST_RET";
  case GraphNodeType::INST_BR:
    return "INST_BR";
  case GraphNodeType::INST_OTHER:
    return "INST_OTHER";
  case GraphNodeType::FUNC_ENTRY:
    return "FUNC_ENTRY";
  case GraphNodeType::PARAM_FORMALIN:
    return "PARAM_FORMALIN";
  case GraphNodeType::PARAM_FORMALOUT:
    return "PARAM_FORMALOUT";
  case GraphNodeType::PARAM_ACTUALIN:
    return "PARAM_ACTUALIN";
  case GraphNodeType::PARAM_ACTUALOUT:
    return "PARAM_ACTUALOUT";
  case GraphNodeType::VAR_STATICALLOCGLOBALSCOPE:
    return "VAR_STATICALLOCGLOBALSCOPE";
  case GraphNodeType::VAR_STATICALLOCMODULESCOPE:
    return "VAR_STATICALLOCMODULESCOPE";
  case GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE:
    return "VAR_STATICALLOCFUNCTIONSCOPE";
  case GraphNodeType::VAR_OTHER:
    return "VAR_OTHER";
  case GraphNodeType::FUNC:
    return "FUNC";
  case GraphNodeType::CLASS:
    return "CLASS";
  case GraphNodeType::ANNO_VAR:
    return "ANNO_VAR";
  case GraphNodeType::ANNO_GLOBAL:
    return "ANNO_GLOBAL";
  case GraphNodeType::ANNO_OTHER:
    return "ANNO_OTHER";
  }
  return "UNKNOWN";
}

static const char *edgeTypeName(EdgeType t) {
  switch (t) {
  case EdgeType::IND_CALL:
    return "IND_CALL";
  case EdgeType::CONTROLDEP_CALLINV:
    return "CONTROLDEP_CALLINV";
  case EdgeType::CONTROLDEP_CALLRET:
    return "CONTROLDEP_CALLRET";
  case EdgeType::CONTROLDEP_ENTRY:
    return "CONTROLDEP_ENTRY";
  case EdgeType::CONTROLDEP_BR:
    return "CONTROLDEP_BR";
  case EdgeType::CONTROLDEP_IND_BR:
    return "CONTROLDEP_IND_BR";
  case EdgeType::DATA_DEF_USE:
    return "DATA_DEF_USE";
  case EdgeType::DATA_RAW:
    return "DATA_RAW";
  case EdgeType::DATA_READ:
    return "DATA_READ";
  case EdgeType::DATA_ALIAS:
    return "DATA_ALIAS";
  case EdgeType::DATA_RET:
    return "DATA_RET";
  case EdgeType::PARAMETER_IN:
    return "PARAMETER_IN";
  case EdgeType::PARAMETER_OUT:
    return "PARAMETER_OUT";
  case EdgeType::PARAMETER_FIELD:
    return "PARAMETER_FIELD";
  case EdgeType::GLOBAL_DEP:
    return "GLOBAL_DEP";
  case EdgeType::VAL_DEP:
    return "VAL_DEP";
  case EdgeType::CLS_MTH:
    return "CLS_MTH";
  case EdgeType::ANNO_VAR:
    return "ANNO_VAR";
  case EdgeType::ANNO_GLOBAL:
    return "ANNO_GLOBAL";
  case EdgeType::ANNO_OTHER:
    return "ANNO_OTHER";
  case EdgeType::TYPE_OTHEREDGE:
    return "TYPE_OTHEREDGE";
  }
  return "UNKNOWN";
}

// ============================================================================
// CypherParser implementation
// ============================================================================

std::unique_ptr<CypherQuery> CypherParser::parse(const std::string &query) {
  clearError();
  std::string trimmedQuery = query;
  trim(trimmedQuery);

  if (trimmedQuery.empty()) {
    setError(CypherErrorCode::PARSE_ERROR, "Empty query", 1, 1);
    return nullptr;
  }

  size_t pos = 0;
  std::vector<std::string> tokens = tokenize(trimmedQuery);

  if (tokens.empty()) {
    setError(CypherErrorCode::PARSE_ERROR, "No valid tokens found", 1, 1);
    return nullptr;
  }

  activeParams_ = nullptr;
  return parseQuery(tokens, pos, CypherQueryParameters{});
}

std::unique_ptr<CypherQuery>
CypherParser::parse(const std::string &query,
                    const CypherQueryParameters &params) {
  clearError();
  std::string trimmedQuery = query;
  trim(trimmedQuery);

  if (trimmedQuery.empty()) {
    setError(CypherErrorCode::PARSE_ERROR, "Empty query", 1, 1);
    return nullptr;
  }

  size_t pos = 0;
  std::vector<std::string> tokens = tokenize(trimmedQuery);

  if (tokens.empty()) {
    setError(CypherErrorCode::PARSE_ERROR, "No valid tokens found", 1, 1);
    return nullptr;
  }

  activeParams_ = &params;
  return parseQuery(tokens, pos, params);
}

void CypherParser::trim(std::string &s) {
  if (s.empty())
    return;

  size_t start = 0;
  while (start < s.size() &&
         std::isspace(static_cast<unsigned char>(s[start]))) {
    start++;
  }

  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    end--;
  }

  s = s.substr(start, end - start);
}

bool CypherParser::isAlpha(char c) {
  return std::isalpha(static_cast<unsigned char>(c));
}

bool CypherParser::isDigit(char c) {
  return std::isdigit(static_cast<unsigned char>(c));
}

bool CypherParser::isAlphaNumeric(char c) {
  return isAlpha(c) || isDigit(c) || c == '_' || c == '$';
}

std::vector<std::string> CypherParser::tokenize(const std::string &s) {
  std::vector<std::string> tokens;
  std::string current;
  bool inString = false;
  char stringChar = '"';

  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];

    if (inString) {
      if (c == '\\' && i + 1 < s.size()) {
        current += c;
        current += s[++i];
      } else if (c == stringChar) {
        current += c;
        tokens.push_back(current);
        current.clear();
        inString = false;
      } else {
        current += c;
      }
      continue;
    }

    if (c == '"' || c == '\'') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      current += c;
      stringChar = c;
      inString = true;
      continue;
    }

    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }

    if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
        c == ',' || c == ';' || c == ':' || c == '-' || c == '>' || c == '<' ||
        c == '=' || c == '!' || c == '.' || c == '@' || c == '#' || c == '*') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      if (c == '-' && i + 1 < s.size() && s[i + 1] == '>') {
        tokens.push_back("->");
        i++;
      } else if (c == '<' && i + 1 < s.size() && s[i + 1] == '-') {
        tokens.push_back("<-");
        i++;
      } else if (c == '<' && i + 1 < s.size() && s[i + 1] == '=') {
        tokens.push_back("<=");
        i++;
      } else if (c == '>' && i + 1 < s.size() && s[i + 1] == '=') {
        tokens.push_back(">=");
        i++;
      } else if (c == '!' && i + 1 < s.size() && s[i + 1] == '=') {
        tokens.push_back("!=");
        i++;
      } else if (c == '<' && i + 1 < s.size() && s[i + 1] == '>') {
        tokens.push_back("<>");
        i++;
      } else if (c == '=' && i + 1 < s.size() && s[i + 1] == '=') {
        tokens.push_back("==");
        i++;
      } else {
        std::string single(1, c);
        tokens.push_back(single);
      }
      continue;
    }

    current += c;
  }

  if (!current.empty()) {
    tokens.push_back(current);
  }

  return tokens;
}

std::unique_ptr<CypherQuery>
CypherParser::parseQuery(std::vector<std::string> &tokens, size_t &pos,
                         const CypherQueryParameters &params) {
  auto query = std::make_unique<CypherQuery>();

  while (hasMore(tokens, pos)) {
    std::string token = peek(tokens, pos);
    std::string upperToken = token;
    std::transform(upperToken.begin(), upperToken.end(), upperToken.begin(),
                   ::toupper);

    if (upperToken == "MATCH") {
      consume(tokens, pos);
      // Helper lambda to get uppercased peek
      auto peekUpper = [&tokens, &pos]() -> std::string {
        if (pos < tokens.size()) {
          std::string t = tokens[pos];
          std::transform(t.begin(), t.end(), t.begin(), ::toupper);
          return t;
        }
        return "";
      };
      while (hasMore(tokens, pos) && peekUpper() != "WHERE" &&
             peekUpper() != "RETURN" && peekUpper() != "WITH" &&
             peekUpper() != ";") {
        // Optional path binding syntax: MATCH p = (a)-[*]->(b) ...
        if (pos + 1 < tokens.size() && tokens[pos + 1] == "=") {
          consume(tokens, pos); // path variable (currently ignored)
          consume(tokens, pos); // '='
        }
        auto pattern = parsePattern(tokens, pos);
        if (!pattern) {
          return nullptr;
        }
        query->addPattern(std::move(pattern));
        // Handle comma-separated patterns
        if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
          consume(tokens, pos);
        }
      }
    } else if (upperToken == "WHERE") {
      consume(tokens, pos);
      auto whereClause = parseWhereClause(tokens, pos);
      if (!whereClause) {
        return nullptr;
      }
      query->setWhereClause(std::move(whereClause));
    } else if (upperToken == "RETURN") {
      consume(tokens, pos);
      // Helper lambda to get uppercased peek
      auto peekUpper = [&tokens, &pos]() -> std::string {
        if (pos < tokens.size()) {
          std::string t = tokens[pos];
          std::transform(t.begin(), t.end(), t.begin(), ::toupper);
          return t;
        }
        return "";
      };
      while (hasMore(tokens, pos) && peekUpper() != "ORDER" &&
             peekUpper() != "LIMIT" && peekUpper() != ";") {
        auto item = parseReturnItem(tokens, pos);
        if (!item) {
          return nullptr;
        }
        query->addReturnItem(std::move(item));
        if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
          consume(tokens, pos);
        }
      }
    } else if (upperToken == "ORDER") {
      consume(tokens, pos);
      if (hasMore(tokens, pos) &&
          (peek(tokens, pos) == "BY" || peek(tokens, pos) == "by")) {
        consume(tokens, pos);
        auto orderBy = parseOrderBy(tokens, pos);
        if (orderBy) {
          query->setOrderBy(std::move(orderBy));
        }
      } else {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected BY after ORDER", 0,
                 0);
        return nullptr;
      }
    } else if (upperToken == "LIMIT") {
      consume(tokens, pos);
      if (hasMore(tokens, pos)) {
        std::string limitStr = consume(tokens, pos);
        try {
          int limit = std::stoi(limitStr);
          query->setLimit(limit);
        } catch (...) {
          setError(CypherErrorCode::SYNTAX_ERROR,
                   "Invalid LIMIT value: " + limitStr, 0, 0);
          return nullptr;
        }
      } else {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected value after LIMIT", 0,
                 0);
        return nullptr;
      }
    } else if (upperToken == "WITH") {
      consume(tokens, pos);
      // Helper lambda to get uppercased peek
      auto peekUpper = [&tokens, &pos]() -> std::string {
        if (pos < tokens.size()) {
          std::string t = tokens[pos];
          std::transform(t.begin(), t.end(), t.begin(), ::toupper);
          return t;
        }
        return "";
      };
      while (hasMore(tokens, pos) && peekUpper() != "MATCH" &&
             peekUpper() != "RETURN" && peekUpper() != ";") {
        auto item = parseReturnItem(tokens, pos);
        if (item) {
          query->addWithItem(std::move(item));
        }
        if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
          consume(tokens, pos);
        }
      }
    } else if (token == ";") {
      break;
    } else {
      setError(CypherErrorCode::SYNTAX_ERROR, "Unexpected token: " + token, 0,
               0);
      return nullptr;
    }
  }

  if (query->getPatterns().empty()) {
    setError(CypherErrorCode::SYNTAX_ERROR, "MATCH clause is required", 0, 0);
    return nullptr;
  }

  return query;
}

std::unique_ptr<CypherPatternElement>
CypherParser::parsePattern(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected pattern", 0, 0);
    return nullptr;
  }

  auto pattern =
      std::make_unique<CypherPatternElement>(parseNodePattern(tokens, pos));

  if (!pattern->getStartNode()) {
    return nullptr;
  }

  auto cloneNodePattern = [](const CypherNodePattern *nodePat)
      -> std::unique_ptr<CypherNodePattern> {
    if (!nodePat)
      return nullptr;
    auto copy = std::make_unique<CypherNodePattern>(nodePat->getVariable(),
                                                    nodePat->getLabel());
    for (const auto &kv : nodePat->getProperties()) {
      copy->addProperty(kv.first, kv.second);
    }
    return copy;
  };

  CypherPatternElement *current = pattern.get();
  while (hasMore(tokens, pos)) {
    const std::string token = peek(tokens, pos);
    if (token != "[" && token != "-" && token != "<-") {
      break;
    }

    auto rel = parseRelationshipPattern(tokens, pos);
    if (!rel) {
      return nullptr;
    }
    current->setRelationship(std::move(rel));

    if (!hasMore(tokens, pos) || peek(tokens, pos) != "(") {
      setError(CypherErrorCode::SYNTAX_ERROR,
               "Expected '(' for end node pattern", 0, 0);
      return nullptr;
    }
    auto endNode = parseNodePattern(tokens, pos);
    if (!endNode) {
      return nullptr;
    }
    current->setEndNode(std::move(endNode));

    if (!hasMore(tokens, pos)) {
      break;
    }
    const std::string nextToken = peek(tokens, pos);
    if (nextToken == "[" || nextToken == "-" || nextToken == "<-") {
      auto nextStart = cloneNodePattern(current->getEndNode());
      if (!nextStart) {
        return nullptr;
      }
      current = current->addNextElement(
          std::make_unique<CypherPatternElement>(std::move(nextStart)));
    } else {
      break;
    }
  }

  return pattern;
}

std::unique_ptr<CypherNodePattern>
CypherParser::parseNodePattern(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected '(' for node pattern", 0,
             0);
    return nullptr;
  }

  if (peek(tokens, pos) != "(") {
    setError(CypherErrorCode::SYNTAX_ERROR,
             "Expected '(' but found: " + peek(tokens, pos), 0, 0);
    return nullptr;
  }
  consume(tokens, pos);

  std::string variable;
  std::string label;
  std::unordered_map<std::string, std::string> properties;

  if (hasMore(tokens, pos)) {
    std::string next = peek(tokens, pos);
    if (next != ":" && next != ")" && next != "{") {
      variable = consume(tokens, pos);
      if (variable.empty()) {
        return nullptr;
      }
    }
  }

  if (hasMore(tokens, pos) && peek(tokens, pos) == ":") {
    consume(tokens, pos);
    if (hasMore(tokens, pos)) {
      label = consume(tokens, pos);
      if (label.empty()) {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected label after ':'", 0,
                 0);
        return nullptr;
      }
    }
  }

  if (hasMore(tokens, pos) && peek(tokens, pos) == "{") {
    consume(tokens, pos);
    while (hasMore(tokens, pos) && peek(tokens, pos) != "}") {
      std::string key = consume(tokens, pos);
      std::string value;
      if (hasMore(tokens, pos) && peek(tokens, pos) == ":") {
        consume(tokens, pos);
        value = consume(tokens, pos);
        if (!value.empty() && value.front() == '$') {
          value = substituteParameter(value.substr(1),
                                      activeParams_ ? *activeParams_
                                                    : CypherQueryParameters{});
        }
        value = unquoteStringToken(std::move(value));
        properties[key] = value;
      }
      if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
        consume(tokens, pos);
      }
    }
    if (hasMore(tokens, pos) && peek(tokens, pos) == "}") {
      consume(tokens, pos);
    }
  }

  if (!hasMore(tokens, pos) || peek(tokens, pos) != ")") {
    setError(CypherErrorCode::SYNTAX_ERROR,
             "Expected ')' to close node pattern", 0, 0);
    return nullptr;
  }
  consume(tokens, pos);

  auto pat = std::make_unique<CypherNodePattern>(variable, label);
  for (const auto &kv : properties) {
    pat->addProperty(kv.first, kv.second);
  }
  return pat;
}

std::unique_ptr<CypherRelationshipPattern>
CypherParser::parseRelationshipPattern(std::vector<std::string> &tokens,
                                       size_t &pos) {
  bool arrowAtStart = false; // arrowhead pointing to start node
  bool arrowAtEnd = false;   // arrowhead pointing to end node

  // Handle leading direction marker: "<-" or "-"
  if (hasMore(tokens, pos)) {
    const std::string token = peek(tokens, pos);
    if (token == "<-") {
      arrowAtStart = true;
      consume(tokens, pos);
    } else if (token == "-") {
      consume(tokens, pos);
    }
  }

  auto consumeTrailingDirection = [&]() {
    if (!hasMore(tokens, pos))
      return;
    const std::string next = peek(tokens, pos);
    if (next == "<-") {
      consume(tokens, pos);
      arrowAtStart = true;
      return;
    }
    if (next == "->") {
      consume(tokens, pos);
      arrowAtEnd = true;
      return;
    }
    if (next == "-") {
      consume(tokens, pos);
      if (hasMore(tokens, pos) && peek(tokens, pos) == ">") {
        consume(tokens, pos);
        arrowAtEnd = true;
      }
      return;
    }
  };

  auto inferDirection = [&]() -> CypherRelationshipPattern::Direction {
    if (arrowAtStart && arrowAtEnd)
      return CypherRelationshipPattern::Direction::BOTH;
    if (arrowAtStart)
      return CypherRelationshipPattern::Direction::IN;
    if (arrowAtEnd)
      return CypherRelationshipPattern::Direction::OUT;
    // Undirected (-[]-) defaults to BOTH for traversal convenience.
    return CypherRelationshipPattern::Direction::BOTH;
  };

  auto parseVarLength = [&]() -> std::pair<int, int> {
    int minHops = 1;
    int maxHops = -1; // -1 means unbounded (executor will cap)
    bool haveExplicitMin = false;

    if (!hasMore(tokens, pos))
      return {minHops, maxHops};

    // "*N" or "*min..max" (both optional)
    const std::string &t0 = peek(tokens, pos);
    if (!t0.empty() && std::isdigit(static_cast<unsigned char>(t0[0]))) {
      std::string minStr = consume(tokens, pos);
      try {
        minHops = std::stoi(minStr);
        haveExplicitMin = true;
      } catch (...) {
        minHops = 1;
      }
    }

    if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
      consume(tokens, pos); // '.'
      if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
        consume(tokens, pos); // '.'
        if (hasMore(tokens, pos)) {
          const std::string &t1 = peek(tokens, pos);
          if (!t1.empty() && std::isdigit(static_cast<unsigned char>(t1[0]))) {
            std::string maxStr = consume(tokens, pos);
            try {
              maxHops = std::stoi(maxStr);
            } catch (...) {
              maxHops = -1;
            }
          } else {
            maxHops = -1;
          }
        }
      }
    } else if (haveExplicitMin) {
      // Exact length "*N".
      maxHops = minHops;
    }

    return {std::max(1, minHops), maxHops};
  };

  // Check for relationship pattern in brackets
  if (hasMore(tokens, pos) && peek(tokens, pos) == "[") {
    consume(tokens, pos); // consume '['

    std::string variable;
    std::string type;
    int minHops = 1;
    int maxHops = 1;
    std::unordered_map<std::string, std::string> properties;

    // Pure variable-length pattern: "[*]" / "[*1..3]" (no var / no type).
    if (hasMore(tokens, pos) && peek(tokens, pos) == "*") {
      consume(tokens, pos); // '*'
      const auto range = parseVarLength();
      minHops = range.first;
      maxHops = range.second;

      if (!hasMore(tokens, pos) || peek(tokens, pos) != "]") {
        setError(CypherErrorCode::SYNTAX_ERROR,
                 "Expected ']' to close relationship pattern", 0, 0);
        return nullptr;
      }
      consume(tokens, pos); // ']'
      consumeTrailingDirection();

      auto rel =
          std::make_unique<CypherRelationshipPattern>("", "", inferDirection());
      rel->setMinHops(minHops);
      rel->setMaxHops(maxHops);
      return rel;
    }

    if (hasMore(tokens, pos)) {
      std::string next = peek(tokens, pos);
      if (next != ":" && next != "]") {
        variable = consume(tokens, pos);
      }
    }

    if (hasMore(tokens, pos) && peek(tokens, pos) == ":") {
      consume(tokens, pos);
      if (hasMore(tokens, pos) && peek(tokens, pos) != "]") {
        type = consume(tokens, pos);
      }
    }

    // Optional variable-length hop bounds: "[:T*1..3]" or "[*1..3]"
    if (hasMore(tokens, pos) && peek(tokens, pos) == "*") {
      consume(tokens, pos); // '*'
      const auto range = parseVarLength();
      minHops = range.first;
      maxHops = range.second;
    }

    // Handle properties in braces
    if (hasMore(tokens, pos) && peek(tokens, pos) == "{") {
      consume(tokens, pos);
      while (hasMore(tokens, pos) && peek(tokens, pos) != "}") {
        std::string key = consume(tokens, pos);
        if (hasMore(tokens, pos) && peek(tokens, pos) == ":") {
          consume(tokens, pos);
          std::string value = consume(tokens, pos);
          if (!value.empty() && value.front() == '$') {
            value = substituteParameter(
                value.substr(1),
                activeParams_ ? *activeParams_ : CypherQueryParameters{});
          }
          value = unquoteStringToken(std::move(value));
          properties[key] = value;
        }
        if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
          consume(tokens, pos);
        }
      }
      if (hasMore(tokens, pos) && peek(tokens, pos) == "}") {
        consume(tokens, pos);
      }
    }

    if (!hasMore(tokens, pos) || peek(tokens, pos) != "]") {
      setError(CypherErrorCode::SYNTAX_ERROR,
               "Expected ']' to close relationship pattern", 0, 0);
      return nullptr;
    }
    consume(tokens, pos); // consume ']'

    // Handle trailing direction
    consumeTrailingDirection();

    auto rel = std::make_unique<CypherRelationshipPattern>(variable, type,
                                                           inferDirection());
    rel->setMinHops(minHops);
    rel->setMaxHops(maxHops);
    for (const auto &kv : properties) {
      rel->addProperty(kv.first, kv.second);
    }
    return rel;
  }

  // No bracket pattern, just direction
  consumeTrailingDirection();
  return std::make_unique<CypherRelationshipPattern>("", "", inferDirection());
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseWhereClause(std::vector<std::string> &tokens, size_t &pos) {
  return parseBooleanExpression(tokens, pos);
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseBooleanExpression(std::vector<std::string> &tokens,
                                     size_t &pos) {
  return parseOrExpression(tokens, pos);
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseOrExpression(std::vector<std::string> &tokens, size_t &pos) {
  auto left = parseAndExpression(tokens, pos);
  if (!left)
    return nullptr;

  while (hasMore(tokens, pos)) {
    std::string op = peek(tokens, pos);
    std::string upperOp = op;
    std::transform(upperOp.begin(), upperOp.end(), upperOp.begin(), ::toupper);
    if (upperOp != "OR")
      break;
    consume(tokens, pos);

    auto right = parseAndExpression(tokens, pos);
    if (!right)
      return nullptr;
    left = CypherWhereClause::makeOr(std::move(left), std::move(right));
  }

  return left;
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseAndExpression(std::vector<std::string> &tokens,
                                 size_t &pos) {
  auto left = parseUnaryExpression(tokens, pos);
  if (!left)
    return nullptr;

  while (hasMore(tokens, pos)) {
    std::string op = peek(tokens, pos);
    std::string upperOp = op;
    std::transform(upperOp.begin(), upperOp.end(), upperOp.begin(), ::toupper);
    if (upperOp != "AND")
      break;
    consume(tokens, pos);

    auto right = parseUnaryExpression(tokens, pos);
    if (!right)
      return nullptr;
    left = CypherWhereClause::makeAnd(std::move(left), std::move(right));
  }

  return left;
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseUnaryExpression(std::vector<std::string> &tokens,
                                   size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR,
             "Expected expression in WHERE clause", 0, 0);
    return nullptr;
  }

  std::string token = peek(tokens, pos);
  std::string upperToken = token;
  std::transform(upperToken.begin(), upperToken.end(), upperToken.begin(),
                 ::toupper);

  if (upperToken == "NOT") {
    consume(tokens, pos);
    auto expr = parseUnaryExpression(tokens, pos);
    if (!expr)
      return nullptr;
    return CypherWhereClause::makeNot(std::move(expr));
  }

  if (upperToken == "EXISTS") {
    consume(tokens, pos);
    if (!hasMore(tokens, pos) || peek(tokens, pos) != "(") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected '(' after EXISTS", 0,
               0);
      return nullptr;
    }
    consume(tokens, pos); // '('
    if (!hasMore(tokens, pos)) {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected operand in EXISTS", 0,
               0);
      return nullptr;
    }

    std::string variable = consume(tokens, pos);
    std::string property;
    if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
      consume(tokens, pos);
      if (!hasMore(tokens, pos)) {
        setError(CypherErrorCode::SYNTAX_ERROR,
                 "Expected property after '.' in EXISTS", 0, 0);
        return nullptr;
      }
      property = consume(tokens, pos);
    }

    if (!hasMore(tokens, pos) || peek(tokens, pos) != ")") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected ')' after EXISTS", 0,
               0);
      return nullptr;
    }
    consume(tokens, pos); // ')'
    return CypherWhereClause::makeExists(variable, property);
  }

  if (token == "(") {
    consume(tokens, pos);
    auto expr = parseOrExpression(tokens, pos);
    if (!expr)
      return nullptr;
    if (!hasMore(tokens, pos) || peek(tokens, pos) != ")") {
      setError(CypherErrorCode::SYNTAX_ERROR,
               "Expected ')' to close parenthesized WHERE expression", 0, 0);
      return nullptr;
    }
    consume(tokens, pos);
    return expr;
  }

  return parseComparison(tokens, pos);
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseComparison(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected comparison expression", 0,
             0);
    return nullptr;
  }

  std::string variable = consume(tokens, pos);
  std::string property;

  if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
    consume(tokens, pos);
    if (hasMore(tokens, pos)) {
      property = consume(tokens, pos);
    }
  }

  if (!hasMore(tokens, pos)) {
    return CypherWhereClause::makeExists(variable, property);
  }

  std::string op = consume(tokens, pos);
  std::string upperOp = op;
  std::transform(upperOp.begin(), upperOp.end(), upperOp.begin(), ::toupper);

  // Handle "IN [..]" / "IN [.., ..]" list membership.
  if (upperOp == "IN") {
    if (!hasMore(tokens, pos) || peek(tokens, pos) != "[") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected '[' after IN", 0, 0);
      return nullptr;
    }
    consume(tokens, pos); // '['

    std::vector<std::string> values;
    while (hasMore(tokens, pos) && peek(tokens, pos) != "]") {
      std::string v = consume(tokens, pos);
      if (!v.empty() && v.front() == '$') {
        v = substituteParameter(v.substr(1), activeParams_
                                                 ? *activeParams_
                                                 : CypherQueryParameters{});
      }
      v = unquoteStringToken(std::move(v));
      values.push_back(std::move(v));

      if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
        consume(tokens, pos);
      }
    }

    if (!hasMore(tokens, pos) || peek(tokens, pos) != "]") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected ']' to close IN list",
               0, 0);
      return nullptr;
    }
    consume(tokens, pos); // ']'

    return CypherWhereClause::makeInList(variable, property, std::move(values));
  }

  // Handle "IS NULL" / "IS NOT NULL"
  if (upperOp == "IS") {
    if (!hasMore(tokens, pos)) {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected NULL after IS", 0, 0);
      return nullptr;
    }
    std::string t1 = consume(tokens, pos);
    std::string upperT1 = t1;
    std::transform(upperT1.begin(), upperT1.end(), upperT1.begin(), ::toupper);

    if (upperT1 == "NOT") {
      if (!hasMore(tokens, pos)) {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected NULL after IS NOT", 0,
                 0);
        return nullptr;
      }
      std::string t2 = consume(tokens, pos);
      std::string upperT2 = t2;
      std::transform(upperT2.begin(), upperT2.end(), upperT2.begin(),
                     ::toupper);
      if (upperT2 != "NULL") {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected NULL after IS NOT", 0,
                 0);
        return nullptr;
      }
      return CypherWhereClause::makeComparison(
          variable, property, CypherComparisonOp::IS_NOT_NULL, "");
    }

    if (upperT1 != "NULL") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected NULL after IS", 0, 0);
      return nullptr;
    }
    return CypherWhereClause::makeComparison(variable, property,
                                             CypherComparisonOp::IS_NULL, "");
  }

  // Handle "STARTS WITH" / "ENDS WITH"
  if (upperOp == "STARTS" || upperOp == "ENDS") {
    if (!hasMore(tokens, pos)) {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected WITH after " + op, 0,
               0);
      return nullptr;
    }
    std::string maybeWith = consume(tokens, pos);
    std::string upperWith = maybeWith;
    std::transform(upperWith.begin(), upperWith.end(), upperWith.begin(),
                   ::toupper);
    if (upperWith != "WITH") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected WITH after " + op, 0,
               0);
      return nullptr;
    }

    std::string value;
    if (hasMore(tokens, pos)) {
      value = consume(tokens, pos);
      if (!value.empty() && value.front() == '$') {
        value = substituteParameter(value.substr(1),
                                    activeParams_ ? *activeParams_
                                                  : CypherQueryParameters{});
      }
      value = unquoteStringToken(std::move(value));
    }

    return CypherWhereClause::makeComparison(
        variable, property,
        (upperOp == "STARTS") ? CypherComparisonOp::STARTS_WITH
                              : CypherComparisonOp::ENDS_WITH,
        value);
  }

  std::string value;
  if (hasMore(tokens, pos)) {
    value = consume(tokens, pos);
    if (!value.empty() && value.front() == '$') {
      value = substituteParameter(value.substr(1),
                                  activeParams_ ? *activeParams_
                                                : CypherQueryParameters{});
    }
    value = unquoteStringToken(std::move(value));
  }

  CypherComparisonOp comparisonOp = CypherComparisonOp::EQUALS;

  if (upperOp == "=" || upperOp == "==") {
    comparisonOp = CypherComparisonOp::EQUALS;
  } else if (upperOp == "!=" || upperOp == "<>") {
    comparisonOp = CypherComparisonOp::NOT_EQUALS;
  } else if (upperOp == "<") {
    comparisonOp = CypherComparisonOp::LESS_THAN;
  } else if (upperOp == "<=") {
    comparisonOp = CypherComparisonOp::LESS_THAN_OR_EQUAL;
  } else if (upperOp == ">") {
    comparisonOp = CypherComparisonOp::GREATER_THAN;
  } else if (upperOp == ">=") {
    comparisonOp = CypherComparisonOp::GREATER_THAN_OR_EQUAL;
  } else if (upperOp == "CONTAINS") {
    comparisonOp = CypherComparisonOp::CONTAINS;
  }

  return CypherWhereClause::makeComparison(variable, property, comparisonOp,
                                           value);
}

std::unique_ptr<CypherReturnItem>
CypherParser::parseReturnItem(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected return item", 0, 0);
    return nullptr;
  }

  std::string token = consume(tokens, pos);
  std::string alias;

  std::string upperToken = token;
  std::transform(upperToken.begin(), upperToken.end(), upperToken.begin(),
                 ::toupper);

  if (upperToken == "COUNT") {
    if (!hasMore(tokens, pos) || peek(tokens, pos) != "(") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected '(' after COUNT", 0, 0);
      return nullptr;
    }
    consume(tokens, pos); // '('

    bool distinct = false;
    if (hasMore(tokens, pos)) {
      std::string maybeDistinct = peek(tokens, pos);
      std::string upperDistinct = maybeDistinct;
      std::transform(upperDistinct.begin(), upperDistinct.end(),
                     upperDistinct.begin(), ::toupper);
      if (upperDistinct == "DISTINCT") {
        distinct = true;
        consume(tokens, pos);
      }
    }

    if (!hasMore(tokens, pos)) {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected COUNT argument", 0, 0);
      return nullptr;
    }

    std::string arg = consume(tokens, pos);
    if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
      consume(tokens, pos); // '.'
      if (!hasMore(tokens, pos)) {
        setError(CypherErrorCode::SYNTAX_ERROR,
                 "Expected property after '.' in COUNT argument", 0, 0);
        return nullptr;
      }
      arg += ".";
      arg += consume(tokens, pos);
    }

    if (!hasMore(tokens, pos) || peek(tokens, pos) != ")") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected ')' after COUNT", 0, 0);
      return nullptr;
    }
    consume(tokens, pos); // ')'

    if (hasMore(tokens, pos) &&
        (peek(tokens, pos) == "AS" || peek(tokens, pos) == "as")) {
      consume(tokens, pos);
      if (hasMore(tokens, pos)) {
        alias = consume(tokens, pos);
      }
    }

    return CypherReturnItem::makeCount(arg, distinct, alias);
  }

  // Handle property access (e.g., n.id -> consume . and property)
  std::string variable = token;
  if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
    consume(tokens, pos); // consume '.'
    if (hasMore(tokens, pos)) {
      std::string prop = consume(tokens, pos);
      variable += "." + prop; // Combine into single variable
    }
  }

  if (hasMore(tokens, pos) &&
      (peek(tokens, pos) == "AS" || peek(tokens, pos) == "as")) {
    consume(tokens, pos);
    if (hasMore(tokens, pos)) {
      alias = consume(tokens, pos);
    }
  }

  return std::make_unique<CypherReturnItem>(variable, alias);
}

std::unique_ptr<CypherOrderBy>
CypherParser::parseOrderBy(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected variable for ORDER BY", 0,
             0);
    return nullptr;
  }

  std::string variable = consume(tokens, pos);
  std::string property;

  // Handle property access (e.g., n.id -> consume . and property)
  if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
    consume(tokens, pos); // consume '.'
    if (hasMore(tokens, pos)) {
      property = consume(tokens, pos); // consume property name
    }
  }

  CypherOrderBy::Direction dir = CypherOrderBy::Direction::ASC;

  if (hasMore(tokens, pos)) {
    std::string next = peek(tokens, pos);
    std::string upperNext = next;
    std::transform(upperNext.begin(), upperNext.end(), upperNext.begin(),
                   ::toupper);
    if (upperNext == "DESC") {
      dir = CypherOrderBy::Direction::DESC;
      consume(tokens, pos);
    } else if (upperNext == "ASC") {
      consume(tokens, pos);
    }
  }

  return std::make_unique<CypherOrderBy>(variable, property, dir);
}

bool CypherParser::hasMore(const std::vector<std::string> &tokens, size_t pos) {
  return pos < tokens.size();
}

const std::string &CypherParser::peek(const std::vector<std::string> &tokens,
                                      size_t pos) {
  static const std::string empty = "";
  if (pos < tokens.size()) {
    return tokens[pos];
  }
  return empty;
}

const std::string &CypherParser::consume(std::vector<std::string> &tokens,
                                         size_t &pos) {
  static const std::string empty = "";
  if (pos < tokens.size()) {
    return tokens[pos++];
  }
  return empty;
}

std::string
CypherParser::substituteParameter(const std::string &name,
                                  const CypherQueryParameters &params) {
  auto it = params.find(name);
  if (it == params.end()) {
    setError(CypherErrorCode::INVALID_PARAMETER, "Unknown parameter: $" + name,
             0, 0);
    lastError_.suggestion =
        "Pass it via --param " + name + "=<value> (repeatable).";
    return "";
  }
  return it->second;
}

// ============================================================================
// CypherResult implementation
// ============================================================================

std::string CypherResult::toString() const {
  std::ostringstream oss;

  switch (type_) {
  case ResultType::NODES:
    oss << "Result(" << nodes_.size() << " nodes)";
    break;
  case ResultType::RELATIONSHIPS:
    oss << "Result(" << relationships_.size() << " relationships)";
    break;
  case ResultType::PATHS:
    oss << "Result(paths)";
    break;
  case ResultType::SCALAR:
    oss << scalarValue_;
    break;
  case ResultType::INTEGER:
    oss << integerValue_;
    break;
  case ResultType::BOOLEAN:
    oss << (booleanValue_ ? "true" : "false");
    break;
  }

  return oss.str();
}

// ============================================================================
// CypherQueryExecutor implementation
// ============================================================================

std::unique_ptr<CypherResult>
CypherQueryExecutor::execute(const CypherQuery &query) {
  CypherQueryStats stats;
  return execute(query, stats);
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::execute(const CypherQuery &query,
                             CypherQueryStats &stats) {
  auto startTime = std::chrono::high_resolution_clock::now();
  lastStats_ = CypherQueryStats();

  // Bindings are per-query; don't let interactive sessions leak state.
  boundVariables_.clear();
  boundRelationships_.clear();

  auto splitVarProp = [](const std::string &expr) {
    std::string var = expr;
    std::string prop;
    const auto dot = expr.find('.');
    if (dot != std::string::npos) {
      var = expr.substr(0, dot);
      prop = expr.substr(dot + 1);
    }
    return std::pair<std::string, std::string>(std::move(var), std::move(prop));
  };

  auto applyNodePattern = [&](Node *n, const CypherNodePattern *pat) {
    if (!pat || !n) {
      return false;
    }

    if (!pat->getLabel().empty()) {
      auto labelMatches = matchNodes(pat->getLabel(), "");
      bool found = false;
      if (labelMatches) {
        for (auto *cand : labelMatches->getNodes()) {
          if (cand == n) {
            found = true;
            break;
          }
        }
      }
      if (!found) {
        return false;
      }
    }

    for (const auto &kv : pat->getProperties()) {
      if (getNodeProperty(n, kv.first) != kv.second) {
        return false;
      }
    }
    return true;
  };

  auto edgeMatchesType = [](EdgeType t, const std::string &upperType) {
    if (upperType.empty()) {
      return true;
    }

    std::vector<std::string> alts;
    size_t startPos = 0;
    while (startPos <= upperType.size()) {
      size_t bar = upperType.find('|', startPos);
      if (bar == std::string::npos) {
        alts.push_back(upperType.substr(startPos));
        break;
      }
      alts.push_back(upperType.substr(startPos, bar - startPos));
      startPos = bar + 1;
    }

    for (const auto &alt : alts) {
      if (alt.empty())
        return true;
      if (alt == "CONTROL_DEP") {
        if (t == EdgeType::CONTROLDEP_ENTRY || t == EdgeType::CONTROLDEP_BR ||
            t == EdgeType::CONTROLDEP_IND_BR ||
            t == EdgeType::CONTROLDEP_CALLINV ||
            t == EdgeType::CONTROLDEP_CALLRET) {
          return true;
        }
      } else if (alt == "CALL") {
        if (t == EdgeType::CONTROLDEP_CALLINV ||
            t == EdgeType::CONTROLDEP_CALLRET || t == EdgeType::IND_CALL) {
          return true;
        }
      } else if (alt == "DATA_DEP") {
        if (t == EdgeType::DATA_DEF_USE) {
          return true;
        }
      } else {
        static const std::unordered_map<std::string, EdgeType> typeMap = {
            {"DATA_DEF_USE", EdgeType::DATA_DEF_USE},
            {"DATA_RAW", EdgeType::DATA_RAW},
            {"DATA_READ", EdgeType::DATA_READ},
            {"DATA_ALIAS", EdgeType::DATA_ALIAS},
            {"DATA_RET", EdgeType::DATA_RET},
            {"CONTROLDEP_ENTRY", EdgeType::CONTROLDEP_ENTRY},
            {"CONTROLDEP_BR", EdgeType::CONTROLDEP_BR},
            {"CONTROLDEP_IND_BR", EdgeType::CONTROLDEP_IND_BR},
            {"CONTROLDEP_CALLINV", EdgeType::CONTROLDEP_CALLINV},
            {"CONTROLDEP_CALLRET", EdgeType::CONTROLDEP_CALLRET},
            {"CALL_INV", EdgeType::CONTROLDEP_CALLINV},
            {"CALL_RET", EdgeType::CONTROLDEP_CALLRET},
            {"IND_CALL", EdgeType::IND_CALL},
            {"PARAM_IN", EdgeType::PARAMETER_IN},
            {"PARAM_OUT", EdgeType::PARAMETER_OUT},
            {"PARAMETER_IN", EdgeType::PARAMETER_IN},
            {"PARAMETER_OUT", EdgeType::PARAMETER_OUT},
        };
        auto it = typeMap.find(alt);
        if (it != typeMap.end() && t == it->second) {
          return true;
        }
      }
    }
    return false;
  };

  auto expandFromNode = [&](Node *start, const CypherRelationshipPattern *rel) {
    std::vector<std::pair<Node *, Edge *>> out;
    if (!start || !rel) {
      return out;
    }

    int minHops = std::max(1, rel->getMinHops());
    int maxHops = rel->hasVariableLength() ? rel->getMaxHops() : 1;
    if (maxHops < 0) {
      maxHops = unboundedMaxHops_;
    }
    maxHops = std::max(1, maxHops);

    std::string upperType = rel->getType();
    std::transform(upperType.begin(), upperType.end(), upperType.begin(),
                   ::toupper);

    std::vector<Node *> frontier{start};
    for (int hop = 0; hop < maxHops && !frontier.empty(); ++hop) {
      std::vector<Node *> next;
      for (auto *node : frontier) {
        if (rel->getDirection() == CypherRelationshipPattern::Direction::OUT ||
            rel->getDirection() == CypherRelationshipPattern::Direction::BOTH) {
          for (auto *e : node->getOutEdgeSet()) {
            if (!edgeMatchesType(e->getEdgeType(), upperType))
              continue;
            Node *nbr = e->getDstNode();
            if (hop + 1 >= minHops) {
              out.push_back({nbr, e});
            }
            next.push_back(nbr);
          }
        }
        if (rel->getDirection() == CypherRelationshipPattern::Direction::IN ||
            rel->getDirection() == CypherRelationshipPattern::Direction::BOTH) {
          for (auto *e : node->getInEdgeSet()) {
            if (!edgeMatchesType(e->getEdgeType(), upperType))
              continue;
            Node *nbr = e->getSrcNode();
            if (hop + 1 >= minHops) {
              out.push_back({nbr, e});
            }
            next.push_back(nbr);
          }
        }
      }
      frontier = std::move(next);
    }
    return out;
  };

  auto collectChain = [](const CypherPatternElement *root) {
    std::vector<const CypherPatternElement *> chain;
    const CypherPatternElement *cur = root;
    while (cur) {
      chain.push_back(cur);
      if (cur->getNextElements().empty()) {
        break;
      }
      cur = cur->getNextElements()[0].get();
    }
    return chain;
  };

  std::vector<MatchRow> rows(1); // one empty row

  for (const auto &pattern : query.getPatterns()) {
    const auto chain = collectChain(pattern.get());
    if (chain.empty()) {
      continue;
    }

    std::vector<MatchRow> nextRows;
    for (const auto &baseRow : rows) {
      std::vector<std::pair<MatchRow, Node *>> states;
      const CypherNodePattern *startPat = chain[0]->getStartNode();
      if (!startPat) {
        continue;
      }

      std::vector<Node *> startCandidates;
      if (!startPat->getVariable().empty()) {
        auto it = baseRow.nodes.find(startPat->getVariable());
        if (it != baseRow.nodes.end()) {
          startCandidates.push_back(it->second);
        }
      }
      if (startCandidates.empty()) {
        auto matched = matchNodes(startPat->getLabel(), "");
        if (!matched) {
          continue;
        }
        startCandidates = matched->getNodes();
      }

      for (auto *cand : startCandidates) {
        if (!applyNodePattern(cand, startPat)) {
          continue;
        }
        MatchRow seeded = baseRow;
        if (!startPat->getVariable().empty()) {
          auto it = seeded.nodes.find(startPat->getVariable());
          if (it != seeded.nodes.end() && it->second != cand) {
            continue;
          }
          seeded.nodes[startPat->getVariable()] = cand;
        }
        states.push_back({std::move(seeded), cand});
      }

      for (const auto *elem : chain) {
        std::vector<std::pair<MatchRow, Node *>> advanced;
        const auto *startNodePat = elem->getStartNode();
        const auto *rel = elem->getRelationship();
        const auto *endNodePat = elem->getEndNode();

        for (auto &st : states) {
          MatchRow row = st.first;
          Node *curNode = st.second;

          if (startNodePat && !applyNodePattern(curNode, startNodePat)) {
            continue;
          }
          if (startNodePat && !startNodePat->getVariable().empty()) {
            auto it = row.nodes.find(startNodePat->getVariable());
            if (it != row.nodes.end() && it->second != curNode) {
              continue;
            }
            row.nodes[startNodePat->getVariable()] = curNode;
          }

          if (!rel) {
            advanced.push_back({std::move(row), curNode});
            continue;
          }

          auto expansions = expandFromNode(curNode, rel);
          for (const auto &exp : expansions) {
            Node *dst = exp.first;
            Edge *edge = exp.second;
            if (endNodePat && !applyNodePattern(dst, endNodePat)) {
              continue;
            }

            MatchRow row2 = row;
            if (!rel->getVariable().empty()) {
              auto it = row2.rels.find(rel->getVariable());
              if (it != row2.rels.end() && it->second != edge) {
                continue;
              }
              row2.rels[rel->getVariable()] = edge;
            }
            if (endNodePat && !endNodePat->getVariable().empty()) {
              auto it = row2.nodes.find(endNodePat->getVariable());
              if (it != row2.nodes.end() && it->second != dst) {
                continue;
              }
              row2.nodes[endNodePat->getVariable()] = dst;
            }
            advanced.push_back({std::move(row2), dst});
          }
        }

        states = std::move(advanced);
        if (states.empty()) {
          break;
        }
      }

      for (auto &st : states) {
        nextRows.push_back(std::move(st.first));
      }
    }
    rows = std::move(nextRows);
    if (rows.empty()) {
      break;
    }
  }

  if (query.getWhereClause()) {
    std::vector<MatchRow> filteredRows;
    filteredRows.reserve(rows.size());
    for (const auto &row : rows) {
      if (evaluateCondition(*query.getWhereClause(), row)) {
        filteredRows.push_back(row);
      }
    }
    rows = std::move(filteredRows);
  }

  // Aggregations (currently: COUNT) return a scalar result.
  if (query.getReturnItems().size() == 1 &&
      query.getReturnItems()[0]->getKind() == CypherReturnItem::Kind::COUNT) {
    const auto &item = *query.getReturnItems()[0];

    auto result =
        std::make_unique<CypherResult>(CypherResult::ResultType::INTEGER);

    auto splitVarProp = [](const std::string &expr) {
      std::string var = expr;
      std::string prop;
      auto dot = expr.find('.');
      if (dot != std::string::npos) {
        var = expr.substr(0, dot);
        prop = expr.substr(dot + 1);
      }
      return std::pair<std::string, std::string>(std::move(var),
                                                 std::move(prop));
    };

    const auto argParts = splitVarProp(item.getAggArg());
    const std::string &argVar = argParts.first;
    const std::string &argProp = argParts.second;
    const bool wantDistinct = item.isAggDistinct();

    int64_t count = 0;

    if (argVar == "*") {
      count = static_cast<int64_t>(rows.size());
    } else if (argProp.empty()) {
      if (wantDistinct) {
        std::unordered_set<uintptr_t> uniq;
        for (const auto &row : rows) {
          auto itN = row.nodes.find(argVar);
          if (itN != row.nodes.end())
            uniq.insert(reinterpret_cast<uintptr_t>(itN->second));
          auto itR = row.rels.find(argVar);
          if (itR != row.rels.end())
            uniq.insert(reinterpret_cast<uintptr_t>(itR->second));
        }
        count = static_cast<int64_t>(uniq.size());
      } else {
        for (const auto &row : rows) {
          if (row.nodes.find(argVar) != row.nodes.end() ||
              row.rels.find(argVar) != row.rels.end()) {
            ++count;
          }
        }
      }
    } else {
      if (wantDistinct) {
        std::unordered_set<std::string> uniq;
        for (const auto &row : rows) {
          auto itN = row.nodes.find(argVar);
          if (itN != row.nodes.end()) {
            const auto v = getNodeProperty(itN->second, argProp);
            if (!v.empty())
              uniq.insert(v);
            continue;
          }
          auto itR = row.rels.find(argVar);
          if (itR != row.rels.end()) {
            const auto v = getEdgeProperty(itR->second, argProp);
            if (!v.empty())
              uniq.insert(v);
          }
        }
        count = static_cast<int64_t>(uniq.size());
      } else {
        for (const auto &row : rows) {
          auto itN = row.nodes.find(argVar);
          if (itN != row.nodes.end()) {
            if (!getNodeProperty(itN->second, argProp).empty())
              ++count;
            continue;
          }
          auto itR = row.rels.find(argVar);
          if (itR != row.rels.end()) {
            if (!getEdgeProperty(itR->second, argProp).empty())
              ++count;
          }
        }
      }
    }

    result->setIntegerValue(count);
    lastStats_.resultsReturned = result->getCount();

    auto endTime = std::chrono::high_resolution_clock::now();
    lastStats_.executionTime =
        std::chrono::duration_cast<std::chrono::microseconds>(endTime -
                                                              startTime);
    stats = lastStats_;
    return result;
  }

  // ORDER BY applies to rows.
  if (query.getOrderBy() && !rows.empty()) {
    const auto &ob = *query.getOrderBy();
    const std::string prop =
        ob.getProperty().empty() ? "label" : ob.getProperty();

    auto tryParseInt = [](const std::string &s, int64_t &out) -> bool {
      if (s.empty())
        return false;
      char *end = nullptr;
      const long long v = std::strtoll(s.c_str(), &end, 10);
      if (!end || *end != '\0')
        return false;
      out = static_cast<int64_t>(v);
      return true;
    };

    auto rowKey = [&](const MatchRow &row) {
      auto itN = row.nodes.find(ob.getVariable());
      if (itN != row.nodes.end()) {
        return getNodeProperty(itN->second, prop);
      }
      auto itR = row.rels.find(ob.getVariable());
      if (itR != row.rels.end()) {
        return getEdgeProperty(itR->second, prop);
      }
      return std::string();
    };

    auto cmp = [&](const MatchRow &a, const MatchRow &b) {
      const std::string va = rowKey(a);
      const std::string vb = rowKey(b);
      int64_t ia = 0, ib = 0;
      const bool na = tryParseInt(va, ia);
      const bool nb = tryParseInt(vb, ib);
      if (na && nb)
        return ia < ib;
      return va < vb;
    };

    std::sort(rows.begin(), rows.end(), cmp);
    if (ob.getDirection() == CypherOrderBy::Direction::DESC) {
      std::reverse(rows.begin(), rows.end());
    }
  }

  if (query.hasLimit() && query.getLimit() > 0 &&
      rows.size() > static_cast<size_t>(query.getLimit())) {
    rows.resize(static_cast<size_t>(query.getLimit()));
  }

  // Rebuild bindings from final rows.
  boundVariables_.clear();
  boundRelationships_.clear();
  for (const auto &row : rows) {
    for (const auto &kv : row.nodes) {
      boundVariables_[kv.first].push_back(kv.second);
    }
    for (const auto &kv : row.rels) {
      boundRelationships_[kv.first].push_back(kv.second);
    }
  }

  std::vector<Node *> projectedNodes;
  std::vector<Edge *> projectedEdges;
  std::vector<std::string> projectedScalars;
  bool wantsNodeProjection = false;
  bool wantsEdgeProjection = false;
  bool wantsScalarProjection = false;
  std::unordered_set<std::string> declaredNodeVars;
  std::unordered_set<std::string> declaredRelVars;

  auto collectDeclaredVars = [&](const auto &self,
                                 const CypherPatternElement *elem) -> void {
    if (!elem)
      return;
    if (elem->getStartNode() && !elem->getStartNode()->getVariable().empty()) {
      declaredNodeVars.insert(elem->getStartNode()->getVariable());
    }
    if (elem->getEndNode() && !elem->getEndNode()->getVariable().empty()) {
      declaredNodeVars.insert(elem->getEndNode()->getVariable());
    }
    if (elem->getRelationship() &&
        !elem->getRelationship()->getVariable().empty()) {
      declaredRelVars.insert(elem->getRelationship()->getVariable());
    }
    for (const auto &next : elem->getNextElements()) {
      self(self, next.get());
    }
  };
  for (const auto &p : query.getPatterns()) {
    collectDeclaredVars(collectDeclaredVars, p.get());
  }

  if (query.getReturnItems().empty()) {
    for (const auto &row : rows) {
      for (const auto &kv : row.nodes)
        projectedNodes.push_back(kv.second);
      for (const auto &kv : row.rels)
        projectedEdges.push_back(kv.second);
    }
  } else {
    std::vector<std::vector<std::string>> tabular;
    tabular.reserve(rows.size());

    for (const auto &itemPtr : query.getReturnItems()) {
      const auto &item = *itemPtr;
      if (item.getKind() != CypherReturnItem::Kind::VARIABLE_OR_PROPERTY) {
        continue;
      }
      const auto varProp = splitVarProp(item.getVariable());
      const std::string &var = varProp.first;
      const std::string &prop = varProp.second;
      if (!prop.empty()) {
        wantsScalarProjection = true;
      } else if (declaredRelVars.find(var) != declaredRelVars.end()) {
        wantsEdgeProjection = true;
      } else if (declaredNodeVars.find(var) != declaredNodeVars.end()) {
        wantsNodeProjection = true;
      }
    }

    for (const auto &row : rows) {
      std::vector<std::string> columns;
      columns.reserve(query.getReturnItems().size());
      for (const auto &itemPtr : query.getReturnItems()) {
        const auto &item = *itemPtr;
        if (item.getKind() != CypherReturnItem::Kind::VARIABLE_OR_PROPERTY) {
          continue;
        }

        const auto varProp = splitVarProp(item.getVariable());
        const std::string &var = varProp.first;
        const std::string &prop = varProp.second;

        auto itN = row.nodes.find(var);
        if (itN != row.nodes.end()) {
          if (prop.empty()) {
            wantsNodeProjection = true;
            projectedNodes.push_back(itN->second);
            columns.push_back(getNodeProperty(itN->second, "label"));
          } else {
            wantsScalarProjection = true;
            const auto v = getNodeProperty(itN->second, prop);
            projectedScalars.push_back(v);
            columns.push_back(v);
          }
          continue;
        }

        auto itR = row.rels.find(var);
        if (itR != row.rels.end()) {
          if (prop.empty()) {
            wantsEdgeProjection = true;
            projectedEdges.push_back(itR->second);
            columns.push_back(getEdgeProperty(itR->second, "label"));
          } else {
            wantsScalarProjection = true;
            const auto v = getEdgeProperty(itR->second, prop);
            projectedScalars.push_back(v);
            columns.push_back(v);
          }
          continue;
        }

        columns.push_back("");
      }
      tabular.push_back(std::move(columns));
    }

    if (query.getReturnItems().size() > 1) {
      wantsScalarProjection = true;
      wantsNodeProjection = false;
      wantsEdgeProjection = false;
      projectedScalars.clear();
      for (const auto &cols : tabular) {
        std::ostringstream line;
        for (size_t i = 0; i < cols.size(); ++i) {
          if (i)
            line << ", ";
          line << cols[i];
        }
        projectedScalars.push_back(line.str());
      }
    }
  }

  std::unique_ptr<CypherResult> result;
  if (wantsScalarProjection && !wantsNodeProjection && !wantsEdgeProjection) {
    std::ostringstream oss;
    for (size_t i = 0; i < projectedScalars.size(); ++i) {
      if (i)
        oss << "\n";
      oss << projectedScalars[i];
    }
    result = std::make_unique<CypherResult>(CypherResult::ResultType::SCALAR);
    result->setScalarValue(oss.str());
  } else if (wantsEdgeProjection && !wantsNodeProjection &&
             !wantsScalarProjection) {
    result =
        std::make_unique<CypherResult>(CypherResult::ResultType::RELATIONSHIPS);
    for (auto *edge : projectedEdges) {
      result->addEdge(edge);
    }
  } else {
    result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);
    for (auto *node : projectedNodes) {
      result->addNode(node);
    }
    for (auto *edge : projectedEdges) {
      result->addEdge(edge);
    }
  }

  lastStats_.resultsReturned = result->getCount();

  auto endTime = std::chrono::high_resolution_clock::now();
  lastStats_.executionTime =
      std::chrono::duration_cast<std::chrono::microseconds>(endTime -
                                                            startTime);
  stats = lastStats_;

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::matchPattern(const CypherPatternElement *pattern) {
  if (!pattern)
    return nullptr;

  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  auto dedupNodes = [](std::vector<Node *> &nodes) {
    std::sort(nodes.begin(), nodes.end());
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  };
  auto dedupEdges = [](std::vector<Edge *> &edges) {
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  };

  auto bindNodes = [&](const std::string &var,
                       const std::vector<Node *> &nodes) {
    if (var.empty())
      return;
    auto &bucket = boundVariables_[var];
    bucket.insert(bucket.end(), nodes.begin(), nodes.end());
    dedupNodes(bucket);
  };
  auto bindEdges = [&](const std::string &var,
                       const std::vector<Edge *> &edges) {
    if (var.empty())
      return;
    auto &bucket = boundRelationships_[var];
    bucket.insert(bucket.end(), edges.begin(), edges.end());
    dedupEdges(bucket);
  };

  auto filterByNodePattern =
      [&](const std::vector<Node *> &candidates,
          const CypherNodePattern *nodePattern) -> std::vector<Node *> {
    if (!nodePattern)
      return candidates;

    std::unordered_set<Node *> labelAllowed;
    if (!nodePattern->getLabel().empty()) {
      auto labelNodes = matchNodes(nodePattern->getLabel(), "");
      if (labelNodes) {
        for (auto *n : labelNodes->getNodes()) {
          labelAllowed.insert(n);
        }
      }
    }

    std::vector<Node *> filtered;
    filtered.reserve(candidates.size());
    for (auto *n : candidates) {
      if (!nodePattern->getLabel().empty() && !labelAllowed.count(n))
        continue;
      bool ok = true;
      for (const auto &kv : nodePattern->getProperties()) {
        if (getNodeProperty(n, kv.first) != kv.second) {
          ok = false;
          break;
        }
      }
      if (ok) {
        filtered.push_back(n);
      }
    }
    dedupNodes(filtered);
    return filtered;
  };

  std::function<std::vector<Node *>(const CypherPatternElement *,
                                    const std::vector<Node *> &)>
      evalElement;
  evalElement =
      [&](const CypherPatternElement *elem,
          const std::vector<Node *> &inputStarts) -> std::vector<Node *> {
    if (!elem)
      return inputStarts;

    std::vector<Node *> starts =
        filterByNodePattern(inputStarts, elem->getStartNode());
    bindNodes(elem->getStartNode() ? elem->getStartNode()->getVariable() : "",
              starts);
    for (auto *n : starts) {
      result->addNode(n);
    }

    std::vector<Node *> terminals = starts;
    const auto *rel = elem->getRelationship();
    if (rel) {
      std::vector<Node *> endNodes;
      std::vector<Edge *> relEdges;
      for (auto *start : starts) {
        int maxHops = rel->hasVariableLength() ? rel->getMaxHops() : 1;
        if (maxHops < 0)
          maxHops = unboundedMaxHops_;
        auto traversed = traverse(start, *rel, maxHops);
        if (!traversed)
          continue;

        const auto &edges = traversed->getRelationships();
        relEdges.insert(relEdges.end(), edges.begin(), edges.end());
        for (auto *e : edges) {
          result->addEdge(e);
        }

        std::vector<Node *> traversedNodes = traversed->getNodes();
        if (elem->getEndNode()) {
          traversedNodes =
              filterByNodePattern(traversedNodes, elem->getEndNode());
        }
        endNodes.insert(endNodes.end(), traversedNodes.begin(),
                        traversedNodes.end());
      }
      dedupNodes(endNodes);
      dedupEdges(relEdges);
      bindEdges(rel->getVariable(), relEdges);
      if (elem->getEndNode()) {
        bindNodes(elem->getEndNode()->getVariable(), endNodes);
      }
      for (auto *n : endNodes) {
        result->addNode(n);
      }
      terminals = std::move(endNodes);
    }

    if (elem->getNextElements().empty()) {
      return terminals;
    }

    std::vector<Node *> allNextTerminals;
    for (const auto &next : elem->getNextElements()) {
      auto nextTerms = evalElement(next.get(), terminals);
      allNextTerminals.insert(allNextTerminals.end(), nextTerms.begin(),
                              nextTerms.end());
    }
    dedupNodes(allNextTerminals);
    return allNextTerminals;
  };

  const auto *startNodePattern = pattern->getStartNode();
  if (!startNodePattern)
    return result;
  auto allStartNodes = matchNodes(startNodePattern->getLabel(), "");
  if (!allStartNodes)
    return result;
  evalElement(pattern, allStartNodes->getNodes());
  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::matchNodes(const std::string &label,
                                const std::string &variable) {
  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  static const std::unordered_map<std::string, GraphNodeType> labelMap = {
      {"INST_FUNCALL", GraphNodeType::INST_FUNCALL},
      {"INST_RET", GraphNodeType::INST_RET},
      {"INST_BR", GraphNodeType::INST_BR},
      {"INST_OTHER", GraphNodeType::INST_OTHER},
      {"FUNC_ENTRY", GraphNodeType::FUNC_ENTRY},
      {"PARAM_FORMALIN", GraphNodeType::PARAM_FORMALIN},
      {"PARAM_FORMALOUT", GraphNodeType::PARAM_FORMALOUT},
      {"PARAM_ACTUALIN", GraphNodeType::PARAM_ACTUALIN},
      {"PARAM_ACTUALOUT", GraphNodeType::PARAM_ACTUALOUT},
      {"VAR_OTHER", GraphNodeType::VAR_OTHER},
      {"VAR_STATICALLOCGLOBALSCOPE", GraphNodeType::VAR_STATICALLOCGLOBALSCOPE},
      {"VAR_STATICALLOCMODULESCOPE", GraphNodeType::VAR_STATICALLOCMODULESCOPE},
      {"VAR_STATICALLOCFUNCTIONSCOPE",
       GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE},
      {"FUNC", GraphNodeType::FUNC},
      {"CLASS", GraphNodeType::CLASS},
      {"ANNO_VAR", GraphNodeType::ANNO_VAR},
      {"ANNO_GLOBAL", GraphNodeType::ANNO_GLOBAL},
      {"ANNO_OTHER", GraphNodeType::ANNO_OTHER}};

  if (label.empty()) {
    for (auto it = pdg_.begin(); it != pdg_.end(); ++it) {
      result->addNode(*it);
    }
  } else {
    const std::string upperLabel = [&]() {
      std::string t = label;
      std::transform(t.begin(), t.end(), t.begin(), ::toupper);
      return t;
    }();

    auto matchesGroup = [&](GraphNodeType t) -> bool {
      if (upperLabel == "INST") {
        return t == GraphNodeType::INST_FUNCALL ||
               t == GraphNodeType::INST_RET || t == GraphNodeType::INST_BR ||
               t == GraphNodeType::INST_OTHER;
      }
      if (upperLabel == "VAR") {
        return t == GraphNodeType::VAR_OTHER ||
               t == GraphNodeType::VAR_STATICALLOCGLOBALSCOPE ||
               t == GraphNodeType::VAR_STATICALLOCMODULESCOPE ||
               t == GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE;
      }
      if (upperLabel == "PARAM") {
        return t == GraphNodeType::PARAM_FORMALIN ||
               t == GraphNodeType::PARAM_FORMALOUT ||
               t == GraphNodeType::PARAM_ACTUALIN ||
               t == GraphNodeType::PARAM_ACTUALOUT;
      }
      if (upperLabel == "ANNO") {
        return t == GraphNodeType::ANNO_VAR ||
               t == GraphNodeType::ANNO_GLOBAL ||
               t == GraphNodeType::ANNO_OTHER;
      }
      return false;
    };

    auto it = labelMap.find(upperLabel);
    for (auto iter = pdg_.begin(); iter != pdg_.end(); ++iter) {
      const auto t = (*iter)->getNodeType();
      if ((it != labelMap.end() && t == it->second) || matchesGroup(t)) {
        result->addNode(*iter);
      }
    }
  }

  if (!variable.empty()) {
    boundVariables_[variable] = result->getNodes();
  }

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::matchEdges(const std::string &type,
                                const std::string &variable) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::RELATIONSHIPS);

  static const std::unordered_map<std::string, EdgeType> typeMap = {
      {"DATA_DEF_USE", EdgeType::DATA_DEF_USE},
      {"DATA_RAW", EdgeType::DATA_RAW},
      {"DATA_READ", EdgeType::DATA_READ},
      {"DATA_ALIAS", EdgeType::DATA_ALIAS},
      {"DATA_RET", EdgeType::DATA_RET},
      {"CONTROLDEP_BR", EdgeType::CONTROLDEP_BR},
      {"CONTROLDEP_ENTRY", EdgeType::CONTROLDEP_ENTRY},
      {"CONTROLDEP_CALLINV", EdgeType::CONTROLDEP_CALLINV},
      {"CONTROLDEP_CALLRET", EdgeType::CONTROLDEP_CALLRET},
      {"IND_CALL", EdgeType::IND_CALL},
      {"PARAMETER_IN", EdgeType::PARAMETER_IN},
      {"PARAMETER_OUT", EdgeType::PARAMETER_OUT}};

  if (type.empty()) {
    for (auto it = pdg_.begin(); it != pdg_.end(); ++it) {
      for (auto *edge : (*it)->getOutEdgeSet()) {
        result->addEdge(edge);
      }
    }
  } else {
    auto it = typeMap.find(type);
    if (it != typeMap.end()) {
      for (auto iter = pdg_.begin(); iter != pdg_.end(); ++iter) {
        for (auto *edge : (*iter)->getOutEdgeSet()) {
          if (edge->getEdgeType() == it->second) {
            result->addEdge(edge);
          }
        }
      }
    }
  }

  if (!variable.empty()) {
    boundRelationships_[variable] = result->getRelationships();
  }

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::traverse(Node *start, const CypherRelationshipPattern &rel,
                              int maxHops) {
  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  if (!start)
    return result;

  const int minHops = std::max(1, rel.getMinHops());
  const int effectiveMaxHops = std::max(1, maxHops);

  // Support "TYPE1|TYPE2|..." (OR) for convenience.
  const std::string typeUpper = [&]() {
    std::string t = rel.getType();
    std::transform(t.begin(), t.end(), t.begin(), ::toupper);
    return t;
  }();

  std::vector<std::string> typeAlts;
  if (!typeUpper.empty()) {
    size_t startPos = 0;
    while (startPos <= typeUpper.size()) {
      size_t bar = typeUpper.find('|', startPos);
      if (bar == std::string::npos) {
        typeAlts.push_back(typeUpper.substr(startPos));
        break;
      }
      typeAlts.push_back(typeUpper.substr(startPos, bar - startPos));
      startPos = bar + 1;
    }
  }

  auto matchesAlt = [&](EdgeType t, const std::string &alt) -> bool {
    if (alt.empty())
      return true;
    if (alt == "CONTROL_DEP") {
      return t == EdgeType::CONTROLDEP_ENTRY || t == EdgeType::CONTROLDEP_BR ||
             t == EdgeType::CONTROLDEP_IND_BR ||
             t == EdgeType::CONTROLDEP_CALLINV ||
             t == EdgeType::CONTROLDEP_CALLRET;
    }
    if (alt == "CALL") {
      return t == EdgeType::CONTROLDEP_CALLINV ||
             t == EdgeType::CONTROLDEP_CALLRET || t == EdgeType::IND_CALL;
    }
    if (alt == "DATA_DEP") {
      return t == EdgeType::DATA_DEF_USE;
    }

    static const std::unordered_map<std::string, EdgeType> typeMap = {
        {"DATA_DEF_USE", EdgeType::DATA_DEF_USE},
        {"DATA_RAW", EdgeType::DATA_RAW},
        {"DATA_READ", EdgeType::DATA_READ},
        {"DATA_ALIAS", EdgeType::DATA_ALIAS},
        {"DATA_RET", EdgeType::DATA_RET},
        {"CONTROLDEP_ENTRY", EdgeType::CONTROLDEP_ENTRY},
        {"CONTROLDEP_BR", EdgeType::CONTROLDEP_BR},
        {"CONTROLDEP_IND_BR", EdgeType::CONTROLDEP_IND_BR},
        {"CONTROLDEP_CALLINV", EdgeType::CONTROLDEP_CALLINV},
        {"CONTROLDEP_CALLRET", EdgeType::CONTROLDEP_CALLRET},
        {"CALL_INV", EdgeType::CONTROLDEP_CALLINV},
        {"CALL_RET", EdgeType::CONTROLDEP_CALLRET},
        {"IND_CALL", EdgeType::IND_CALL},
        {"PARAM_IN", EdgeType::PARAMETER_IN},
        {"PARAM_OUT", EdgeType::PARAMETER_OUT},
        {"PARAMETER_IN", EdgeType::PARAMETER_IN},
        {"PARAMETER_OUT", EdgeType::PARAMETER_OUT},
    };

    auto it = typeMap.find(alt);
    return it != typeMap.end() && t == it->second;
  };

  auto edgeMatches = [&](EdgeType t) -> bool {
    if (typeAlts.empty())
      return true;
    for (const auto &alt : typeAlts) {
      if (matchesAlt(t, alt))
        return true;
    }
    return false;
  };

  std::unordered_map<Node *, int> dist;
  dist.emplace(start, 0);

  std::vector<Node *> frontier = {start};
  std::unordered_set<Edge *> visitedEdges;

  for (int hop = 0; hop < effectiveMaxHops && !frontier.empty(); ++hop) {
    std::vector<Node *> next;
    next.reserve(frontier.size() * 2);

    auto visitNeighbor = [&](Edge *edge, Node *neighbor) {
      if (!edgeMatches(edge->getEdgeType()))
        return;
      visitedEdges.insert(edge);
      auto it = dist.find(neighbor);
      if (it != dist.end())
        return;
      const int nd = hop + 1;
      dist.emplace(neighbor, nd);
      if (nd >= minHops)
        result->addNode(neighbor);
      next.push_back(neighbor);
    };

    for (auto *node : frontier) {
      if (rel.getDirection() == CypherRelationshipPattern::Direction::OUT ||
          rel.getDirection() == CypherRelationshipPattern::Direction::BOTH) {
        for (auto *edge : node->getOutEdgeSet()) {
          visitNeighbor(edge, edge->getDstNode());
        }
      }

      if (rel.getDirection() == CypherRelationshipPattern::Direction::IN ||
          rel.getDirection() == CypherRelationshipPattern::Direction::BOTH) {
        for (auto *edge : node->getInEdgeSet()) {
          visitNeighbor(edge, edge->getSrcNode());
        }
      }
    }

    frontier = std::move(next);
  }

  for (auto *e : visitedEdges)
    result->addEdge(e);

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::filterByWhere(const std::vector<Node *> &nodes,
                                   const CypherWhereClause &where) {
  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  for (auto *node : nodes) {
    if (evaluateCondition(where, node)) {
      result->addNode(node);
    }
  }

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::filterByWhere(const std::vector<Edge *> &edges,
                                   const CypherWhereClause &where) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::RELATIONSHIPS);

  for (auto *edge : edges) {
    if (evaluateCondition(where, edge)) {
      result->addEdge(edge);
    }
  }

  return result;
}

bool CypherQueryExecutor::evaluateCondition(
    const CypherWhereClause &condition,
    const CypherQueryExecutor::MatchRow &row) {
  if (condition.isBooleanOp()) {
    if (condition.getBoolOp() == "NOT") {
      const auto *child = condition.getChild();
      return child && !evaluateCondition(*child, row);
    }

    const auto *left = condition.getLeft();
    const auto *right = condition.getRight();
    bool left_result = left ? evaluateCondition(*left, row) : true;
    bool right_result = right ? evaluateCondition(*right, row) : true;

    if (condition.getBoolOp() == "AND")
      return left_result && right_result;
    if (condition.getBoolOp() == "OR")
      return left_result || right_result;
    return false;
  }

  const std::string &var = condition.getVariableName();
  const std::string &prop = condition.getProperty();

  Node *bound_node = nullptr;
  Edge *bound_edge = nullptr;

  if (var.empty()) {
    if (row.nodes.size() == 1 && row.rels.empty())
      bound_node = row.nodes.begin()->second;
    else if (row.rels.size() == 1 && row.nodes.empty())
      bound_edge = row.rels.begin()->second;
  } else {
    auto it_node = row.nodes.find(var);
    if (it_node != row.nodes.end())
      bound_node = it_node->second;
    auto it_rel = row.rels.find(var);
    if (it_rel != row.rels.end())
      bound_edge = it_rel->second;
  }

  if (condition.isExists()) {
    if (prop.empty())
      return bound_node != nullptr || bound_edge != nullptr;
    if (bound_node != nullptr)
      return !getNodeProperty(bound_node, prop).empty();
    if (bound_edge != nullptr)
      return !getEdgeProperty(bound_edge, prop).empty();
    return false;
  }

  std::string lhs;
  if (bound_node != nullptr) {
    lhs = getNodeProperty(bound_node, prop);
  } else if (bound_edge != nullptr) {
    lhs = getEdgeProperty(bound_edge, prop);
  } else {
    return false;
  }

  if (condition.getComparisonOp() == CypherComparisonOp::IN) {
    for (const auto &v : condition.getListValues()) {
      if (lhs == v)
        return true;
    }
    return false;
  }

  return applyComparison(lhs, condition.getComparisonOp(), condition.getValue());
}

bool CypherQueryExecutor::evaluateCondition(const CypherWhereClause &condition,
                                            Node *node) {
  MatchRow row;
  if (!condition.getVariableName().empty())
    row.nodes[condition.getVariableName()] = node;
  else
    row.nodes[""] = node;
  return evaluateCondition(condition, row);
}

bool CypherQueryExecutor::evaluateCondition(const CypherWhereClause &condition,
                                            Edge *edge) {
  MatchRow row;
  if (!condition.getVariableName().empty())
    row.rels[condition.getVariableName()] = edge;
  else
    row.rels[""] = edge;
  return evaluateCondition(condition, row);
}

bool CypherQueryExecutor::applyComparison(const std::string &nodeValue,
                                          CypherComparisonOp op,
                                          const std::string &queryValue) {
  switch (op) {
  case CypherComparisonOp::EQUALS:
    return nodeValue == queryValue;
  case CypherComparisonOp::NOT_EQUALS:
    return nodeValue != queryValue;
  case CypherComparisonOp::LESS_THAN:
    try {
      return std::stoll(nodeValue) < std::stoll(queryValue);
    } catch (...) {
      return nodeValue < queryValue;
    }
  case CypherComparisonOp::LESS_THAN_OR_EQUAL:
    try {
      return std::stoll(nodeValue) <= std::stoll(queryValue);
    } catch (...) {
      return nodeValue <= queryValue;
    }
  case CypherComparisonOp::GREATER_THAN:
    try {
      return std::stoll(nodeValue) > std::stoll(queryValue);
    } catch (...) {
      return nodeValue > queryValue;
    }
  case CypherComparisonOp::GREATER_THAN_OR_EQUAL:
    try {
      return std::stoll(nodeValue) >= std::stoll(queryValue);
    } catch (...) {
      return nodeValue >= queryValue;
    }
  case CypherComparisonOp::IS_NULL:
    return nodeValue.empty();
  case CypherComparisonOp::IS_NOT_NULL:
    return !nodeValue.empty();
  case CypherComparisonOp::CONTAINS:
    return nodeValue.find(queryValue) != std::string::npos;
  case CypherComparisonOp::STARTS_WITH:
    return nodeValue.find(queryValue) == 0;
  case CypherComparisonOp::ENDS_WITH:
    return nodeValue.size() >= queryValue.size() &&
           nodeValue.substr(nodeValue.size() - queryValue.size()) == queryValue;
  case CypherComparisonOp::IN:
    return nodeValue == queryValue;
  }
  return true;
}

std::string CypherQueryExecutor::getNodeProperty(Node *node,
                                                 const std::string &property) {
  if (!node)
    return "";

  const std::string prop = toLower(property);
  if (prop.empty())
    return "";

  if (prop == "type" || prop == "type_id" || prop == "node_type" ||
      prop == "node_type_id") {
    return std::to_string(static_cast<int>(node->getNodeType()));
  }

  if (prop == "label" || prop == "kind") {
    return graphNodeTypeName(node->getNodeType());
  }

  if (prop == "func" || prop == "function") {
    if (auto *func = node->getFunc())
      return func->getName().str();
    if (auto *v = node->getValue())
      if (auto *f = llvm::dyn_cast<llvm::Function>(v))
        return f->getName().str();
    return "";
  }

  if (prop == "name") {
    if (auto *v = node->getValue()) {
      if (v->hasName())
        return v->getName().str();
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (i->hasName())
          return i->getName().str();
      }
    }
    return "";
  }

  if (prop == "opcode") {
    if (auto *v = node->getValue()) {
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        return i->getOpcodeName();
      }
    }
    return "";
  }

  if (prop == "callee") {
    if (auto *v = node->getValue()) {
      if (auto *cb = llvm::dyn_cast<llvm::CallBase>(v)) {
        if (auto *f = cb->getCalledFunction()) {
          return f->getName().str();
        }
        return "<indirect>";
      }
    }
    return "";
  }

  if (prop == "src_file" || prop == "source_file") {
    if (auto *v = node->getValue()) {
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (const llvm::DebugLoc &dl = i->getDebugLoc()) {
          auto *loc = dl.get();
          if (!loc)
            return "";
          return loc->getFilename().str();
        }
      }
    }
    return "";
  }

  if (prop == "src_line" || prop == "source_line") {
    if (auto *v = node->getValue()) {
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (const llvm::DebugLoc &dl = i->getDebugLoc()) {
          auto *loc = dl.get();
          if (!loc)
            return "";
          return std::to_string(loc->getLine());
        }
      }
    }
    return "";
  }

  if (prop == "src_col" || prop == "source_col" || prop == "source_column") {
    if (auto *v = node->getValue()) {
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (const llvm::DebugLoc &dl = i->getDebugLoc()) {
          auto *loc = dl.get();
          if (!loc)
            return "";
          return std::to_string(loc->getColumn());
        }
      }
    }
    return "";
  }

  if (prop == "src" || prop == "source") {
    const std::string file = getNodeProperty(node, "src_file");
    const std::string line = getNodeProperty(node, "src_line");
    const std::string col = getNodeProperty(node, "src_col");
    if (file.empty() && line.empty() && col.empty())
      return "";
    std::ostringstream oss;
    oss << file;
    if (!line.empty())
      oss << ":" << line;
    if (!col.empty())
      oss << ":" << col;
    return oss.str();
  }

  if (prop == "di_type" || prop == "dtype" || prop == "type_name") {
    if (auto *dt = node->getDIType()) {
      return dbgutils::getSourceLevelTypeName(*dt);
    }
    return "";
  }

  if (prop == "llvm" || prop == "ir") {
    if (auto *v = node->getValue()) {
      std::string s;
      llvm::raw_string_ostream os(s);
      v->print(os);
      os.flush();
      return s;
    }
    return "";
  }

  return "";
}

std::string CypherQueryExecutor::getEdgeProperty(Edge *edge,
                                                 const std::string &property) {
  if (!edge)
    return "";

  const std::string prop = toLower(property);
  if (prop == "type" || prop == "type_id" || prop == "edge_type" ||
      prop == "edge_type_id") {
    return std::to_string(static_cast<int>(edge->getEdgeType()));
  }
  if (prop == "label" || prop == "kind") {
    return edgeTypeName(edge->getEdgeType());
  }

  if (prop == "src") {
    return getNodeProperty(edge->getSrcNode(), "label");
  }
  if (prop == "dst") {
    return getNodeProperty(edge->getDstNode(), "label");
  }

  if (prop.rfind("src_", 0) == 0) {
    return getNodeProperty(edge->getSrcNode(), prop.substr(4));
  }
  if (prop.rfind("dst_", 0) == 0) {
    return getNodeProperty(edge->getDstNode(), prop.substr(4));
  }

  return "";
}

// ============================================================================
// Optimizer query functions
// ============================================================================

std::unique_ptr<CypherResult>
CypherQueryExecutor::canMoveEarlier(Node *moving, Node *anchor) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::BOOLEAN);
  if (!moving || !anchor) {
    result->setBooleanValue(false);
    return result;
  }

  TransformQuery query(pdg_);
  LLVMQueryContext llvm_context;
  MotionCheckResult legality =
      query.canMoveEarlier(*moving, *anchor, llvm_context);
  result->setBooleanValue(legality.legal);
  return result;
}

std::unique_ptr<CypherResult> CypherQueryExecutor::canMoveLater(Node *moving,
                                                                Node *anchor) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::BOOLEAN);
  if (!moving || !anchor) {
    result->setBooleanValue(false);
    return result;
  }

  TransformQuery query(pdg_);
  LLVMQueryContext llvm_context;
  MotionCheckResult legality =
      query.canMoveLater(*moving, *anchor, llvm_context);
  result->setBooleanValue(legality.legal);
  return result;
}

std::unique_ptr<CypherResult> CypherQueryExecutor::independent(Node *a,
                                                               Node *b) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::BOOLEAN);
  if (!a || !b) {
    result->setBooleanValue(false);
    return result;
  }

  TransformQuery query(pdg_);
  LLVMQueryContext llvm_context;
  IndependenceCheckResult indep = query.independent(*a, *b, llvm_context);
  result->setBooleanValue(indep.independent);
  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::readySet(const std::vector<Node *> &region,
                              const std::vector<Node *> &scheduled) {
  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  std::set<Node *> region_set(region.begin(), region.end());
  std::set<Node *> scheduled_set(scheduled.begin(), scheduled.end());

  TransformQuery query(pdg_);
  LLVMQueryContext llvm_context;
  PDGQueryResult ready = query.readySet(PDGQueryScope::nodeSet(region_set),
                                        scheduled_set, llvm_context);

  for (Node *n : ready.nodes) {
    result->addNode(n);
  }

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::criticalPath(const std::vector<Node *> &region) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::INTEGER);

  std::set<Node *> region_set(region.begin(), region.end());
  TransformQuery query(pdg_);
  LLVMQueryContext llvm_context;
  size_t length = query.criticalPathLength(PDGQueryScope::nodeSet(region_set),
                                           llvm_context);

  result->setIntegerValue(static_cast<int64_t>(length));
  return result;
}

} // namespace pdg
