#ifndef DATAFLOW_APA_ENGINES_ADTDELAYEDSOLVER_H_
#define DATAFLOW_APA_ENGINES_ADTDELAYEDSOLVER_H_

#include "Dataflow/APA/Engines/SolverContext.h"

namespace elimination {
namespace detail {

// Paper-style ADT "delayed" engine.
//
// Instead of eagerly rewriting every leaf expression, this variant stores
// parent-to-child prefixes in a union-find-like structure and reconstructs the
// final expression lazily for each leaf at the end.
template <typename AnalysisDomainTy, typename ReducibleViewT>
bool computeADTDelayedPathExpr(
    IntraEliminationSolverContext<AnalysisDomainTy> &Ctx,
    const ReducibleViewT &R,
    typename IntraEliminationSolverContext<AnalysisDomainTy>::ADTNode *W,
    const std::unordered_map<
        typename IntraEliminationSolverContext<AnalysisDomainTy>::n_t,
        typename IntraEliminationSolverContext<AnalysisDomainTy>::ADTNode *>
        &LeafOf) {
  if (!W) {
    return false;
  }
  if (W->Leaf) {
    W->UFExpr = Ctx.Exprs.one();
    if (!W->Parent && Ctx.hasSelfLoop(R, W->FlowNode)) {
      W->UFExpr = Ctx.Exprs.star(
          Ctx.Exprs.atom(R.edgeTransfer(W->FlowNode, W->FlowNode)));
    }
    return true;
  }

  assert(W->Left && W->Right);
  if (!computeADTDelayedPathExpr(Ctx, R, W->Left, LeafOf)) {
    return false;
  }
  if (!computeADTDelayedPathExpr(Ctx, R, W->Right, LeafOf)) {
    return false;
  }

  const auto R1 = W->Left->Entry;
  const auto R2 = W->Right->Entry;

  // As in the simple engine, X summarizes forward crossings into the right
  // interval and Y summarizes back crossings into the left interval. The
  // difference is that we keep the resulting prefixes on ADT edges rather than
  // pushing them immediately to every descendant leaf.
  //
  // F/B sets are expected to be normalized so each cross edge targets the
  // corresponding interval entry (R2 for F, R1 for B). If not, we treat the
  // reducible view as inconsistent and reject this ADT attempt.
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
    X = Ctx.Exprs.unite(X, Ctx.Exprs.concat(Ctx.evalUF(It->second), Edge));
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
    Y = Ctx.Exprs.unite(Y, Ctx.Exprs.concat(Ctx.evalUF(It->second), Edge));
  }

  auto L = Ctx.Exprs.star(Ctx.Exprs.concat(X, Y));
  auto RPref = Ctx.Exprs.concat(L, X);

  // Leaf self-loops must be folded into the prefix before linking because the
  // delayed representation stores only interval-entry to child-entry summaries.
  if (W->Left->Leaf) {
    const auto U = W->Left->FlowNode;
    if (Ctx.hasSelfLoop(R, U)) {
      L = Ctx.Exprs.concat(
          L, Ctx.Exprs.star(Ctx.Exprs.atom(R.edgeTransfer(U, U))));
    }
  }
  if (W->Right->Leaf) {
    const auto U = W->Right->FlowNode;
    if (Ctx.hasSelfLoop(R, U)) {
      RPref = Ctx.Exprs.concat(
          RPref, Ctx.Exprs.star(Ctx.Exprs.atom(R.edgeTransfer(U, U))));
    }
  }

  // Record the prefixes structurally. evalUF() later composes these links with
  // path compression when a concrete leaf result is requested.
  Ctx.linkUpdate(W, W->Left, L);
  Ctx.linkUpdate(W, W->Right, RPref);
  return true;
}

template <typename AnalysisDomainTy, typename ReducibleViewT>
bool solveADTDelayedWith(IntraEliminationSolverContext<AnalysisDomainTy> &Ctx,
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

  // The root starts as its own representative with the empty-prefix identity.
  Ctx.initUF(Root);
  Root->UFParent = Root;
  Root->UFExpr = Ctx.Exprs.one();

  if (!computeADTDelayedPathExpr(Ctx, R, Root, LeafOf)) {
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
    auto E = Ctx.evalUF(Leaf);
    Ctx.Results.ExprTo(N) = E;
    Ctx.Results.IN(N) = Ctx.eval(E, Init);
  }
  return true;
}

template <typename AnalysisDomainTy>
bool solveADTDelayed(IntraEliminationSolverContext<AnalysisDomainTy> &Ctx) {
  using Context = IntraEliminationSolverContext<AnalysisDomainTy>;
  // Prefer metadata supplied by the client; otherwise derive a reducible view
  // from the plain CFG and reject the engine if reducibility checks fail.
  if (const auto *R =
          dynamic_cast<const typename Context::ReducibleProblemTy *>(
              &Ctx.Problem)) {
    typename Context::ReducibleViewProvided View(*R);
    if (View.init()) {
      return solveADTDelayedWith(Ctx, View);
    }
  }

  typename Context::ComputedReducibleView View;
  if (!Ctx.buildComputedReducibleView(View)) {
    return false;
  }
  return solveADTDelayedWith(Ctx, View);
}

} // namespace detail
} // namespace elimination

#endif // DATAFLOW_APA_ENGINES_ADTDELAYEDSOLVER_H_
