/**
 * @file CypherQueryExtensions.h
 * @brief Extended Cypher query capabilities for PDG analysis
 *
 * This file extends the base CypherQuery implementation with:
 * 1. Aggregation functions (COUNT, SUM, AVG, MIN, MAX, COLLECT)
 * 2. Regular path queries with path constraints
 * 3. Subgraph pattern matching and isomorphism
 * 4. WITH clause for query composition and pipelining
 * 5. OPTIONAL MATCH for left-outer-join semantics
 * 6. Path predicates and filters (ALL, ANY, NONE, SINGLE)
 * 7. List comprehensions and pattern comprehensions
 * 8. UNWIND for list expansion
 * 9. CASE expressions for conditional logic
 * 10. Temporal/quantitative path queries
 *
 * References:
 * - Cypher Query Language Reference (Neo4j)
 * - Mendelzon & Wood, "Finding Regular Simple Paths in Graph Databases", SIAM
 * 1995
 * - Angles et al., "G-CORE: A Core for Future Graph Query Languages", SIGMOD
 * 2018
 */

#pragma once

#include "IR/PDG/Analysis/CypherQuery.h"

#include <functional>
#include <numeric>
#include <optional>
#include <regex>

namespace pdg {

// ============================================================================
// Aggregation Functions
// ============================================================================

/**
 * @brief Supported aggregation function types
 */
enum class AggregationFunction {
  COUNT,            // Count number of items
  COUNT_DISTINCT,   // Count unique items
  SUM,              // Sum numeric values
  AVG,              // Average of numeric values
  MIN,              // Minimum value
  MAX,              // Maximum value
  COLLECT,          // Collect all values into a list
  COLLECT_DISTINCT, // Collect unique values
  STDEV,            // Standard deviation
  PERCENTILE        // Percentile (requires parameter)
};

/**
 * @brief Aggregation expression in RETURN clause
 *
 * Example: RETURN COUNT(n), AVG(n.depth), COLLECT(n.name)
 */
class CypherAggregation {
public:
  CypherAggregation(AggregationFunction func, const std::string &expr,
                    const std::string &alias = "")
      : function_(func), expression_(expr), alias_(alias), percentile_(0.5) {}

  AggregationFunction getFunction() const { return function_; }
  std::string getExpression() const { return expression_; }
  std::string getAlias() const { return alias_; }

  void setPercentile(double p) { percentile_ = p; }
  double getPercentile() const { return percentile_; }

  // Apply aggregation to a collection of values
  std::string apply(const std::vector<std::string> &values) const;
  double applyNumeric(const std::vector<double> &values) const;

private:
  AggregationFunction function_;
  std::string expression_;
  std::string alias_;
  double percentile_; // For PERCENTILE function
};

// ============================================================================
// Regular Path Queries
// ============================================================================

/**
 * @brief Path constraint types for regular path queries
 */
enum class PathConstraintType {
  SIMPLE,         // No repeated nodes (simple path)
  TRAIL,          // No repeated edges (trail)
  ACYCLIC,        // No cycles
  SHORTEST,       // Shortest path only
  ALL_SHORTEST,   // All shortest paths
  K_SHORTEST,     // K shortest paths
  REGEX,          // Edge labels match regex
  LENGTH_BOUNDED, // Path length within bounds
  COST_BOUNDED    // Path cost within bounds
};

/**
 * @brief Regular path query pattern
 *
 * Supports complex path constraints beyond simple variable-length patterns.
 * Example: MATCH p = (a)-[:DATA_DEF_USE|DATA_RAW*1..5]->(b)
 *          WHERE ALL(r IN relationships(p) WHERE r.weight > 0)
 */
class RegularPathQuery {
public:
  RegularPathQuery() : minLength_(1), maxLength_(10), k_(1) {}

  void addConstraint(PathConstraintType type) { constraints_.push_back(type); }
  void setEdgeLabelRegex(const std::string &regex) { edgeLabelRegex_ = regex; }
  void setLengthBounds(int min, int max) {
    minLength_ = min;
    maxLength_ = max;
  }
  void setK(int k) { k_ = k; }

