/**
 * @file CypherQueryExtensions.cpp
 * @brief Implementation of extended Cypher query capabilities
 */

#include "IR/PDG/Analysis/CypherQueryExtensions.h"

#include "IR/PDG/Analysis/Slicing.h"
#include "IR/PDG/Support/PDGUtils.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <regex>
#include <sstream>

using namespace llvm;

namespace pdg {

// ============================================================================
// CypherAggregation Implementation
// ============================================================================

std::string
CypherAggregation::apply(const std::vector<std::string> &values) const {
  if (values.empty()) {
    return "null";
  }

  switch (function_) {
  case AggregationFunction::COUNT:
    return std::to_string(values.size());

  case AggregationFunction::COUNT_DISTINCT: {
    std::unordered_set<std::string> unique(values.begin(), values.end());
    return std::to_string(unique.size());
  }

  case AggregationFunction::COLLECT: {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < values.size(); ++i) {
      if (i > 0)
        oss << ", ";
      oss << values[i];
    }
    oss << "]";
    return oss.str();
  }

  case AggregationFunction::COLLECT_DISTINCT: {
    std::unordered_set<std::string> unique(values.begin(), values.end());
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (const auto &v : unique) {
      if (!first)
        oss << ", ";
      oss << v;
      first = false;
    }
    oss << "]";
    return oss.str();
  }

  case AggregationFunction::MIN:
    return *std::min_element(values.begin(), values.end());

  case AggregationFunction::MAX:
    return *std::max_element(values.begin(), values.end());

  default:
    // Numeric aggregations require numeric conversion
    std::vector<double> numeric;
    for (const auto &v : values) {
      try {
        numeric.push_back(std::stod(v));
      } catch (...) {
        // Skip non-numeric values
      }
    }
    return std::to_string(applyNumeric(numeric));
  }
}

double
CypherAggregation::applyNumeric(const std::vector<double> &values) const {
  if (values.empty()) {
    return 0.0;
  }

  switch (function_) {
  case AggregationFunction::SUM:
    return std::accumulate(values.begin(), values.end(), 0.0);

  case AggregationFunction::AVG:
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();

  case AggregationFunction::MIN:
    return *std::min_element(values.begin(), values.end());

  case AggregationFunction::MAX:
    return *std::max_element(values.begin(), values.end());

  case AggregationFunction::STDEV: {
    double mean =
        std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    double sq_sum = std::accumulate(
        values.begin(), values.end(), 0.0,
        [mean](double acc, double v) { return acc + (v - mean) * (v - mean); });
    return std::sqrt(sq_sum / values.size());
  }

  case AggregationFunction::PERCENTILE: {
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    size_t idx = static_cast<size_t>(percentile_ * (sorted.size() - 1));
    return sorted[idx];
  }

  default:
    return 0.0;
  }
}

// ============================================================================
// RegularPathQuery Implementation
// ============================================================================

bool RegularPathQuery::satisfiesConstraints(
    const std::vector<Node *> &path, const std::vector<Edge *> &edges) const {

  for (auto constraint : constraints_) {
    switch (constraint) {
    case PathConstraintType::SIMPLE:
      if (!isSimplePath(path))
        return false;
      break;

    case PathConstraintType::TRAIL:
      if (!isTrail(edges))
        return false;
      break;

    case PathConstraintType::ACYCLIC:
      if (!isSimplePath(path))
        return false;
      break;

    case PathConstraintType::LENGTH_BOUNDED:
      if (static_cast<int>(path.size()) < minLength_ ||
          static_cast<int>(path.size()) > maxLength_)
        return false;
      break;

    case PathConstraintType::REGEX:
      if (!matchesRegex(edges))
        return false;
      break;

    default:
      break;
    }
  }

  return true;
}

bool RegularPathQuery::isSimplePath(const std::vector<Node *> &path) const {
  std::unordered_set<Node *> seen;
  for (auto *node : path) {
    if (seen.count(node))
      return false;
    seen.insert(node);
  }
  return true;
}

