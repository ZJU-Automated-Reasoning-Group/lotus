#ifndef DATAFLOW_APA_EAN_EXPORT_H_
#define DATAFLOW_APA_EAN_EXPORT_H_

// Export: canonical e-graph  ->  PathExprFactory<TransferT>::Ref.
//
// M1 "tree" export: each e-class currently holds a single e-node (no rewrites
// have run), so node selection is trivially `nodes.front()`. When reuse-aware
// batch extraction lands (M4), only `pick()` below changes — it becomes a
// cost-driven choice of the best e-node per class; the recursive rebuild and
// the shared-node memo stay exactly as they are.
//
// Cross-root sharing is recovered automatically: `memo_` keyed by canonical Id
// ensures each e-class is materialized once, and PathExprFactory's own
// hash-consing (unite/concat/star caches) collapses structurally equal nodes.
// Atoms are re-exported by reusing their original Ref verbatim (opaque to EAN).

#include <unordered_map>
#include <vector>

#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/EAN/AtomTable.h"
#include "Dataflow/APA/EAN/Import.h" // for the Graph type alias
#include "Dataflow/APA/EAN/PathLang.h"

namespace elimination {
namespace ean {

namespace detail {

template <typename TransferT> class Exporter {
public:
  using Factory = PathExprFactory<TransferT>;
  using Ref = typename Factory::Ref;

  Exporter(const Graph &g, const AtomTable<TransferT> &atoms, Factory &f)
      : g_(g), atoms_(atoms), f_(f) {}

  Ref exportId(Id c) {
    c = g_.find(c);
    auto it = memo_.find(c);
    if (it != memo_.end()) {
      return it->second;
    }
    Ref result = build(pick(c));
    memo_.emplace(c, result);
    return result;
  }

private:
  // M1: single e-node per class. M4 replaces this with cost-based selection.
  const PathLang &pick(Id c) { return g_[c].nodes.front(); }

  Ref build(const PathLang &n) {
    if (isZero(n)) {
      return f_.zero();
    }
    if (isOne(n)) {
      return f_.one();
    }
    if (isAtom(n)) {
      return atoms_.atom(parseAtomId(n.op()));
    }
    if (isStar(n)) {
      return f_.star(exportId(n.children().front()));
    }
    if (isJoin(n)) {
      const auto &kids = n.children();
      Ref acc = exportId(kids.front());
      for (std::size_t i = 1; i < kids.size(); ++i) {
        acc = f_.unite(acc, exportId(kids[i]));
      }
      return acc;
    }
    // isSeq
    const auto &kids = n.children();
    Ref acc = exportId(kids.front());
    for (std::size_t i = 1; i < kids.size(); ++i) {
      acc = f_.concat(acc, exportId(kids[i]));
    }
    return acc;
  }

  const Graph &g_;
  const AtomTable<TransferT> &atoms_;
  Factory &f_;
  std::unordered_map<Id, Ref> memo_;
};

} // namespace detail

// Export one e-class `root` back into factory `F`, returning an equivalent
// path-expression Ref.
template <typename TransferT>
typename PathExprFactory<TransferT>::Ref
exportTree(const Graph &g, const AtomTable<TransferT> &atoms, Id root,
           PathExprFactory<TransferT> &F) {
  detail::Exporter<TransferT> exp(g, atoms, F);
  return exp.exportId(root);
}

// Batch convenience: export every root of an ImportResult into `F`, preserving
// cross-root sharing within a single Exporter (one shared memo).
template <typename TransferT>
std::vector<typename PathExprFactory<TransferT>::Ref>
exportBatch(const ImportResult<TransferT> &imp, PathExprFactory<TransferT> &F) {
  detail::Exporter<TransferT> exp(imp.g, imp.atoms, F);
  std::vector<typename PathExprFactory<TransferT>::Ref> out;
  out.reserve(imp.roots.size());
  for (Id r : imp.roots) {
    out.push_back(exp.exportId(r));
  }
  return out;
}

} // namespace ean
} // namespace elimination

#endif // DATAFLOW_APA_EAN_EXPORT_H_