  // Path validation
  bool satisfiesConstraints(const std::vector<Node *> &path,
                            const std::vector<Edge *> &edges) const;

  // Path enumeration with constraints
  std::vector<std::vector<Node *>> findPaths(Node *start, Node *end,
                                             ProgramGraph &pdg) const;

private:
  std::vector<PathConstraintType> constraints_;
  std::string edgeLabelRegex_;
  int minLength_;
  int maxLength_;
  int k_; // For K_SHORTEST

  bool isSimplePath(const std::vector<Node *> &path) const;
  bool isTrail(const std::vector<Edge *> &edges) const;
  bool matchesRegex(const std::vector<Edge *> &edges) const;
};

// ============================================================================
// Subgraph Pattern Matching
// ============================================================================

/**
 * @brief Subgraph pattern for isomorphism queries
 *
 * Represents a template graph pattern to match against the PDG.
 * Example: Find all instances of a specific code pattern (e.g., use-after-free)
 */
class SubgraphPattern {
public:
  struct PatternNode {
    std::string id;
    std::unordered_map<std::string, std::string> properties;
    std::optional<GraphNodeType> nodeType;
  };

  struct PatternEdge {
    std::string srcId;
    std::string dstId;
    std::optional<EdgeType> edgeType;
    std::unordered_map<std::string, std::string> properties;
  };

  void addNode(const PatternNode &node) { nodes_.push_back(node); }
  void addEdge(const PatternEdge &edge) { edges_.push_back(edge); }

  // Find all subgraph isomorphisms
  std::vector<std::unordered_map<std::string, Node *>>
  findMatches(ProgramGraph &pdg) const;

  // Check if a candidate mapping is valid
  bool isValidMapping(const std::unordered_map<std::string, Node *> &mapping,
                      ProgramGraph &pdg) const;

private:
  std::vector<PatternNode> nodes_;
  std::vector<PatternEdge> edges_;

  // VF2 algorithm for subgraph isomorphism
  void
  vf2Match(ProgramGraph &pdg,
           std::unordered_map<std::string, Node *> &currentMapping,
           std::vector<std::unordered_map<std::string, Node *>> &results) const;
};

// ============================================================================
// WITH Clause for Query Composition
// ============================================================================

/**
 * @brief WITH clause for query pipelining
 *
 * Allows chaining multiple query stages with intermediate projections.
 * Example: MATCH (a)-[r]->(b) WITH a, COUNT(r) AS degree WHERE degree > 5
 * RETURN a
 */
class CypherWithClause {
public:
  void addProjection(const std::string &variable,
                     const std::string &alias = "") {
    projections_.push_back({variable, alias.empty() ? variable : alias});
  }

  void addAggregation(const CypherAggregation &agg) {
    aggregations_.push_back(agg);
  }

  void setWhereClause(std::unique_ptr<CypherWhereClause> where) {
    whereClause_ = std::move(where);
  }

  const std::vector<std::pair<std::string, std::string>> &
  getProjections() const {
    return projections_;
  }

  const std::vector<CypherAggregation> &getAggregations() const {
    return aggregations_;
  }

  const CypherWhereClause *getWhereClause() const { return whereClause_.get(); }

private:
  std::vector<std::pair<std::string, std::string>>
      projections_; // (variable, alias)
  std::vector<CypherAggregation> aggregations_;
  std::unique_ptr<CypherWhereClause> whereClause_;
};

// ============================================================================
// OPTIONAL MATCH
// ============================================================================

/**
 * @brief Optional pattern matching (left outer join semantics)
 *
 * Example: MATCH (a) OPTIONAL MATCH (a)-[r]->(b) RETURN a, b
 * Returns 'a' even if no relationship exists, with 'b' as null
 */
class CypherOptionalMatch {
public:
  CypherOptionalMatch(std::unique_ptr<CypherPatternElement> pattern)
      : pattern_(std::move(pattern)) {}

  void setWhereClause(std::unique_ptr<CypherWhereClause> where) {
    whereClause_ = std::move(where);
  }

