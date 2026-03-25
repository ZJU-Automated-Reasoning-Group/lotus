#ifndef NPA_INTERPROC_CONSTANT_PROPAGATION_H
#define NPA_INTERPROC_CONSTANT_PROPAGATION_H

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

enum class ConstantPropagationTag {
  Top,
  Const,
};

struct ConstantPropagationValue {
  ConstantPropagationTag tag = ConstantPropagationTag::Top;
  llvm::APInt constant = llvm::APInt(1, 0);

  bool isConstant() const { return tag == ConstantPropagationTag::Const; }

  bool operator==(const ConstantPropagationValue &other) const {
    return tag == other.tag &&
           (!isConstant() ||
            (constant.getBitWidth() == other.constant.getBitWidth() &&
             constant.eq(other.constant)));
  }
};

struct ConstantPropagationState {
  bool reachable = false;
  std::unordered_map<const llvm::Value *, ConstantPropagationValue> values;

  bool operator==(const ConstantPropagationState &other) const {
    return reachable == other.reachable && values == other.values;
  }
};

struct ConstantPropagationOp {
  enum class Kind {
    AssignConst,
    Copy,
    Cast,
    Binary,
    Compare,
    AssumeNotCases,
    Phi,
    Select,
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
  llvm::APInt constant = llvm::APInt(1, 0);
  std::vector<const llvm::Value *> inputs;

  bool operator<(const ConstantPropagationOp &other) const;
  bool operator==(const ConstantPropagationOp &other) const;
};

using ConstantPropagationDomain =
    SummaryTransformerDomain<ConstantPropagationOp>;

class InterproceduralConstantPropagation {
public:
  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, ConstantPropagationDomain::value_type> summaries;
    std::map<BlockKey, ConstantPropagationState> blockFacts;
  };

  static Result run(llvm::Module &M, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible);
};

} // namespace npa

#endif // NPA_INTERPROC_CONSTANT_PROPAGATION_H
