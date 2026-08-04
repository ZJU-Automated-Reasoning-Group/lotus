#ifndef DATAFLOW_APA_EAN_PATHLANG_H_
#define DATAFLOW_APA_EAN_PATHLANG_H_

// PathLang: the e-graph-side representation of APA path expressions.
//
// EAN reuses the generic `lotus::egraph::SymbolLang` node (a string operator
// plus a variadic vector of child Ids) rather than defining a bespoke language
// type. This header centralizes the operator-name convention so that the raw
// strings never leak into Import/Export/Rewrite code.
//
// Operator convention (see docs/EAN_项目计划书.md, M1):
//   "zero"      leaf            -- 0  (no path)
//   "one"       leaf            -- 1  (empty path)
//   "atom#<id>" leaf            -- opaque transfer atom, id keyed by AtomTable
//   "join"      variadic        -- ⊕  (choice);   children sorted + deduped (ACI)
//   "seq"       variadic        -- ·  (sequence);  children order-preserving
//   "star"      arity 1         -- *  (iteration)
//
// The variadic-vs-canonical rules (flatten / sort / dedup for join, flatten /
// keep-order for seq) are enforced by Import, not here; this header only names
// operators and builds nodes.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Solvers/EGraph/Id.h"
#include "Solvers/EGraph/Language.h"
#include "Solvers/EGraph/Util.h"

namespace elimination {
namespace ean {

// The e-graph language EAN operates on. A path expression e-node is one of the
// operators below applied to child e-classes.
using PathLang = ::lotus::egraph::SymbolLang;
using Id = ::lotus::egraph::Id;
using Symbol = ::lotus::egraph::Symbol;

namespace ops {
inline constexpr std::string_view kZero = "zero";
inline constexpr std::string_view kOne = "one";
inline constexpr std::string_view kJoin = "join";
inline constexpr std::string_view kSeq = "seq";
inline constexpr std::string_view kStar = "star";
// Atoms use the "atom#<decimal-id>" scheme; see atomOp / parseAtom below.
inline constexpr std::string_view kAtomPrefix = "atom#";
} // namespace ops

// ---- operator name helpers -------------------------------------------------

inline std::string atomOp(std::uint32_t id) {
  return std::string(ops::kAtomPrefix) + std::to_string(id);
}

// If `op` names an atom, return its id; otherwise return an empty optional.
// Implemented without <optional> churn via a (bool, id) pair to keep this a
// leaf header.
inline bool isAtomOp(const Symbol &op) {
  std::string_view sv = op.view();
  return sv.size() > ops::kAtomPrefix.size() &&
         sv.substr(0, ops::kAtomPrefix.size()) == ops::kAtomPrefix;
}

// Precondition: isAtomOp(op) is true.
inline std::uint32_t parseAtomId(const Symbol &op) {
  std::string_view sv = op.view();
  sv.remove_prefix(ops::kAtomPrefix.size());
  std::uint32_t id = 0;
  for (char c : sv) {
    id = id * 10u + static_cast<std::uint32_t>(c - '0');
  }
  return id;
}

// ---- node kind classification ----------------------------------------------

inline bool isZero(const PathLang &n) {
  return n.children().empty() && n.op() == ops::kZero;
}
inline bool isOne(const PathLang &n) {
  return n.children().empty() && n.op() == ops::kOne;
}
inline bool isJoin(const PathLang &n) { return n.op() == ops::kJoin; }
inline bool isSeq(const PathLang &n) { return n.op() == ops::kSeq; }
inline bool isStar(const PathLang &n) { return n.op() == ops::kStar; }
inline bool isAtom(const PathLang &n) {
  return n.children().empty() && isAtomOp(n.op());
}

// ---- node builders ----------------------------------------------------------

inline PathLang makeZero() { return PathLang::leaf(Symbol(std::string(ops::kZero))); }
inline PathLang makeOne() { return PathLang::leaf(Symbol(std::string(ops::kOne))); }
inline PathLang makeAtom(std::uint32_t id) {
  return PathLang::leaf(Symbol(atomOp(id)));
}
inline PathLang makeJoin(std::vector<Id> children) {
  return PathLang(Symbol(std::string(ops::kJoin)), std::move(children));
}
inline PathLang makeSeq(std::vector<Id> children) {
  return PathLang(Symbol(std::string(ops::kSeq)), std::move(children));
}
inline PathLang makeStar(Id body) {
  return PathLang(Symbol(std::string(ops::kStar)), std::vector<Id>{body});
}

} // namespace ean
} // namespace elimination

#endif // DATAFLOW_APA_EAN_PATHLANG_H_