  const CypherPatternElement *getPattern() const { return pattern_.get(); }
  const CypherWhereClause *getWhereClause() const { return whereClause_.get(); }

private:
  std::unique_ptr<CypherPatternElement> pattern_;
  std::unique_ptr<CypherWhereClause> whereClause_;
};

// ============================================================================
// Path Predicates
// ============================================================================

/**
 * @brief Path predicate types for filtering
 */
enum class PathPredicateType {
  ALL,   // All elements satisfy condition
  ANY,   // At least one element satisfies condition
  NONE,  // No elements satisfy condition
  SINGLE // Exactly one element satisfies condition
};

/**
 * @brief Path predicate for filtering paths
 *
 * Example: WHERE ALL(n IN nodes(p) WHERE n.type = 'INST_FUNCALL')
 */
class PathPredicate {
public:
  PathPredicate(PathPredicateType type, const std::string &variable,
                const std::string &collection,
                std::unique_ptr<CypherWhereClause> condition)
      : type_(type), variable_(variable), collection_(collection),
        condition_(std::move(condition)) {}

  bool evaluate(const std::vector<Node *> &nodes) const;
  bool evaluate(const std::vector<Edge *> &edges) const;

private:
  PathPredicateType type_;
  std::string variable_;
  std::string collection_;
  std::unique_ptr<CypherWhereClause> condition_;
};

// ============================================================================
// List Comprehensions
// ============================================================================

/**
 * @brief List comprehension for transforming collections
 *
 * Example: [n IN nodes(p) | n.name]
 * Example: [n IN nodes(p) WHERE n.type = 'INST_FUNCALL' | n.line]
 */
class ListComprehension {
public:
  ListComprehension(const std::string &variable, const std::string &collection,
                    const std::string &expression)
      : variable_(variable), collection_(collection), expression_(expression) {}

  void setFilter(std::unique_ptr<CypherWhereClause> filter) {
    filter_ = std::move(filter);
  }

  std::vector<std::string> evaluate(const std::vector<Node *> &nodes) const;

private:
  std::string variable_;
  std::string collection_;
  std::string expression_;
  std::unique_ptr<CypherWhereClause> filter_;
};

// ============================================================================
// CASE Expressions
// ============================================================================

/**
 * @brief CASE expression for conditional logic
 *
 * Example: CASE WHEN n.type = 'INST_FUNCALL' THEN 'call'
 *               WHEN n.type = 'INST_RET' THEN 'return'
 *               ELSE 'other' END
 */
class CaseExpression {
public:
  struct WhenClause {
    std::unique_ptr<CypherWhereClause> condition;
    std::string result;
  };

  void addWhen(std::unique_ptr<CypherWhereClause> condition,
               const std::string &result) {
    whenClauses_.push_back({std::move(condition), result});
  }

  void setElse(const std::string &result) { elseResult_ = result; }

  std::string evaluate(Node *node) const;

private:
  std::vector<WhenClause> whenClauses_;
  std::string elseResult_;
};

// ============================================================================
// UNWIND for List Expansion
// ============================================================================

/**
 * @brief UNWIND clause for expanding lists into rows
 *
 * Example: UNWIND [1, 2, 3] AS x RETURN x
 * Example: MATCH p = (a)-[*]->(b) UNWIND nodes(p) AS n RETURN n
 */
class CypherUnwind {
public:
  CypherUnwind(const std::string &listExpr, const std::string &alias)
      : listExpression_(listExpr), alias_(alias) {}

  std::string getListExpression() const { return listExpression_; }
  std::string getAlias() const { return alias_; }

private:
  std::string listExpression_;
  std::string alias_;
};

// ============================================================================
// Extended Query Executor
// ============================================================================

/**
 * @brief Extended Cypher query executor with advanced features
 */
class ExtendedCypherExecutor : public CypherQueryExecutor {
public:
  ExtendedCypherExecutor(ProgramGraph &pdg) : CypherQueryExecutor(pdg) {}

  // Execute query with aggregations
  std::unique_ptr<CypherResult>
  executeWithAggregation(const CypherQuery &query,
                         const std::vector<CypherAggregation> &aggregations);

