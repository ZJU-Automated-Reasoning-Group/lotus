#ifndef DATAFLOW_APA_ANALYSES_INTER_AFFINEEQUALITIES_H_
#define DATAFLOW_APA_ANALYSES_INTER_AFFINEEQUALITIES_H_

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Domains/AffineRelationDomain.h"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace llvm {
class BasicBlock;
class Function;
class Value;
class Module;
} // namespace llvm

namespace elimination {

struct FunctionKey {
  const llvm::Function *function = nullptr;

  bool operator<(const FunctionKey &other) const {
    return function < other.function;
  }
};

struct BlockKey {
  const llvm::BasicBlock *block = nullptr;

  bool operator<(const BlockKey &other) const { return block < other.block; }
};

struct AffineExpr {
  bool top = true;
  int64_t constant = 0;
  std::unordered_map<const llvm::Value *, int64_t> terms;

  bool operator==(const AffineExpr &other) const {
    return top == other.top && constant == other.constant &&
           terms == other.terms;
  }
};

struct AffineEquality {
  unsigned bitWidth = 0;
  int64_t constant = 0;
  std::unordered_map<const llvm::Value *, int64_t> terms;

  bool operator==(const AffineEquality &other) const {
    return bitWidth == other.bitWidth && constant == other.constant &&
           terms == other.terms;
  }
};

struct AffineState {
  bool reachable = false;
  std::unordered_map<const llvm::Value *, AffineExpr> values;
  std::vector<AffineEquality> equalities;

  bool operator==(const AffineState &other) const {
    return reachable == other.reachable && values == other.values &&
           equalities == other.equalities;
  }
};

class InterAffineEqualities {
public:
  struct Result {
    SolveStatus status = SolveStatus::Ok;
    std::map<FunctionKey, AffineRelationDomain::value_type> summaries;
    std::map<BlockKey, AffineRelationDomain::value_type> blockRelations;
  };

  static Result run(llvm::Module &M, bool verbose = false);
};

AffineState
materializeAffineExpressions(const AffineRelationDomain::value_type &relation);

} // namespace elimination

#endif // DATAFLOW_APA_ANALYSES_INTER_AFFINEEQUALITIES_H_
