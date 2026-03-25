#ifndef DATAFLOW_APA_ENGINES_STATEELIMINATIONSOLVER_H_
#define DATAFLOW_APA_ENGINES_STATEELIMINATIONSOLVER_H_

#include "Dataflow/APA/Engines/SolverContext.h"

namespace elimination {
namespace detail {

// Generic Floyd-Warshall-style elimination over the full CFG. This engine
// makes no reducibility assumptions and therefore serves as the baseline
// implementation as well as the fallback when ADT-specific preconditions fail.
template <typename AnalysisDomainTy>
std::vector<std::size_t> getStateEliminationOrder(
    const IntraEliminationSolverContext<AnalysisDomainTy> &Ctx) {
  using Context = IntraEliminationSolverContext<AnalysisDomainTy>;
  const auto N = Ctx.Nodes.size();
  std::vector<std::size_t> Order(N);
  const auto *R =
      dynamic_cast<const typename Context::ReducibleProblemTy *>(&Ctx.Problem);
  if (R != nullptr) {
    const auto Topo = R->topologicalOrder();
    if (Topo.size() == N) {
      for (std::size_t i = 0; i < N; ++i) {
        const auto It = Ctx.Index.find(Topo[N - 1 - i]);
        if (It == Ctx.Index.end()) {
          goto DefaultOrder;
        }
        Order[i] = It->second;
      }
      return Order;
    }
  }
DefaultOrder:
  for (std::size_t i = 0; i < N; ++i) {
    Order[i] = i;
  }
  return Order;
}

template <typename AnalysisDomainTy>
void buildStateEliminationMatrix(
    IntraEliminationSolverContext<AnalysisDomainTy> &Ctx) {
  // Build the usual elimination matrix where M[i][j] summarizes all direct
  // edges from node i to node j. Diagonals start at one() so paths are allowed
  // to stay at a node before additional eliminations introduce loops.
  Ctx.Nodes = Ctx.Problem.nodes();
  Ctx.Index.clear();
  Ctx.Index.reserve(Ctx.Nodes.size());
  for (std::size_t i = 0; i < Ctx.Nodes.size(); ++i) {
    Ctx.Index.emplace(Ctx.Nodes[i], i);
  }

  const auto N = Ctx.Nodes.size();
  Ctx.Matrix.assign(
      N,
      std::vector<
          typename IntraEliminationSolverContext<AnalysisDomainTy>::expr_ref_t>(
          N, Ctx.Exprs.zero()));
  for (std::size_t i = 0; i < N; ++i) {
    Ctx.Matrix[i][i] = Ctx.Exprs.one();
  }

  for (const auto &Src : Ctx.Nodes) {
    const auto SrcIdx = Ctx.idx(Src);
    for (const auto &Dst : Ctx.Problem.succs(Src)) {
      const auto It = Ctx.Index.find(Dst);
      if (It == Ctx.Index.end()) {
        continue;
      }
      const auto DstIdx = It->second;
      Ctx.Matrix[SrcIdx][DstIdx] =
          Ctx.Exprs.unite(Ctx.Matrix[SrcIdx][DstIdx],
                          Ctx.Exprs.atom(Ctx.Problem.edgeTransfer(Src, Dst)));
    }
  }
}

template <typename AnalysisDomainTy>
void eliminateStateIntermediates(
    IntraEliminationSolverContext<AnalysisDomainTy> &Ctx) {
  using Context = IntraEliminationSolverContext<AnalysisDomainTy>;
  const auto N = Ctx.Nodes.size();
  std::vector<typename Context::expr_ref_t> ColK(N);
  std::vector<typename Context::expr_ref_t> RowK(N);

  const auto Order = getStateEliminationOrder(Ctx);
  for (std::size_t ki = 0; ki < N; ++ki) {
    const std::size_t k = Order[ki];
    // Snapshot row/column k before mutating the matrix. This mirrors the
    // standard state-elimination update:
    //   M[i][j] |= M[i][k] . M[k][k]* . M[k][j]
    for (std::size_t i = 0; i < N; ++i) {
      ColK[i] = Ctx.Matrix[i][k];
    }
    for (std::size_t j = 0; j < N; ++j) {
      RowK[j] = Ctx.Matrix[k][j];
    }

    const auto KStar = Ctx.Exprs.star(Ctx.Matrix[k][k]);
    for (std::size_t i = 0; i < N; ++i) {
      if (Context::expr_factory_t::isZero(ColK[i])) {
        continue;
      }
      for (std::size_t j = 0; j < N; ++j) {
        if (Context::expr_factory_t::isZero(RowK[j])) {
          continue;
        }
        auto Via = Ctx.Exprs.concat(ColK[i], KStar);
        Via = Ctx.Exprs.concat(Via, RowK[j]);
        Ctx.Matrix[i][j] = Ctx.Exprs.unite(Ctx.Matrix[i][j], Via);
      }
    }
  }
}

template <typename AnalysisDomainTy>
bool materializeStateResults(
    IntraEliminationSolverContext<AnalysisDomainTy> &Ctx) {
  using Context = IntraEliminationSolverContext<AnalysisDomainTy>;
  Ctx.Results = typename Context::result_t{};
  if (Ctx.Nodes.empty()) {
    return true;
  }

  const auto EntryIt = Ctx.Index.find(Ctx.Problem.entry());
  if (EntryIt == Ctx.Index.end()) {
    return false;
  }
  const auto EntryIdx = EntryIt->second;

  const auto Init = Ctx.Problem.initialFact();
  for (std::size_t j = 0; j < Ctx.Nodes.size(); ++j) {
    const auto &N = Ctx.Nodes[j];
    // Each remaining matrix entry summarizes all paths from entry to N.
    auto E = Ctx.Matrix[EntryIdx][j];
    Ctx.Results.ExprTo(N) = E;
    Ctx.Results.IN(N) = Ctx.eval(E, Init);
  }
  return true;
}

template <typename AnalysisDomainTy>
bool solveStateElimination(
    IntraEliminationSolverContext<AnalysisDomainTy> &Ctx) {
  buildStateEliminationMatrix(Ctx);
  eliminateStateIntermediates(Ctx);
  return materializeStateResults(Ctx);
}

} // namespace detail
} // namespace elimination

#endif // DATAFLOW_APA_ENGINES_STATEELIMINATIONSOLVER_H_
