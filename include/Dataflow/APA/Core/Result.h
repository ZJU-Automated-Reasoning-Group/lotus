#ifndef DATAFLOW_APA_CORE_RESULT_H_
#define DATAFLOW_APA_CORE_RESULT_H_

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Core/PathExpr.h"

#include <unordered_map>

namespace elimination {

template <typename NodeT, typename FactT, typename TransferT>
class DataFlowResultT {
public:
  using node_t = NodeT;
  using fact_t = FactT;
  using transfer_t = TransferT;
  using expr_factory_t = PathExprFactory<TransferT>;
  using expr_ref_t = typename expr_factory_t::Ref;

  // Builder-style accessor: creates a fact entry on demand.
  FactT &IN(const NodeT &N) { return In[N]; }
  // Builder-style accessor for path-expression summaries.
  expr_ref_t &ExprTo(const NodeT &N) { return Expr[N]; }

  bool containsNode(const NodeT &N) const { return In.find(N) != In.end(); }

  // Read-only lookup that preserves the distinction between
  // "node has no recorded fact" and "node maps to a default-like fact".
  const FactT *tryIN(const NodeT &N) const {
    auto It = In.find(N);
    if (It != In.end()) {
      return &It->second;
    }
    return nullptr;
  }

  expr_ref_t ExprTo(const NodeT &N) const {
    auto It = Expr.find(N);
    if (It != Expr.end()) {
      return It->second;
    }
    return {};
  }

  void setSolveMetadata(SolveStatus S, const SolveDiagnostics &D) {
    HasSolveMetadata = true;
    Status = S;
    Diagnostics = D;
  }

  bool hasSolveMetadata() const { return HasSolveMetadata; }
  SolveStatus solveStatus() const { return Status; }
  const SolveDiagnostics &solveDiagnostics() const { return Diagnostics; }

private:
  std::unordered_map<NodeT, FactT> In;
  std::unordered_map<NodeT, expr_ref_t> Expr;
  bool HasSolveMetadata = false;
  SolveStatus Status = SolveStatus::Ok;
  SolveDiagnostics Diagnostics;
};

} // namespace elimination

#endif // DATAFLOW_APA_CORE_RESULT_H_
