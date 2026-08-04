#ifndef DATAFLOW_APA_EAN_IMPORT_H_
#define DATAFLOW_APA_EAN_IMPORT_H_

// Import: PathExprFactory<TransferT>::Ref DAG  ->  canonical e-graph.
//
// This realizes invariants I1–I3 of the paper's import step (docs, §III.A):
//   I2 (profile-relative canonical form): variadic joins are flattened, have 0
//       removed, are deduplicated and sorted; variadic sequences are flattened
//       and have 1 removed but are NEVER reordered.
//   I3 (root preservation): every input root maps to an e-class id, returned in
//       order, so the original expression is always recoverable.
//
// Flattening is done on the *Ref tree* (PathExprFactory's Union/Concat are
// binary, so `(a⊕b)⊕c` is literally `Union(Union(a,b),c)`), which is cleaner
// than post-hoc flattening inside the e-graph and needs no e-graph state.
//
// M1 scope: only the universally-valid Kleene simplifications are applied
// (drop 0/1, annihilation, dedup/idempotence, (A*)*=A*, 0*=1*=1). No law
// profile and no exploratory rewrites yet.

#include <cstdint>
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/EAN/AtomTable.h"
#include "Dataflow/APA/EAN/PathLang.h"
#include "Solvers/EGraph/Analysis.h"
#include "Solvers/EGraph/EGraph.h"

namespace elimination {
namespace ean {

// The concrete e-graph type EAN builds. M1 needs no e-class analysis data.
using Graph = ::lotus::egraph::EGraph<PathLang, ::lotus::egraph::NoAnalysis<PathLang>>;

template <typename TransferT> struct ImportResult {
  Graph g;
  std::vector<Id> roots;         // one canonical e-class id per input root (I3)
  AtomTable<TransferT> atoms;    // atom id <-> original Ref, for Export
};

namespace detail {

template <typename TransferT> class Importer {
public:
  using Factory = PathExprFactory<TransferT>;
  using Ref = typename Factory::Ref;
  using Kind = typename Factory::Kind;

  Graph g;
  std::vector<Id> roots;
  AtomTable<TransferT> atoms;

  Id importExpr(const Ref &e) {
    auto it = memo_.find(e.get());
    if (it != memo_.end()) {
      return g.find(it->second);
    }
    Id result = importFresh(e);
    memo_.emplace(e.get(), result);
    return result;
  }

private:
  Id zeroId() { return g.add(makeZero()); }
  Id oneId() { return g.add(makeOne()); }

  const PathLang &firstNode(Id c) { return g[c].nodes.front(); }

  Id importFresh(const Ref &e) {
    switch (e->K) {
    case Kind::Zero:
      return zeroId();
    case Kind::One:
      return oneId();
    case Kind::Atom:
      return g.add(makeAtom(atoms.intern(e)));
    case Kind::Union: {
      std::vector<Id> members;
      collectJoin(e, members);
      return buildJoin(std::move(members));
    }
    case Kind::Concat: {
      std::vector<Id> members;
      collectSeq(e, members);
      return buildSeq(std::move(members));
    }
    case Kind::Star:
      return buildStar(importExpr(e->L));
    }
    return zeroId(); // unreachable; silences -Wreturn-type
  }

  // Flatten nested Union at the Ref level; leaves (non-Union) get imported.
  void collectJoin(const Ref &e, std::vector<Id> &out) {
    if (e->K == Kind::Union) {
      collectJoin(e->L, out);
      collectJoin(e->R, out);
    } else {
      out.push_back(importExpr(e));
    }
  }

  void collectSeq(const Ref &e, std::vector<Id> &out) {
    if (e->K == Kind::Concat) {
      collectSeq(e->L, out);
      collectSeq(e->R, out);
    } else {
      out.push_back(importExpr(e));
    }
  }

  // JOIN-ACI: canonicalize to reps, drop 0, sort, dedup. Members are never
  // themselves join nodes (join e-nodes are only minted here, and collectJoin
  // only imports non-Union Refs), so no id-level join flattening is needed.
  Id buildJoin(std::vector<Id> members) {
    const Id zero = zeroId();
    std::vector<Id> kept;
    kept.reserve(members.size());
    for (Id m : members) {
      m = g.find(m);
      if (m != zero) {
        kept.push_back(m);
      }
    }
    std::sort(kept.begin(), kept.end());
    kept.erase(std::unique(kept.begin(), kept.end()), kept.end());
    if (kept.empty()) {
      return zero;
    }
    if (kept.size() == 1) {
      return kept.front();
    }
    return g.add(makeJoin(std::move(kept)));
  }

  // SEQ: canonicalize, drop 1, annihilate on 0, keep order, no dedup.
  Id buildSeq(std::vector<Id> members) {
    const Id zero = zeroId();
    const Id one = oneId();
    std::vector<Id> kept;
    kept.reserve(members.size());
    for (Id m : members) {
      m = g.find(m);
      if (m == zero) {
        return zero; // 0 · x = x · 0 = 0
      }
      if (m == one) {
        continue; // 1 · x = x · 1 = x
      }
      kept.push_back(m);
    }
    if (kept.empty()) {
      return one;
    }
    if (kept.size() == 1) {
      return kept.front();
    }
    return g.add(makeSeq(std::move(kept)));
  }

  Id buildStar(Id sub) {
    sub = g.find(sub);
    const PathLang &n = firstNode(sub);
    if (isStar(n)) {
      return sub; // (A*)* = A*
    }
    if (isZero(n) || isOne(n)) {
      return oneId(); // 0* = 1* = 1
    }
    return g.add(makeStar(sub));
  }

  std::unordered_map<const typename Factory::Expr *, Id> memo_;
};

} // namespace detail

// Import a batch of path-expression roots into a canonical e-graph.
template <typename TransferT>
ImportResult<TransferT>
importCanonical(const std::vector<typename PathExprFactory<TransferT>::Ref> &R) {
  detail::Importer<TransferT> imp;
  imp.roots.reserve(R.size());
  for (const auto &r : R) {
    imp.roots.push_back(imp.importExpr(r));
  }
  imp.g.rebuild();
  for (auto &id : imp.roots) {
    id = imp.g.find(id);
  }
  return ImportResult<TransferT>{std::move(imp.g), std::move(imp.roots),
                                 std::move(imp.atoms)};
}

} // namespace ean
} // namespace elimination

#endif // DATAFLOW_APA_EAN_IMPORT_H_