  // Execute regular path query
  std::vector<std::vector<Node *>>
  executeRegularPathQuery(const RegularPathQuery &query, Node *start,
                          Node *end);

  // Execute subgraph pattern matching
  std::vector<std::unordered_map<std::string, Node *>>
  executeSubgraphMatch(const SubgraphPattern &pattern);

  // Execute query with WITH clause
  std::unique_ptr<CypherResult>
  executeWithClause(const CypherQuery &query,
                    const CypherWithClause &withClause);

  // Execute optional match
  std::unique_ptr<CypherResult>
  executeOptionalMatch(const CypherQuery &query,
                       const CypherOptionalMatch &optionalMatch);

private:
  // Helper for aggregation computation
  std::unordered_map<std::string, std::string> computeAggregations(
      const std::vector<CypherAggregation> &aggregations,
      const std::vector<std::unordered_map<std::string, Node *>> &bindings);
};

// ============================================================================
// Query Builder for Programmatic Construction
// ============================================================================

/**
 * @brief Fluent API for building Cypher queries programmatically
 *
 * Example:
 *   auto query = CypherQueryBuilder()
 *     .match("(a:INST_FUNCALL)")
 *     .relationship("r", "DATA_DEF_USE", 1, 5)
 *     .match("(b:INST_RET)")
 *     .where("a.function = 'malloc'")
 *     .returnAggregation(AggregationFunction::COUNT, "b", "leak_count")
 *     .build();
 */
class CypherQueryBuilder {
public:
  CypherQueryBuilder &match(const std::string &pattern);
  CypherQueryBuilder &optionalMatch(const std::string &pattern);
  CypherQueryBuilder &where(const std::string &condition);
  CypherQueryBuilder &with(const std::string &projection);
  CypherQueryBuilder &unwind(const std::string &list, const std::string &alias);
  CypherQueryBuilder &returnVar(const std::string &variable);
  CypherQueryBuilder &returnAggregation(AggregationFunction func,
                                        const std::string &expr,
                                        const std::string &alias = "");
  CypherQueryBuilder &orderBy(const std::string &variable,
                              bool ascending = true);
  CypherQueryBuilder &limit(int limit);

  CypherQueryBuilder &relationship(const std::string &var,
                                   const std::string &type, int minHops = 1,
                                   int maxHops = 1);

  std::unique_ptr<CypherQuery> build();

private:
  std::vector<std::string> matchClauses_;
  std::vector<std::string> optionalMatchClauses_;
  std::vector<std::string> whereClauses_;
  std::vector<std::string> withClauses_;
  std::vector<std::pair<std::string, std::string>> unwindClauses_;
  std::vector<std::string> returnItems_;
  std::vector<CypherAggregation> aggregations_;
  std::vector<std::pair<std::string, bool>> orderByClauses_;
  int limit_ = -1;
};

// ============================================================================
// Predefined Query Templates
// ============================================================================

/**
 * @brief Common query patterns for PDG analysis
 */
class CypherQueryTemplates {
public:
  // Find all paths from source to sink with specific edge types
  static std::unique_ptr<CypherQuery>
  findDataFlowPaths(const std::string &sourcePattern,
                    const std::string &sinkPattern,
                    const std::vector<EdgeType> &edgeTypes, int maxDepth = 10);

  // Find all nodes with high fan-in/fan-out
  static std::unique_ptr<CypherQuery> findHighDegreeNodes(int minDegree,
                                                          bool incoming = true);

  // Find strongly connected components
  static std::unique_ptr<CypherQuery> findStronglyConnectedComponents();

  // Find all cycles in the PDG
  static std::unique_ptr<CypherQuery> findCycles(int maxLength = 10);

  // Find all definitions reaching a use
  static std::unique_ptr<CypherQuery>
  findReachingDefinitions(const std::string &usePattern);

  // Find all uses of a definition
  static std::unique_ptr<CypherQuery>
  findReachingUses(const std::string &defPattern);

  // Find common ancestors/descendants
  static std::unique_ptr<CypherQuery>
  findCommonAncestors(const std::string &node1Pattern,
                      const std::string &node2Pattern);
};

} // namespace pdg
