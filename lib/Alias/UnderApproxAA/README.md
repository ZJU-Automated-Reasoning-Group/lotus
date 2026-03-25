# UnderApproxAA V2

`UnderApproxAA` is a sound under-approximation for **must-alias** queries.

- `MustAlias` means the analysis proved the two pointer values denote the same
  address.
- `false` from `EquivDB::mustAlias()` means unknown.
- `MayAlias` is returned from the LLVM AA interface whenever the analysis cannot
  prove `MustAlias`.

## Semantic Core

The V2 engine is a forward dataflow analysis over three domains:

- `AliasGraph`: the CC'18-style access-path graph used as the semantic base
  for canonical pointer references.
- `expr_env`: a per-block environment mapping SSA pointer values to exact
  canonical references.
- `must_store`: a per-block map from singleton memory slots
  `(object, constant byte offset)` to exact canonical references.

Block joins are **intersections**. A fact survives only if it is present in
every predecessor.

## Canonical Pointer References

Internally the solver uses canonical pointer references of four kinds:

- `Value`: root SSA values such as function arguments.
- `AccessPath`: a root reference plus a structural GEP path.
- `Fixed`: null and globals.
- `Fresh`: allocation-producing roots such as `alloca`, recognized allocation
  calls, and `noalias` pointer returns.

The same structural reference is interned to the same `VarId`, which lets the
analysis use `AliasGraph::intersect()` directly at control-flow joins.

## Transfer Semantics

The solver handles pointer-producing instructions with exact transfer rules:

- Casts and all-zero GEPs forward the operand reference unchanged.
- Constant and symbolic GEPs extend the base reference with a structural path.
- `phi` and `select` produce a reference only when all incoming references are
  identical after predecessor intersection.
- Loads produce a reference only when the current `must_store` contains a fact
  for the singleton slot being loaded. If the slot fact is absent, optional
  `MemorySSA` and `DominatorTree` hooks may still recover a unique stored value.
- Stores update `must_store` only for singleton slots; otherwise the relevant
  fact is killed.

## Calls and Summaries

Direct callees may contribute a conservative summary:

- `ReturnRef`: `Arg(i)`, `ArgPath(i, path)`, `Null`, `Global`, or `Fresh`.
- `StrongStoreEffects`: exact strong updates to singleton argument-relative
  slots.

If a call has no exact summary:

- `readnone` / `readonly` preserve `must_store`.
- `argmemonly` kills only slots reachable from pointer arguments.
- other calls kill all caller-visible facts except unrelated non-escaping local
  alloca slots.

## Public Contract

- `UnderApproxAA::mustAlias()` remains function-scoped and boolean.
- `UnderApproxAA::alias()` returns `MustAlias` when proven, `MayAlias`
  otherwise.
- `UnderApproxAA::query()` is a deprecated wrapper with the same behavior as
  `alias()` on raw values.

## Files

- `AliasGraph.cpp` / `AliasGraph.h`: access-path graph and intersection logic
- `EquivDB.cpp` / `EquivDB.h`: V2 fixed-point solver and query database
- `UnderApproxAA.cpp` / `UnderApproxAA.h`: LLVM AA wrapper and per-function
  cache
