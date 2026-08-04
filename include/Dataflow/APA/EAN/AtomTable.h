#ifndef DATAFLOW_APA_EAN_ATOMTABLE_H_
#define DATAFLOW_APA_EAN_ATOMTABLE_H_

// AtomTable: a bijection between the opaque transfer atoms of a
// PathExprFactory<TransferT> and the small integer ids that PathLang encodes
// into "atom#<id>" operator names.
//
// Deduplication key is the Expr* pointer of the atom node, NOT the TransferT
// value. Within one PathExprFactory an atom is hash-consed, so "same atom" ==
// "same Ref" == "same pointer"; this judgement does not depend on whether
// TransferT is equality-comparable (PathExprFactory only value-dedups atoms
// when TransferT is comparable, and falls back to per-node identity otherwise).
//
// Export reuses the original Ref stored here verbatim — atoms are opaque to
// EAN, so there is nothing to rebuild. This is valid because the owning
// factory outlives an EAN run.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Dataflow/APA/Core/PathExpr.h"

namespace elimination {
namespace ean {

template <typename TransferT> class AtomTable {
public:
  using Factory = PathExprFactory<TransferT>;
  using Ref = typename Factory::Ref;
  using Expr = typename Factory::Expr;

  // Return the stable id for the atom `e`, allocating one on first sight.
  // Precondition: e && e->K == Kind::Atom.
  std::uint32_t intern(const Ref &e) {
    const Expr *key = e.get();
    auto it = ptr_to_id_.find(key);
    if (it != ptr_to_id_.end()) {
      return it->second;
    }
    const auto id = static_cast<std::uint32_t>(id_to_atom_.size());
    ptr_to_id_.emplace(key, id);
    id_to_atom_.push_back(e);
    return id;
  }

  // Recover the original atom Ref for a previously interned id.
  // Precondition: id < size().
  const Ref &atom(std::uint32_t id) const { return id_to_atom_[id]; }

  std::size_t size() const { return id_to_atom_.size(); }

private:
  std::unordered_map<const Expr *, std::uint32_t> ptr_to_id_;
  std::vector<Ref> id_to_atom_;
};

} // namespace ean
} // namespace elimination

#endif // DATAFLOW_APA_EAN_ATOMTABLE_H_
