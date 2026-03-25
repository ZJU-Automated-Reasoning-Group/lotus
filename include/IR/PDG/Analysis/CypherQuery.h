#pragma once

#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Core/PDGEnums.h"
#include "IR/PDG/Support/LLVMEssentials.h"

#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace pdg {

// Forward declarations
class CypherQueryPlanner;
class CypherQueryExecutor;

/**
 * @brief Cypher query error codes for comprehensive error handling
 */
enum class CypherErrorCode {
  SUCCESS = 0,
  PARSE_ERROR,
  SYNTAX_ERROR,
  UNKNOWN_TOKEN,
  UNEXPECTED_TOKEN,
  MISSING_TOKEN,
  INVALID_PATTERN,
  INVALID_WHERE_CLAUSE,
  INVALID_RETURN,
  INVALID_RELATIONSHIP,
  UNKNOWN_LABEL,
  UNKNOWN_RELATIONSHIP_TYPE,
  EXECUTION_ERROR,
  TIMEOUT,
  MEMORY_LIMIT_EXCEEDED,
  INVALID_PARAMETER,
  UNSUPPORTED_FEATURE
};

/**
 * @brief Detailed error information for query failures
 */
struct CypherError {
  CypherErrorCode code;
  std::string message;
  int line;
  int column;
  std::string query;      // Portion of query with error
  std::string suggestion; // Helpful suggestion for fixing

  CypherError(CypherErrorCode c = CypherErrorCode::SUCCESS,
              const std::string &msg = "", int ln = 0, int col = 0)
      : code(c), message(msg), line(ln), column(col), suggestion("") {}

  std::string toString() const {
    std::ostringstream oss;
    oss << "Error";
    if (line > 0 || column > 0) {
      oss << " at line " << line << ", column " << column;
    }
    oss << ": " << message;
    if (!suggestion.empty()) {
      oss << " (suggestion: " << suggestion << ")";
    }
    return oss.str();
  }
};

/**
 * @brief Supported comparison operators in WHERE clauses
 */
enum class CypherComparisonOp {
  EQUALS,
  NOT_EQUALS,
  LESS_THAN,
  LESS_THAN_OR_EQUAL,
  GREATER_THAN,
  GREATER_THAN_OR_EQUAL,
  IS_NULL,
  IS_NOT_NULL,
  STARTS_WITH,
  ENDS_WITH,
  CONTAINS,
  IN
};

/**
 * @brief WHERE clause expression types
 */
enum class CypherWhereType {
  BOOLEAN,    // AND, OR, NOT
  COMPARISON, // Variable op value
  PROPERTY,   // Property access
  EXISTS,     // EXISTS clause
  REGEX       // Regular expression match
};

/**
 * @brief Represents a node pattern in Cypher (e.g., (n:NodeType))
 */
class CypherNodePattern {
public:
  CypherNodePattern(const std::string &variable = "",
                    const std::string &label = "")
      : variable_(variable), label_(label) {}

  const std::string &getVariable() const { return variable_; }
  const std::string &getLabel() const { return label_; }
  void setVariable(const std::string &v) { variable_ = v; }
  void setLabel(const std::string &l) { label_ = l; }

  void addProperty(const std::string &key, const std::string &value) {
    properties_[key] = value;
  }

  const std::unordered_map<std::string, std::string> &getProperties() const {
    return properties_;
  }

  bool hasProperty(const std::string &key) const {
    return properties_.find(key) != properties_.end();
  }

  const std::string *getProperty(const std::string &key) const {
    auto it = properties_.find(key);
    if (it != properties_.end()) {
      return &(it->second);
    }
    return nullptr;
  }

private:
  std::string variable_;
  std::string label_;
  std::unordered_map<std::string, std::string> properties_;
};

/**
 * @brief Represents a relationship pattern in Cypher (e.g., [r:EDGE_TYPE])
 */
class CypherRelationshipPattern {
public:
  enum class Direction {
    OUT, // (a)-[:T]->(b)
    IN,  // (a)<-[:T]-(b)
    BOTH // (a)-[:T]-(b) or (a)<-[:T]->(b)
  };

  CypherRelationshipPattern(const std::string &variable = "",
                            const std::string &type = "",
                            Direction direction = Direction::OUT)
      : variable_(variable), type_(type), direction_(direction) {}

  const std::string &getVariable() const { return variable_; }
  const std::string &getType() const { return type_; }
  Direction getDirection() const { return direction_; }
  bool isBidirectional() const { return direction_ == Direction::BOTH; }

