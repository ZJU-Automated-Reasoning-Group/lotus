#ifndef DATAFLOW_APA_CORE_INTERRESULT_H_
#define DATAFLOW_APA_CORE_INTERRESULT_H_

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/Mono/Solver/CallStringSolver.h"

#include <vector>

namespace elimination {

struct InterSummarySolveDiagnostics final {
  std::size_t discovered_context_node_count = 0;
  std::size_t seed_count = 0;
  std::size_t equation_node_count = 0;
  std::size_t equation_edge_count = 0;
  std::size_t scc_count = 0;
  std::size_t cyclic_scc_count = 0;
};

template <unsigned K, typename FactT, typename TransferT,
          typename NodeT = llvm::Instruction *>
class InterDataFlowResultT
    : public dataflow::ContextSensitiveDataFlowResult<K, FactT, NodeT> {
public:
  using Base = dataflow::ContextSensitiveDataFlowResult<K, FactT, NodeT>;
  using fact_t = FactT;
  using transfer_t = TransferT;
  using n_t = NodeT;
  using Context = typename Base::Context;
  using ContextKey = typename Base::ContextKey;

  InterDataFlowResultT() = default;
  explicit InterDataFlowResultT(const Base &Other) : Base(Other) {}

  const fact_t *tryIN(const ContextKey &Key) const {
    auto It = this->getINMap().find(Key);
    if (It == this->getINMap().end()) {
      return nullptr;
    }
    return &It->second;
  }

  const fact_t *tryOUT(const ContextKey &Key) const {
    auto It = this->getOUTMap().find(Key);
    if (It == this->getOUTMap().end()) {
      return nullptr;
    }
    return &It->second;
  }

  const fact_t *tryIN(n_t Inst, const Context &Ctx) const {
    return tryIN(ContextKey{Inst, Ctx});
  }

  const fact_t *tryOUT(n_t Inst, const Context &Ctx) const {
    return tryOUT(ContextKey{Inst, Ctx});
  }

  bool containsInstruction(n_t Inst) const {
    for (const auto &Entry : this->getINMap()) {
      if (Entry.first.Inst == Inst) {
        return true;
      }
    }
    return false;
  }

  std::vector<ContextKey> contextsForInstruction(n_t Inst) const {
    std::vector<ContextKey> Keys;
    for (const auto &Entry : this->getINMap()) {
      if (Entry.first.Inst == Inst) {
        Keys.push_back(Entry.first);
      }
    }
    return Keys;
  }

  void setSolveStatus(SolveStatus S) {
    HasSolveMetadata = true;
    Status = S;
  }

  bool hasSolveMetadata() const { return HasSolveMetadata; }
  SolveStatus solveStatus() const { return Status; }

  void setSummarySolveDiagnostics(const InterSummarySolveDiagnostics &D) {
    HasSummaryDiagnostics = true;
    SummaryDiagnostics = D;
  }

  bool hasSummarySolveDiagnostics() const { return HasSummaryDiagnostics; }
  const InterSummarySolveDiagnostics &summarySolveDiagnostics() const {
    return SummaryDiagnostics;
  }

private:
  bool HasSolveMetadata = false;
  SolveStatus Status = SolveStatus::Ok;
  bool HasSummaryDiagnostics = false;
  InterSummarySolveDiagnostics SummaryDiagnostics;
};

} // namespace elimination

#endif // DATAFLOW_APA_CORE_INTERRESULT_H_
