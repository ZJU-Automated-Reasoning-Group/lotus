#ifndef ANALYSIS_DATAFLOW_WPDS_DATAFLOWFACTS_H_
#define ANALYSIS_DATAFLOW_WPDS_DATAFLOWFACTS_H_

#include "Utils/LLVM/SystemHeaders.h"

#include <ostream>
#include <set>

namespace wpds {

/**
 * Domain of dataflow facts for WPDS-based analyses.
 *
 * Represents a set of facts (LLVM Values) with optional "universe" semantics.
 * Provides the set operations required by the WPDS framework (union, intersect,
 * diff, equality) as static methods, matching the paper's notion of a fact
 * domain for interprocedural dataflow.
 *
 * @see Reps, Schwoon, Jha: "Weighted Pushdown Systems and their Application
 *      to Interprocedural Dataflow Analysis"
 */
class DataFlowFacts {
public:
  DataFlowFacts();
  DataFlowFacts(const std::set<Value *> &facts);
  DataFlowFacts(const DataFlowFacts &other);
  ~DataFlowFacts() = default;

  DataFlowFacts &operator=(const DataFlowFacts &other);
  bool operator==(const DataFlowFacts &other) const;

  // Required set operations for WPDS
  static DataFlowFacts EmptySet();
  static DataFlowFacts UniverseSet();
  static void ClearUniverse();
  static DataFlowFacts Union(const DataFlowFacts &x, const DataFlowFacts &y);
  static DataFlowFacts Intersect(const DataFlowFacts &x,
                                 const DataFlowFacts &y);
  static DataFlowFacts Diff(const DataFlowFacts &x, const DataFlowFacts &y);
  static bool Eq(const DataFlowFacts &x, const DataFlowFacts &y);

  // Get the underlying set of facts
  const std::set<Value *> &getFacts() const;
  void addFact(Value *val);
  void removeFact(Value *val);
  bool containsFact(Value *val) const;
  std::size_t size() const;
  bool isEmpty() const;

  // Debug printing
  std::ostream &print(std::ostream &os) const;

private:
  bool is_universe = false;
  std::set<Value *> facts;
};

} // namespace wpds

#endif // ANALYSIS_DATAFLOW_WPDS_DATAFLOWFACTS_H_