  void setVariable(const std::string &v) { variable_ = v; }
  void setType(const std::string &t) { type_ = t; }
  void setDirection(Direction d) { direction_ = d; }

  void setMinHops(int min) { minHops_ = std::max(1, min); }
  void setMaxHops(int max) { maxHops_ = max; }
  int getMinHops() const { return minHops_; }
  int getMaxHops() const { return maxHops_; }
  bool hasVariableLength() const { return minHops_ != 1 || maxHops_ != 1; }

  void addProperty(const std::string &key, const std::string &value) {
    properties_[key] = value;
  }

  const std::unordered_map<std::string, std::string> &getProperties() const {
    return properties_;
  }

private:
  std::string variable_;
  std::string type_;
  Direction direction_ = Direction::OUT;
  int minHops_ = 1;
  int maxHops_ = 1;
  std::unordered_map<std::string, std::string> properties_;
};

/**
 * @brief Represents a complete pattern element (node-relationship-node)
 */
class CypherPatternElement {
public:
  CypherPatternElement(std::unique_ptr<CypherNodePattern> start)
      : startNode_(std::move(start)) {}

  void setRelationship(std::unique_ptr<CypherRelationshipPattern> rel) {
    relationship_ = std::move(rel);
  }

  void setEndNode(std::unique_ptr<CypherNodePattern> end) {
    endNode_ = std::move(end);
  }

  CypherPatternElement *
  addNextElement(std::unique_ptr<CypherPatternElement> next) {
    nextElements_.push_back(std::move(next));
    return nextElements_.back().get();
  }

  const CypherNodePattern *getStartNode() const { return startNode_.get(); }
  const CypherRelationshipPattern *getRelationship() const {
    return relationship_.get();
  }
  const CypherNodePattern *getEndNode() const { return endNode_.get(); }
  const std::vector<std::unique_ptr<CypherPatternElement>> &
  getNextElements() const {
    return nextElements_;
  }

  CypherNodePattern *getStartNode() { return startNode_.get(); }
  CypherRelationshipPattern *getRelationship() { return relationship_.get(); }
  CypherNodePattern *getEndNode() { return endNode_.get(); }

private:
  std::unique_ptr<CypherNodePattern> startNode_;
  std::unique_ptr<CypherRelationshipPattern> relationship_;
  std::unique_ptr<CypherNodePattern> endNode_;
  std::vector<std::unique_ptr<CypherPatternElement>> nextElements_;
};

/**
 * @brief WHERE clause condition with comprehensive operator support
 */
class CypherWhereClause {
public:
  CypherWhereClause() = default;

  explicit CypherWhereClause(const std::string &variable)
      : type_(CypherWhereType::PROPERTY), variableName_(variable) {}

  // Factory methods for different clause types
  static std::unique_ptr<CypherWhereClause>
  makeAnd(std::unique_ptr<CypherWhereClause> left,
          std::unique_ptr<CypherWhereClause> right) {
    auto clause = std::make_unique<CypherWhereClause>();
    clause->type_ = CypherWhereType::BOOLEAN;
    clause->boolOp_ = "AND";
    clause->left_ = std::move(left);
    clause->right_ = std::move(right);
    return clause;
  }

  static std::unique_ptr<CypherWhereClause>
  makeOr(std::unique_ptr<CypherWhereClause> left,
         std::unique_ptr<CypherWhereClause> right) {
    auto clause = std::make_unique<CypherWhereClause>();
    clause->type_ = CypherWhereType::BOOLEAN;
    clause->boolOp_ = "OR";
    clause->left_ = std::move(left);
    clause->right_ = std::move(right);
    return clause;
  }

  static std::unique_ptr<CypherWhereClause>
  makeNot(std::unique_ptr<CypherWhereClause> expr) {
    auto clause = std::make_unique<CypherWhereClause>();
    clause->type_ = CypherWhereType::BOOLEAN;
    clause->boolOp_ = "NOT";
    clause->child_ = std::move(expr);
    return clause;
  }

  static std::unique_ptr<CypherWhereClause>
  makeComparison(const std::string &variable, const std::string &property,
                 CypherComparisonOp op, const std::string &value) {
    auto clause = std::make_unique<CypherWhereClause>();
    clause->variableName_ = variable;
    clause->property_ = property;
    clause->comparisonOp_ = op;
    clause->value_ = value;
    clause->type_ = CypherWhereType::COMPARISON;
    return clause;
  }

