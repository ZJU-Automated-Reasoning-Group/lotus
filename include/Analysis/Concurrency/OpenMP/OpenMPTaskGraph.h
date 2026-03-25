/**
 * @file OpenMPTaskGraph.h
 * @brief OpenMP Task Dependency Graph
 *
 * This file provides infrastructure for tracking OpenMP task dependencies
 * via the depend clause.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#pragma once

#include "Analysis/Concurrency/ConcurrencyRelation.h"
#include "Analysis/Concurrency/OpenMP/OpenMPSemantics.h"

#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OpenMP {

/**
 * @class OpenMPTaskGraph
 * @brief Builds and analyzes OpenMP task dependency graph
 *
 * Analyzes OpenMP task constructs with depend clauses to build a
 * task dependency graph for precise happens-before analysis.
 */
class OpenMPTaskGraph {
public:
  using AnalysisSummary = OpenMPSemantics::AnalysisSummary;

  enum class TaskRelation { HappensBefore, Excluded, Parallel, Unknown };

  explicit OpenMPTaskGraph(llvm::Module &module);

  /**
   * @brief Analyze the module to build task dependency graph
   */
  void analyze();

  /**
   * @brief Get all tasks in the program
   */
  const std::vector<std::unique_ptr<Task>> &getAllTasks() const {
    return m_semantics.getTasks();
  }

  const Task *getTaskForCreate(const llvm::Instruction *inst) const;

  const std::vector<WaitBoundaryInfo> &getWaitBoundaries() const {
    return m_semantics.getWaitBoundaryInfos();
  }

  const AnalysisSummary &getSummary() const { return m_summary; }
  const OpenMPSemantics &getSemantics() const { return m_semantics; }

  size_t getDeferredWaitDepsCount() const { return m_deferred_wait_deps_count; }
  size_t getDeferredImpreciseConflictCount() const {
    return m_deferred_imprecise_conflict_count;
  }
  const std::unordered_map<std::string, size_t> &
  getUnknownReasonCounts() const {
    return m_deferred_reason_counts;
  }
  const std::unordered_map<std::string, size_t> &
  getDeferredReasonCounts() const {
    return m_deferred_reason_counts;
  }
  size_t getRelationCount(concurrency::RelationKind kind) const;
  const std::map<std::pair<const Task *, const Task *>, concurrency::Relation> &
  getRelations() const {
    return m_relations;
  }

  /**
   * @brief Check if two tasks have a happens-before relationship
   */
  bool happensBefore(const Task *t1, const Task *t2) const;

  /**
   * @brief Classify the relation between two tasks.
   *
   * `Unknown` means the analysis found evidence that the tasks are not
   * provably independent, but could not justify a definite happens-before
   * edge either.
   */
  TaskRelation classifyTaskRelation(const Task *t1, const Task *t2) const;

  DependencyConflict classifyDependencyConflict(const Dependency &d1,
                                                const Dependency &d2) const;

  /**
   * @brief Check if two tasks may execute in parallel
   */
  bool mayBeParallel(const Task *t1, const Task *t2) const;

  /**
   * @brief Get the maximum nested parallelism depth encountered
   */
  size_t getMaxNestedDepth() const { return m_semantics.getMaxNestedDepth(); }

  /**
   * @brief Check if a region is nested (true) or flat (false)
   */
  bool isNestedRegion(size_t region_id) const;

  /**
   * @brief Get nesting depth for a specific region
   */
  size_t getRegionNestingDepth(size_t region_id) const;

private:
  llvm::Module &m_module;
  OpenMPSemantics m_semantics;
  std::map<std::pair<const Task *, const Task *>, concurrency::Relation>
      m_relations;
  size_t m_deferred_wait_deps_count = 0;
  mutable size_t m_deferred_imprecise_conflict_count = 0;
  mutable std::unordered_map<std::string, size_t> m_deferred_reason_counts;
  AnalysisSummary m_summary;

  /**
   * @brief Build dependency edges between tasks
   */
  void buildDependencyEdges();

  /**
   * @brief Check if two dependencies conflict
   */
  bool dependenciesConflict(const Dependency &d1, const Dependency &d2) const;

  bool isMutexLikeExclusion(const Dependency &d1, const Dependency &d2) const;
};

} // namespace OpenMP
