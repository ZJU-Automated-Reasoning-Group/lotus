#ifndef DATAFLOW_APA_ENGINES_ADTSIMPLESOLVER_H_
#define DATAFLOW_APA_ENGINES_ADTSIMPLESOLVER_H_

#include "Dataflow/APA/Engines/SolverContext.h"

namespace elimination {
namespace detail {

// Paper-style ADT "simple" engine.
//
// This variant eagerly pushes path-expression prefixes down to all leaves in an
// interval. It is straightforward to understand, but it updates every leaf in a
// subtree whenever an internal ADT node is processed.
template <typename AnalysisDomainTy, typename ReducibleViewT>
bool computeADTSimplePathExpr(
    IntraEliminationSolverContext<AnalysisDomainTy> &Ctx,
    const ReducibleViewT &R,
    typename IntraEliminationSolverContext<AnalysisDomainTy>::ADTNode *W,
    const std::unordered_map<
        typename IntraEliminationSolverContext<AnalysisDomainTy>::n_t,
        typename IntraEliminationSolverContext<AnalysisDomainTy>::ADTNode *>
        &LeafOf,
    const std::vector<
        typename IntraEliminationSolverContext<AnalysisDomainTy>::ADTNode *>
        &LeafByPos) {
  if (!W) {
    return false;
  }
  if (W->Leaf) {
    W->SimpleExpr = Ctx.Exprs.one();
    if (Ctx.hasSelfLoop(R, W->FlowNode)) {
      W->SimpleExpr = Ctx.Exprs.star(
          Ctx.Exprs.atom(R.edgeTransfer(W->FlowNode, W->FlowNode)));
    }
    return true;
  }

  assert(W->Left && W->Right);
  if (!computeADTSimplePathExpr(Ctx, R, W->Left, LeafOf, LeafByPos)) {
    return false;
  }
  if (!computeADTSimplePathExpr(Ctx, R, W->Right, LeafOf, LeafByPos)) {
    return false;
  }

  const auto R1 = W->Left->Entry;
  const auto R2 = W->Right->Entry;

  // X collects cross edges that enter the right interval, while Y collects
  // back edges that re-enter the left interval. These correspond to the paper's
  // interval summary equations for a composition node.
  //
  // F/B classification is expected to be normalized so that:
  //   - each F edge has destination R2 (right-entry)
  //   - each B edge has destination R1 (left-entry)
  // Any mismatch indicates malformed ADT edge classification and we reject
  // this engine invocation.
  auto X = Ctx.Exprs.zero();
  for (const auto &E : W->F) {
    auto It = LeafOf.find(E.Src);
    if (It == LeafOf.end()) {
      return false;
    }
    if (E.Dst != R2) {
      return false;
    }
    auto Edge = Ctx.Exprs.atom(R.edgeTransfer(E.Src, E.Dst));
    X = Ctx.Exprs.unite(X, Ctx.Exprs.concat(It->second->SimpleExpr, Edge));
  }

  auto Y = Ctx.Exprs.zero();
  for (const auto &E : W->B) {
    auto It = LeafOf.find(E.Src);
    if (It == LeafOf.end()) {
      return false;
    }
    if (E.Dst != R1) {
      return false;
    }
    auto Edge = Ctx.Exprs.atom(R.edgeTransfer(E.Src, E.Dst));
    Y = Ctx.Exprs.unite(Y, Ctx.Exprs.concat(It->second->SimpleExpr, Edge));
  }

  const auto L = Ctx.Exprs.star(Ctx.Exprs.concat(X, Y));
  const auto RPref = Ctx.Exprs.concat(L, X);

  // Eager propagation: update every leaf in the left/right interval with the
  // prefix induced by eliminating this composition node.
  for (int Pos = W->Left->MinPos; Pos <= W->Left->MaxPos; ++Pos) {
    auto *Leaf = LeafByPos[Pos];
    Leaf->SimpleExpr = Ctx.Exprs.concat(L, Leaf->SimpleExpr);
  }
  for (int Pos = W->Right->MinPos; Pos <= W->Right->MaxPos; ++Pos) {
    auto *Leaf = LeafByPos[Pos];
    Leaf->SimpleExpr = Ctx.Exprs.concat(RPref, Leaf->SimpleExpr);
  }
  return true;
}

template <typename AnalysisDomainTy, typename ReducibleViewT>
bool solveADTSimpleWith(IntraEliminationSolverContext<AnalysisDomainTy> &Ctx,
                        const ReducibleViewT &R) {
  using Context = IntraEliminationSolverContext<AnalysisDomainTy>;
  using n_t = typename Context::n_t;
  using ADTNode = typename Context::ADTNode;

  ADTNode *Root = nullptr;
  std::unordered_map<n_t, ADTNode *> LeafOf;
  std::unordered_map<n_t, int> TopoPos;
  std::vector<ADTNode *> LeafByPos;
  typename Context::LCATable Lca;
  if (!Ctx.prepareADT(R, Root, LeafOf, TopoPos, LeafByPos, Lca)) {
    return false;
  }

  // The simple engine materializes each leaf expression directly, so once the
  // ADT walk finishes there is no deferred reconstruction step.
  if (!computeADTSimplePathExpr(Ctx, R, Root, LeafOf, LeafByPos)) {
    return false;
  }

  Ctx.Results = typename Context::result_t{};
  const auto Init = Ctx.Problem.initialFact();
  for (const auto &N : Ctx.Problem.nodes()) {
    auto It = LeafOf.find(N);
    if (It == LeafOf.end()) {
      continue;
    }
    auto *Leaf = It->second;
    Ctx.Results.ExprTo(N) = Leaf->SimpleExpr;
    Ctx.Results.IN(N) = Ctx.eval(Leaf->SimpleExpr, Init);
  }
  return true;
}

template <typename AnalysisDomainTy>
bool solveADTSimple(IntraEliminationSolverContext<AnalysisDomainTy> &Ctx) {
  using Context = IntraEliminationSolverContext<AnalysisDomainTy>;
  // Prefer metadata supplied by the client; otherwise derive a reducible view
  // from the plain CFG and reject the engine if reducibility checks fail.
  if (const auto *R =
          dynamic_cast<const typename Context::ReducibleProblemTy *>(
              &Ctx.Problem)) {
    typename Context::ReducibleViewProvided View(*R);
    if (View.init()) {
      return solveADTSimpleWith(Ctx, View);
    }
  }

  typename Context::ComputedReducibleView View;
  if (!Ctx.buildComputedReducibleView(View)) {
    return false;
  }
  return solveADTSimpleWith(Ctx, View);
}

} // namespace detail
} // namespace elimination

#endif // DATAFLOW_APA_ENGINES_ADTSIMPLESOLVER_H_