  static std::unique_ptr<CypherWhereClause>
  makeInList(const std::string &variable, const std::string &property,
             std::vector<std::string> values) {
    auto clause = std::make_unique<CypherWhereClause>();
    clause->variableName_ = variable;
    clause->property_ = property;
    clause->comparisonOp_ = CypherComparisonOp::IN;
    clause->listValues_ = std::move(values);
    clause->type_ = CypherWhereType::COMPARISON;
    return clause;
  }

  static std::unique_ptr<CypherWhereClause>
  makeExists(const std::string &variable, const std::string &property = "") {
    auto clause = std::make_unique<CypherWhereClause>();
    clause->variableName_ = variable;
    clause->property_ = property;
    clause->type_ = CypherWhereType::EXISTS;
    return clause;
  }

  // Accessors
  CypherWhereType getType() const { return type_; }
  const std::string &getVariableName() const { return variableName_; }
  const std::string &getProperty() const { return property_; }
  const std::string &getValue() const { return value_; }
  const std::vector<std::string> &getListValues() const { return listValues_; }
  CypherComparisonOp getComparisonOp() const { return comparisonOp_; }
  const std::string &getBoolOp() const { return boolOp_; }

  const CypherWhereClause *getLeft() const { return left_.get(); }
  const CypherWhereClause *getRight() const { return right_.get(); }
  const CypherWhereClause *getChild() const { return child_.get(); }

  bool isBooleanOp() const { return type_ == CypherWhereType::BOOLEAN; }
  bool isComparison() const { return type_ == CypherWhereType::COMPARISON; }
  bool isExists() const { return type_ == CypherWhereType::EXISTS; }

private:
  CypherWhereType type_ = CypherWhereType::PROPERTY;
  std::string variableName_;
  std::string property_;
  std::string value_;
  std::vector<std::string> listValues_;
  CypherComparisonOp comparisonOp_ = CypherComparisonOp::EQUALS;
  std::string boolOp_;

  std::unique_ptr<CypherWhereClause> left_;
  std::unique_ptr<CypherWhereClause> right_;
  std::unique_ptr<CypherWhereClause> child_;
};

/**
 * @brief RETURN clause item
 */
class CypherReturnItem {
public:
  enum class Kind {
    VARIABLE_OR_PROPERTY, // "n" or "n.prop"
    COUNT                 // "COUNT(*)", "COUNT(n)", "COUNT(DISTINCT n.prop)"
  };

  CypherReturnItem(const std::string &variable, const std::string &alias = "")
      : kind_(Kind::VARIABLE_OR_PROPERTY), variable_(variable), alias_(alias) {}

  static std::unique_ptr<CypherReturnItem>
  makeCount(const std::string &arg, bool distinct = false,
            const std::string &alias = "") {
    auto item = std::make_unique<CypherReturnItem>("", alias);
    item->kind_ = Kind::COUNT;
    item->aggArg_ = arg;
    item->aggDistinct_ = distinct;
    return item;
  }

  Kind getKind() const { return kind_; }
  const std::string &getVariable() const { return variable_; }
  const std::string &getAlias() const { return alias_; }
  bool hasAlias() const { return !alias_.empty(); }
  const std::string &getAggArg() const { return aggArg_; }
  bool isAggDistinct() const { return aggDistinct_; }

  void setVariable(const std::string &v) { variable_ = v; }
  void setAlias(const std::string &a) { alias_ = a; }

private:
  Kind kind_{Kind::VARIABLE_OR_PROPERTY};
  std::string variable_;
  std::string alias_;
  std::string aggArg_;
  bool aggDistinct_{false};
};

/**
 * @brief ORDER BY specification
 */
class CypherOrderBy {
public:
  enum class Direction { ASC, DESC };

  CypherOrderBy(const std::string &variable, const std::string &property = "",
                Direction dir = Direction::ASC)
      : variable_(variable), property_(property), direction_(dir) {}

  const std::string &getVariable() const { return variable_; }
  const std::string &getProperty() const { return property_; }
  Direction getDirection() const { return direction_; }
  bool isAscending() const { return direction_ == Direction::ASC; }

private:
  std::string variable_;
  std::string property_;
  Direction direction_;
};

/**
 * @brief Complete Cypher query representation with all clauses
 */
class CypherQuery {
public:
  void addPattern(std::unique_ptr<CypherPatternElement> pattern) {
    patterns_.push_back(std::move(pattern));
  }