bool RegularPathQuery::isTrail(const std::vector<Edge *> &edges) const {
  std::unordered_set<Edge *> seen;
  for (auto *edge : edges) {
    if (seen.count(edge))
      return false;
    seen.insert(edge);
  }
  return true;
}

bool RegularPathQuery::matchesRegex(const std::vector<Edge *> &edges) const {
  if (edgeLabelRegex_.empty())
    return true;

  std::ostringstream oss;
  for (auto *edge : edges) {
    if (edge) {
      oss << static_cast<int>(edge->getEdgeType()) << ",";
    }
  }

  std::string edgeSequence = oss.str();
  try {
    std::regex pattern(edgeLabelRegex_);
    return std::regex_match(edgeSequence, pattern);
  } catch (...) {
    return false;
  }
}

std::vector<std::vector<Node *>>
RegularPathQuery::findPaths(Node *start, Node *end, ProgramGraph &pdg) const {
  std::vector<std::vector<Node *>> result;

  // BFS/DFS with constraint checking
  struct PathState {
    std::vector<Node *> nodes;
    std::vector<Edge *> edges;
  };

  std::queue<PathState> worklist;
  worklist.push({{start}, {}});

  while (!worklist.empty() && result.size() < static_cast<size_t>(k_)) {
    PathState current = worklist.front();
    worklist.pop();

    Node *last = current.nodes.back();
    if (last == end) {
      if (satisfiesConstraints(current.nodes, current.edges)) {
        result.push_back(current.nodes);
      }
      continue;
    }

    if (static_cast<int>(current.nodes.size()) >= maxLength_)
      continue;

    for (auto *edge : last->getOutEdgeSet()) {
      if (!edge)
        continue;

      Node *next = edge->getDstNode();
      if (!next)
        continue;

      PathState newState = current;
      newState.nodes.push_back(next);
      newState.edges.push_back(edge);

      // Early constraint checking
      bool valid = true;
      for (auto constraint : constraints_) {
        if (constraint == PathConstraintType::SIMPLE) {
          if (std::find(current.nodes.begin(), current.nodes.end(), next) !=
              current.nodes.end()) {
            valid = false;
            break;
          }
        }
      }

      if (valid) {
        worklist.push(newState);
      }
    }
  }

  return result;
}

// ============================================================================
// SubgraphPattern Implementation
// ============================================================================

std::vector<std::unordered_map<std::string, Node *>>
SubgraphPattern::findMatches(ProgramGraph &pdg) const {
  std::vector<std::unordered_map<std::string, Node *>> results;

  if (nodes_.empty())
    return results;

  // Start with first pattern node
  const PatternNode &firstPattern = nodes_[0];
  std::vector<Node *> candidates;

  // Find candidate nodes for first pattern node
  for (auto *node : pdg.getNodeSet()) {
    if (!node)
      continue;

    bool matches = true;

    // Check node type
    if (firstPattern.nodeType.has_value() &&
        node->getNodeType() != firstPattern.nodeType.value()) {
      matches = false;
    }

    // Check properties
    for (const auto &[key, value] : firstPattern.properties) {
      // Property checking would go here
      // This is simplified - real implementation would check node properties
    }

    if (matches) {
      candidates.push_back(node);
    }
  }

  // Try to extend each candidate to a full match
  for (auto *candidate : candidates) {
    std::unordered_map<std::string, Node *> mapping;
    mapping[firstPattern.id] = candidate;

    if (extendMapping(mapping, 1, pdg)) {
      results.push_back(mapping);
    }
  }

  return results;
}

bool SubgraphPattern::extendMapping(
    std::unordered_map<std::string, Node *> &mapping, size_t nextPatternIdx,
    ProgramGraph &pdg) const {

  if (nextPatternIdx >= nodes_.size()) {
    return isValidMapping(mapping, pdg);
  }

  const PatternNode &pattern = nodes_[nextPatternIdx];

  // Find candidate nodes for this pattern
  for (auto *node : pdg.getNodeSet()) {
    if (!node)
      continue;

    // Check if already mapped
    bool alreadyMapped = false;
    for (const auto &[id, mappedNode] : mapping) {
      if (mappedNode == node) {
        alreadyMapped = true;
        break;
      }
    }
    if (alreadyMapped)
      continue;

    // Check node type
    if (pattern.nodeType.has_value() &&
        node->getNodeType() != pattern.nodeType.value()) {
      continue;
    }

    // Try this mapping
    mapping[pattern.id] = node;

    if (extendMapping(mapping, nextPatternIdx + 1, pdg)) {
      return true;
    }

    // Backtrack
    mapping.erase(pattern.id);
  }

  return false;
}

