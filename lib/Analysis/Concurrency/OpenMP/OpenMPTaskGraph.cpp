/**
 * @file OpenMPTaskGraph.cpp
 * @brief Implementation of OpenMP Task Dependency Graph
 */

#include "Analysis/Concurrency/OpenMP/OpenMPTaskGraph.h"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace OpenMP;

namespace {

const Value *stripValue(const Value *value) {
  return value ? value->stripPointerCasts() : nullptr;
}

const Value *canonicalizeDependencyAddress(const Value *value,
                                           const DataLayout &DL,
                                           int64_t &offset,
                                           bool &has_precise_offset) {
  offset = 0;
  has_precise_offset = false;
  if (!value) {
    return nullptr;
  }

  value = stripValue(value);
  if (const auto *load = dyn_cast<LoadInst>(value)) {
    value = load->getPointerOperand()->stripPointerCasts();
  }

  if (const Value *base_with_offset =
          GetPointerBaseWithConstantOffset(value, offset, DL)) {
    const Value *canonical_base = stripValue(base_with_offset);
    if (!(isa<GEPOperator>(value) && canonical_base == stripValue(value))) {
      has_precise_offset = true;
      return canonical_base;
    }
  }

  if (const Value *base = getUnderlyingObject(value)) {
    return stripValue(base);
  }
  return value;
}

std::pair<const Task *, const Task *> normalizeTaskPair(const Task *lhs,
                                                        const Task *rhs) {
  return lhs < rhs ? std::make_pair(lhs, rhs) : std::make_pair(rhs, lhs);
}

} // namespace

OpenMPTaskGraph::OpenMPTaskGraph(Module &module)
    : m_module(module), m_semantics(module) {}

void OpenMPTaskGraph::analyze() {
  errs() << "Starting OpenMP Task Dependency Analysis...\n";
  m_semantics.analyze();
  m_summary = m_semantics.getSummary();
  buildDependencyEdges();

  errs() << "Found " << m_semantics.getTasks().size()
         << " OpenMP tasks with dependencies\n";
  if (m_deferred_wait_deps_count) {
    errs() << "Deferred " << m_deferred_wait_deps_count
           << " OpenMP wait_deps boundaries (partial synchronization)\n";
  }
  if (m_deferred_imprecise_conflict_count) {
    errs() << "Deferred " << m_deferred_imprecise_conflict_count
           << " imprecise OpenMP depend conflicts (no definite HB edge)\n";
  }
}

const Task *OpenMPTaskGraph::getTaskForCreate(const Instruction *inst) const {
  return m_semantics.getTaskForCreate(inst);
}

void OpenMPTaskGraph::buildDependencyEdges() {
  m_relations = m_semantics.getRelations();
  m_deferred_reason_counts = m_semantics.getDeferredReasonCounts();
  m_deferred_imprecise_conflict_count =
      m_semantics.getDeferredImpreciseConflictCount();
  m_deferred_wait_deps_count = 0;
  for (const OpenMPTaskEvent &event : m_semantics.getTaskEvents()) {
    if (event.kind == OpenMPTaskEvent::Kind::TaskwaitDeps &&
        event.is_partial_wait && event.dependencies.empty()) {
      ++m_deferred_wait_deps_count;
    }
  }
}

bool OpenMPTaskGraph::dependenciesConflict(const Dependency &d1,
                                           const Dependency &d2) const {
  return classifyDependencyConflict(d1, d2) == DependencyConflict::MustConflict;
}

DependencyConflict
OpenMPTaskGraph::classifyDependencyConflict(const Dependency &d1,
                                            const Dependency &d2) const {
  return m_semantics.classifyDependencyConflict(d1, d2);
}

bool OpenMPTaskGraph::isMutexLikeExclusion(const Dependency &d1,
                                           const Dependency &d2) const {
  return d1.type == DependType::MUTEXINOUTSET ||
         d2.type == DependType::MUTEXINOUTSET;
}

bool OpenMPTaskGraph::happensBefore(const Task *t1, const Task *t2) const {
  if (!t1 || !t2 || t1 == t2) {
    return false;
  }

  std::set<const Task *> visited;
  std::vector<const Task *> worklist;
  worklist.push_back(t1);
  visited.insert(t1);

  while (!worklist.empty()) {
    const Task *current = worklist.back();
    worklist.pop_back();

    if (current == t2) {
      return true;
    }

    for (const Task *succ : current->successors) {
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

size_t OpenMPTaskGraph::getRelationCount(concurrency::RelationKind kind) const {
  size_t count = 0;
  for (const auto &entry : m_relations) {
    if (entry.second.kind == kind) {
      ++count;
    }
  }
  return count;
}

bool OpenMPTaskGraph::isNestedRegion(size_t region_id) const {
  return m_semantics.isNestedRegion(region_id);
}

size_t OpenMPTaskGraph::getRegionNestingDepth(size_t region_id) const {
  return m_semantics.getRegionNestingDepth(region_id);
}

OpenMPTaskGraph::TaskRelation
OpenMPTaskGraph::classifyTaskRelation(const Task *t1, const Task *t2) const {
  if (!t1 || !t2 || t1 == t2) {
    return TaskRelation::Unknown;
  }
  if (happensBefore(t1, t2) || happensBefore(t2, t1)) {
    return TaskRelation::HappensBefore;
  }
  if (t1->exclusions.count(const_cast<Task *>(t2)) ||
      t2->exclusions.count(const_cast<Task *>(t1))) {
    return TaskRelation::Excluded;
  }
  if (m_relations.count(normalizeTaskPair(t1, t2))) {
    return TaskRelation::Unknown;
  }
  return TaskRelation::Parallel;
}

bool OpenMPTaskGraph::mayBeParallel(const Task *t1, const Task *t2) const {
  if (!t1 || !t2 || t1 == t2) {
    return false;
  }
  return classifyTaskRelation(t1, t2) == TaskRelation::Parallel;
}