  void setWhereClause(std::unique_ptr<CypherWhereClause> where) {
    whereClause_ = std::move(where);
  }

  void addReturnItem(std::unique_ptr<CypherReturnItem> item) {
    returnItems_.push_back(std::move(item));
  }

  void setOrderBy(std::unique_ptr<CypherOrderBy> orderBy) {
    orderBy_ = std::move(orderBy);
  }

  void setLimit(int limit) { limit_ = limit > 0 ? limit : -1; }

  void addWithItem(std::unique_ptr<CypherReturnItem> item) {
    withItems_.push_back(std::move(item));
  }

  void setCreateClause(std::unique_ptr<CypherPatternElement> pattern) {
    createPattern_ = std::move(pattern);
  }

  void setDeleteClause(const std::vector<std::string> &targets) {
    deleteTargets_ = targets;
  }

  void addSetItem(const std::string &variable, const std::string &property,
                  const std::string &value) {
    setItems_.push_back({variable, property, value});
  }

  // Accessors
  const std::vector<std::unique_ptr<CypherPatternElement>> &
  getPatterns() const {
    return patterns_;
  }
  const CypherWhereClause *getWhereClause() const { return whereClause_.get(); }
  const std::vector<std::unique_ptr<CypherReturnItem>> &getReturnItems() const {
    return returnItems_;
  }
  const CypherOrderBy *getOrderBy() const { return orderBy_.get(); }
  int getLimit() const { return limit_; }
  const std::vector<std::unique_ptr<CypherReturnItem>> &getWithItems() const {
    return withItems_;
  }
  const CypherPatternElement *getCreatePattern() const {
    return createPattern_.get();
  }
  const std::vector<std::string> &getDeleteTargets() const {
    return deleteTargets_;
  }
  const std::vector<std::tuple<std::string, std::string, std::string>> &
  getSetItems() const {
    return setItems_;
  }

  bool hasWhere() const { return whereClause_ != nullptr; }
  bool hasOrderBy() const { return orderBy_ != nullptr; }
  bool hasLimit() const { return limit_ > 0; }
  bool hasCreate() const { return createPattern_ != nullptr; }
  bool hasDelete() const { return !deleteTargets_.empty(); }
  bool hasSet() const { return !setItems_.empty(); }

private:
  std::vector<std::unique_ptr<CypherPatternElement>> patterns_;
  std::unique_ptr<CypherWhereClause> whereClause_;
  std::vector<std::unique_ptr<CypherReturnItem>> returnItems_;
  std::unique_ptr<CypherOrderBy> orderBy_;
  int limit_ = -1;
  std::vector<std::unique_ptr<CypherReturnItem>> withItems_;
  std::unique_ptr<CypherPatternElement> createPattern_;
  std::vector<std::string> deleteTargets_;
  std::vector<std::tuple<std::string, std::string, std::string>> setItems_;
};

/**
 * @brief Query parameters for parameterized queries
 */
using CypherQueryParameters = std::unordered_map<std::string, std::string>;

/**
 * @brief Compiled query with cached execution plan
 */
class CypherCompiledQuery {
public:
  std::unique_ptr<CypherQuery> query;
  CypherQueryParameters params;
  std::chrono::steady_clock::time_point compiledAt;
  size_t useCount = 0;

  CypherCompiledQuery() = default;
  CypherCompiledQuery(std::unique_ptr<CypherQuery> q, CypherQueryParameters p)
      : query(std::move(q)), params(std::move(p)) {
    compiledAt = std::chrono::steady_clock::now();
  }

  bool isExpired(std::chrono::seconds maxAge) const {
    auto now = std::chrono::steady_clock::now();
    return (now - compiledAt) > maxAge;
  }
};

/**
 * @brief Cypher query parser with comprehensive error handling
 */
class CypherParser {
public:
  CypherParser() = default;

  std::unique_ptr<CypherQuery> parse(const std::string &query);
  std::unique_ptr<CypherQuery> parse(const std::string &query,
                                     const CypherQueryParameters &params);

  const CypherError &getLastError() const { return lastError_; }
  bool hasError() const { return lastError_.code != CypherErrorCode::SUCCESS; }

  // Utility methods
  static std::string escapeString(const std::string &s);
  static std::string unescapeString(const std::string &s);
  static bool isValidIdentifier(const std::string &s);
  static bool isKeyword(const std::string &s);

private:
  CypherError lastError_;
  const CypherQueryParameters *activeParams_ = nullptr;