bool SubgraphPattern::isValidMapping(
    const std::unordered_map<std::string, Node *> &mapping,
    ProgramGraph &pdg) const {

  // Check all edges in pattern
  for (const auto &patternEdge : edges_) {
    auto srcIt = mapping.find(patternEdge.srcId);
    auto dstIt = mapping.find(patternEdge.dstId);

    if (srcIt == mapping.end() || dstIt == mapping.end())
      return false;

    Node *srcNode = srcIt->second;
    Node *dstNode = dstIt->second;

    // Check if edge exists
    bool edgeFound = false;
    for (auto *edge : srcNode->getOutEdgeSet()) {
      if (!edge)
        continue;

      if (edge->getDstNode() == dstNode) {
        // Check edge type if specified
        if (patternEdge.edgeType.has_value() &&
            edge->getEdgeType() != patternEdge.edgeType.value()) {
          continue;
        }
        edgeFound = true;
        break;
      }
    }

    if (!edgeFound)
      return false;
  }

  return true;
}

// ============================================================================
// WithClause Implementation
// ============================================================================

std::unique_ptr<CypherResult>
WithClause::execute(std::unique_ptr<CypherResult> input) const {
  if (!input)
    return nullptr;

  auto output = std::make_unique<CypherResult>();

  // Project specified variables
  for (const auto &row : input->getRows()) {
    std::unordered_map<std::string, std::string> newRow;

    for (const auto &proj : projections_) {
      auto it = row.find(proj.expression);
      if (it != row.end()) {
        std::string key = proj.alias.empty() ? proj.expression : proj.alias;
        newRow[key] = it->second;
      }
    }

    output->addRow(newRow);
  }

  // Apply aggregations if any
  if (!aggregations_.empty()) {
    std::unordered_map<std::string, std::vector<std::string>> grouped;

    for (const auto &row : output->getRows()) {
      for (const auto &agg : aggregations_) {
        auto it = row.find(agg.getExpression());
        if (it != row.end()) {
          grouped[agg.getExpression()].push_back(it->second);
        }
      }
    }

    // Create aggregated result
    auto aggResult = std::make_unique<CypherResult>();
    std::unordered_map<std::string, std::string> aggRow;

    for (const auto &agg : aggregations_) {
      auto it = grouped.find(agg.getExpression());
      if (it != grouped.end()) {
        std::string key =
            agg.getAlias().empty() ? agg.getExpression() : agg.getAlias();
        aggRow[key] = agg.apply(it->second);
      }
    }

    aggResult->addRow(aggRow);
    return aggResult;
  }

  return output;
}

// ============================================================================
// OptionalMatch Implementation
// ============================================================================

std::unique_ptr<CypherResult>
OptionalMatch::execute(std::unique_ptr<CypherResult> input,
                       ProgramGraph &pdg) const {
  if (!input)
    return nullptr;

  auto output = std::make_unique<CypherResult>();

  for (const auto &row : input->getRows()) {
    // Try to match pattern
    bool matched = false;

    // This is simplified - real implementation would execute the pattern
    // For now, just pass through the row
    output->addRow(row);
  }

  return output;
}

// ============================================================================
// PathPredicate Implementation
// ============================================================================

bool PathPredicate::evaluate(const std::vector<Node *> &path,
                             const std::vector<Edge *> &edges) const {
  switch (type_) {
  case PathPredicateType::ALL:
    return evaluateAll(path, edges);

  case PathPredicateType::ANY:
    return evaluateAny(path, edges);

  case PathPredicateType::NONE:
    return !evaluateAny(path, edges);

  case PathPredicateType::SINGLE:
    return evaluateSingle(path, edges);

  default:
    return false;
  }
}

