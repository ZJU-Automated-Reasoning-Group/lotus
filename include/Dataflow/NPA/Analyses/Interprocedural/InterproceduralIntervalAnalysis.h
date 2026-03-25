#ifndef NPA_INTERPROC_INTERVAL_ANALYSIS_H
#define NPA_INTERPROC_INTERVAL_ANALYSIS_H

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"
#include "Dataflow/NPA/Domains/SummaryTransformerDomain.h"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/APInt.h>

namespace llvm {
class Value;
class Module;
} // namespace llvm

namespace npa {

enum class IntervalOrdering {
  Signed,
  Unsigned,
};

struct Interval {
  bool bottom = false;
  bool hasLower = false;
  bool hasUpper = false;
  IntervalOrdering ordering = IntervalOrdering::Signed;
  llvm::APInt lower = llvm::APInt(1, 0);
  llvm::APInt upper = llvm::APInt(1, 0);

  static Interval top(unsigned bitWidth = 1,
                      IntervalOrdering ordering = IntervalOrdering::Signed) {
    Interval out;
    out.ordering = ordering;
    out.lower = llvm::APInt(bitWidth, 0);
    out.upper = llvm::APInt(bitWidth, 0);
    return out;
  }

  static Interval point(const llvm::APInt &value,
                        IntervalOrdering ordering = IntervalOrdering::Signed) {
    Interval out;
    out.hasLower = true;
    out.hasUpper = true;
    out.ordering = ordering;
    out.lower = value;
    out.upper = value;
    return out;
  }

  bool isExact() const {
    return hasLower && hasUpper && lower.getBitWidth() == upper.getBitWidth() &&
           lower.eq(upper);
  }

  bool operator==(const Interval &other) const {
    return bottom == other.bottom && hasLower == other.hasLower &&
           hasUpper == other.hasUpper && ordering == other.ordering &&
           (!hasLower || (lower.getBitWidth() == other.lower.getBitWidth() &&
                          lower.eq(other.lower))) &&
           (!hasUpper || (upper.getBitWidth() == other.upper.getBitWidth() &&
                          upper.eq(other.upper)));
  }
};

struct IntervalState {
  bool reachable = false;
  std::unordered_map<const llvm::Value *, Interval> values;

  bool operator==(const IntervalState &other) const {
    return reachable == other.reachable && values == other.values;
  }
};

struct IntervalOp {
  enum class Kind {
    AssignConst,
    Copy,
    Cast,
    Binary,
    Compare,
    AssumeNotCases,
    Select,
    Phi,
    Forget,
  };

  Kind kind = Kind::Forget;
  const llvm::Value *dest = nullptr;
  const llvm::Value *lhs = nullptr;
  const llvm::Value *rhs = nullptr;
  const llvm::Value *cond = nullptr;
  unsigned opcode = 0;
  unsigned bitWidth = 0;
  unsigned sourceBitWidth = 0;
  IntervalOrdering ordering = IntervalOrdering::Signed;
  llvm::APInt constant = llvm::APInt(1, 0);
  std::vector<const llvm::Value *> inputs;

  bool operator<(const IntervalOp &other) const;
  bool operator==(const IntervalOp &other) const;
};

using IntervalDomain = SummaryTransformerDomain<IntervalOp>;

class InterproceduralIntervalAnalysis {
public:
  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, IntervalDomain::value_type> summaries;
    std::map<BlockKey, IntervalState> blockFacts;
  };

  static Result run(llvm::Module &M, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible);
};

} // namespace npa

#endif // NPA_INTERPROC_INTERVAL_ANALYSIS_H