  void setError(CypherErrorCode code, const std::string &message, int line = 0,
                int col = 0) {
    lastError_ = CypherError(code, message, line, col);
  }

  void clearError() { lastError_ = CypherError(); }

  // Parsing helpers
  void trim(std::string &s);
  bool isAlpha(char c);
  bool isDigit(char c);
  bool isAlphaNumeric(char c);

  // Tokenization
  std::vector<std::string> tokenize(const std::string &s);

  // Parsing functions
  std::unique_ptr<CypherQuery> parseQuery(std::vector<std::string> &tokens,
                                          size_t &pos,
                                          const CypherQueryParameters &params);
  std::unique_ptr<CypherPatternElement>
  parsePattern(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherNodePattern>
  parseNodePattern(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherRelationshipPattern>
  parseRelationshipPattern(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherWhereClause>
  parseWhereClause(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherWhereClause>
  parseBooleanExpression(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherWhereClause>
  parseOrExpression(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherWhereClause>
  parseAndExpression(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherWhereClause>
  parseUnaryExpression(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherWhereClause>
  parseComparison(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherReturnItem>
  parseReturnItem(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherOrderBy> parseOrderBy(std::vector<std::string> &tokens,
                                              size_t &pos);

  // Helper methods
  bool hasMore(const std::vector<std::string> &tokens, size_t pos);
  const std::string &peek(const std::vector<std::string> &tokens, size_t pos);
  const std::string &consume(std::vector<std::string> &tokens, size_t &pos);
  bool matchToken(const std::string &expected, std::vector<std::string> &tokens,
                  size_t &pos);

  // Parameter substitution
  std::string substituteParameter(const std::string &name,
                                  const CypherQueryParameters &params);
};

/**
 * @brief Result of query execution
 */
class CypherResult {
public:
  enum class ResultType {
    NODES,
    RELATIONSHIPS,
    PATHS,
    SCALAR,
    INTEGER,
    BOOLEAN
  };

  CypherResult(ResultType type = ResultType::NODES) : type_(type) {}

  void addNode(Node *node) { nodes_.push_back(node); }

  void addEdge(Edge *edge) { relationships_.push_back(edge); }

  void addRelationship(Edge *edge) { relationships_.push_back(edge); }

  void setScalarValue(const std::string &value) {
    type_ = ResultType::SCALAR;
    scalarValue_ = value;
  }

  void setIntegerValue(int64_t value) {
    type_ = ResultType::INTEGER;
    integerValue_ = value;
  }

  void setBooleanValue(bool value) {
    type_ = ResultType::BOOLEAN;
    booleanValue_ = value;
  }

  ResultType getType() const { return type_; }
  const std::vector<Node *> &getNodes() const { return nodes_; }
  const std::vector<Edge *> &getRelationships() const { return relationships_; }
  const std::string &getScalarValue() const { return scalarValue_; }
  int64_t getIntegerValue() const { return integerValue_; }
  bool getBooleanValue() const { return booleanValue_; }

  std::string toString() const;
  bool isEmpty() const {
    return nodes_.empty() && relationships_.empty() && scalarValue_.empty() &&
           integerValue_ == 0 && !booleanValue_;
  }

  size_t getCount() const {
    switch (type_) {
    case ResultType::NODES:
      return nodes_.size();
    case ResultType::RELATIONSHIPS:
      return relationships_.size();
    case ResultType::INTEGER:
      return 1;
    case ResultType::BOOLEAN:
      return 1;
    default:
      return scalarValue_.empty() ? 0 : 1;
    }
  }

private:
  ResultType type_;
  std::vector<Node *> nodes_;
  std::vector<Edge *> relationships_;
  std::string scalarValue_;
  int64_t integerValue_ = 0;
  bool booleanValue_ = false;
};

/**
 * @brief Query execution statistics
 */
struct CypherQueryStats {
  std::chrono::microseconds parseTime{0};
  std::chrono::microseconds executionTime{0};
  size_t nodesVisited = 0;
  size_t edgesVisited = 0;
  size_t resultsReturned = 0;
  bool usedCache = false;
  bool timedOut = false;
};

/**
 * @brief Cypher query executor with caching and optimization
 */
class CypherQueryExecutor {
public:
  CypherQueryExecutor(ProgramGraph &pdg) : pdg_(pdg) {}

  std::unique_ptr<CypherResult> execute(const CypherQuery &query);
  std::unique_ptr<CypherResult> execute(const CypherQuery &query,
                                        CypherQueryStats &stats);

  // PDG-specific operations mapped to Cypher
  std::unique_ptr<CypherResult> matchNodes(const std::string &label,
                                           const std::string &variable);
  std::unique_ptr<CypherResult> matchEdges(const std::string &type,
                                           const std::string &variable);
  std::unique_ptr<CypherResult>
  matchPattern(const CypherPatternElement *pattern);
  std::unique_ptr<CypherResult>
  traverse(Node *start, const CypherRelationshipPattern &rel, int maxHops);
  std::unique_ptr<CypherResult> filterByWhere(const std::vector<Node *> &nodes,
                                              const CypherWhereClause &where);
  std::unique_ptr<CypherResult> filterByWhere(const std::vector<Edge *> &edges,
                                              const CypherWhereClause &where);

  // Optimizer query functions
  std::unique_ptr<CypherResult> canMoveEarlier(Node *moving, Node *anchor);
  std::unique_ptr<CypherResult> canMoveLater(Node *moving, Node *anchor);
  std::unique_ptr<CypherResult> independent(Node *a, Node *b);
  std::unique_ptr<CypherResult> readySet(const std::vector<Node *> &region,
                                         const std::vector<Node *> &scheduled);
  std::unique_ptr<CypherResult> criticalPath(const std::vector<Node *> &region);

  // Utility
  ProgramGraph &getPDG() { return pdg_; }
  const ProgramGraph &getPDG() const { return pdg_; }

  // Introspection helpers (useful for tooling / result rendering)
  const std::vector<Node *> *getBoundVariable(const std::string &name) const {
    auto it = boundVariables_.find(name);
    if (it == boundVariables_.end())
      return nullptr;
    return &it->second;
  }

  const std::vector<Edge *> *
  getBoundRelationship(const std::string &name) const {
    auto it = boundRelationships_.find(name);
    if (it == boundRelationships_.end())
      return nullptr;
    return &it->second;
  }

  std::string getNodePropertyString(Node *node, const std::string &property) {
    return getNodeProperty(node, property);
  }

  std::string getEdgePropertyString(Edge *edge, const std::string &property) {
    return getEdgeProperty(edge, property);
  }

  void setError(const std::string &error) { lastError_ = error; }
  const std::string &getLastError() const { return lastError_; }

  // Caching
  void clearCache() { queryCache_.clear(); }
  void setCacheMaxSize(size_t maxSize) { cacheMaxSize_ = maxSize; }
  void setQueryTimeout(std::chrono::seconds timeout) {
    queryTimeout_ = timeout;
  }

  void setUnboundedMaxHops(int maxHops) {
    unboundedMaxHops_ = std::max(1, maxHops);
  }

  const CypherQueryStats &getLastStats() const { return lastStats_; }

private:
  struct MatchRow {
    std::unordered_map<std::string, Node *> nodes;
    std::unordered_map<std::string, Edge *> rels;
  };

  ProgramGraph &pdg_;
  std::string lastError_;
  CypherQueryStats lastStats_;

  // Query caching
  std::unordered_map<std::string, CypherCompiledQuery> queryCache_;
  size_t cacheMaxSize_ = 100;
  std::chrono::seconds queryTimeout_{30};

  std::unordered_map<std::string, std::vector<Node *>> boundVariables_;
  std::unordered_map<std::string, std::vector<Edge *>> boundRelationships_;
  int unboundedMaxHops_ = 5;

  // Helper methods
  bool evaluateCondition(const CypherWhereClause &condition,
                         const MatchRow &row);
  bool evaluateCondition(const CypherWhereClause &condition, Node *node);
  bool evaluateCondition(const CypherWhereClause &condition, Edge *edge);
  std::string getNodeProperty(Node *node, const std::string &property);
  std::string getEdgeProperty(Edge *edge, const std::string &property);

  bool applyComparison(const std::string &nodeValue, CypherComparisonOp op,
                       const std::string &queryValue);

  // Caching helpers
  std::string generateCacheKey(const CypherQuery &query);
  std::unique_ptr<CypherResult> getFromCache(const std::string &key);
  void addToCache(const std::string &key, const CypherQuery &query,
                  std::unique_ptr<CypherResult> result);

  // Performance optimization
  bool shouldUseIndex(const std::string &label);
  std::vector<Node *> getNodesByLabel(const std::string &label);
};

} // namespace pdg