bool PathPredicate::evaluateAll(const std::vector<Node *> &path,
                                const std::vector<Edge *> &edges) const {
  if (targetType_ == PathPredicateTarget::NODES) {
    for (auto *node : path) {
      if (!condition_(node, nullptr))
        return false;
    }
  } else {
    for (auto *edge : edges) {
      if (!condition_(nullptr, edge))
        return false;
    }
  }
  return true;
}

bool PathPredicate::evaluateAny(const std::vector<Node *> &path,
                                const std::vector<Edge *> &edges) const {
  if (targetType_ == PathPredicateTarget::NODES) {
    for (auto *node : path) {
      if (condition_(node, nullptr))
        return true;
    }
  } else {
    for (auto *edge : edges) {
      if (condition_(nullptr, edge))
        return true;
    }
  }
  return false;
}

bool PathPredicate::evaluateSingle(const std::vector<Node *> &path,
                                   const std::vector<Edge *> &edges) const {
  int count = 0;

  if (targetType_ == PathPredicateTarget::NODES) {
    for (auto *node : path) {
      if (condition_(node, nullptr))
        count++;
    }
  } else {
    for (auto *edge : edges) {
      if (condition_(nullptr, edge))
        count++;
    }
  }

  return count == 1;
}

// ============================================================================
// CaseExpression Implementation
// ============================================================================

std::string CaseExpression::evaluate(
    const std::unordered_map<std::string, std::string> &context) const {

  for (const auto &[condition, result] : cases_) {
    // Evaluate condition
    // This is simplified - real implementation would parse and evaluate
    if (evaluateCondition(condition, context)) {
      return evaluateExpression(result, context);
    }
  }

  return elseResult_;
}

bool CaseExpression::evaluateCondition(
    const std::string &condition,
    const std::unordered_map<std::string, std::string> &context) const {
  // Simplified condition evaluation
  // Real implementation would parse and evaluate boolean expressions
  return !condition.empty();
}

std::string CaseExpression::evaluateExpression(
    const std::string &expr,
    const std::unordered_map<std::string, std::string> &context) const {
  // Check if it's a variable reference
  auto it = context.find(expr);
  if (it != context.end()) {
    return it->second;
  }
  return expr;
}

// ============================================================================
// TemporalPathQuery Implementation
// ============================================================================

std::vector<std::vector<Node *>>
TemporalPathQuery::findPaths(Node *start, Node *end, ProgramGraph &pdg) const {
  std::vector<std::vector<Node *>> result;

  struct TimedPath {
    std::vector<Node *> nodes;
    std::vector<Edge *> edges;
    double totalCost;
    std::vector<double> timestamps;
  };

  std::queue<TimedPath> worklist;
  worklist.push({{start}, {}, 0.0, {0.0}});

  while (!worklist.empty()) {
    TimedPath current = worklist.front();
    worklist.pop();

    Node *last = current.nodes.back();
    if (last == end) {
      if (satisfiesTemporalConstraints(current.timestamps, current.totalCost)) {
        result.push_back(current.nodes);
      }
      continue;
    }

    for (auto *edge : last->getOutEdgeSet()) {
      if (!edge)
        continue;

      Node *next = edge->getDstNode();
      if (!next)
        continue;

      TimedPath newPath = current;
      newPath.nodes.push_back(next);
      newPath.edges.push_back(edge);

      // Compute edge cost (simplified)
      double edgeCost = 1.0;
      newPath.totalCost += edgeCost;
      newPath.timestamps.push_back(current.timestamps.back() + edgeCost);

      if (newPath.totalCost <= maxCost_) {
        worklist.push(newPath);
      }
    }
  }

  return result;
}

bool TemporalPathQuery::satisfiesTemporalConstraints(
    const std::vector<double> &timestamps, double totalCost) const {

  if (totalCost > maxCost_)
    return false;

  if (timestamps.size() < 2)
    return true;

  // Check temporal ordering
  for (size_t i = 1; i < timestamps.size(); ++i) {
    if (timestamps[i] < timestamps[i - 1])
      return false;
  }

  return true;
}

} // namespace pdg
