# Mutual refinement of CFL reachability

This is an illustrative implementation of the algorithm proposed in
*Mutual Refinements of Context-Free Language Reachability* (SAS 2023), based
on the [upstream implementation](https://github.com/sdingcn/mutual-refinement/).

## Scope

This library is a grammar-agnostic engine, not an interleaved-Dyck analysis
pipeline. It provides:

- an integer-encoded Chomsky-normal-form `CnfGrammar`;
- CFL saturation over an integer-encoded `CnfGraph`;
- unary/binary derivation records; and
- backward closure from derived reachability edges to contributing input
  edges.

Clients supply the grammar, graph encoding, refinement schedule, and meaning
of the result. The library does not parse `op/cp/ob/cb` labels, construct
projected or parity grammars, classify results as lower or upper bounds,
perform benchmark preprocessing, or issue on-demand interleaved-Dyck queries.

`MutualRefinementMain.cpp` also contains the original generic file-driven
experiment and an alternating refinement loop. It treats label strings as
opaque grammar symbols and exposes `mutual_refinement_main`, but it still has
no typed interleaved-Dyck semantics or benchmark pipeline. The reusable public
headers are the lower-level `CnfGrammar` and `CnfGraph` APIs described above.

## Relationship to other Lotus CFL components

Mutual refinement is the projected-CFL refinement layer in Lotus's broader
interleaved-Dyck family:

- [Interleaved-Dyck Approximation](../InterleavedDyckApproximation/README.md)
  is a domain-specific client of this library. It constructs the grammars,
  translates typed graph labels, orchestrates alternating refinement, and
  interprets results as lower and upper bounds.
- [`InterDyckGraphReduce`](../InterDyckGraphReduce/README.md) attacks the same
  family of problems through specialized graph simplification rather than
  alternating grammar refinement.
- [`MCFL`](../MCFL/README.md) derives multi-component grammar facts directly
  and yields dimension-indexed underapproximations with concrete witnesses.

The latter two modules do not currently depend on `MutualRefinement`. In
particular, their graph, grammar, and proof representations are different, so
combining them requires explicit adapters rather than a direct API call. The
[CFL overview](../README.md) summarizes the roles and possible future
composition points.

## Command line

Build the original generic experiment driver with:

```sh
cmake --build build --target lotus-cfl-mutual-refinement
build/bin/lotus-cfl-mutual-refinement grammars.txt graph.dot refine
```

The final argument is `naive` or `refine`. The grammar file contains one or
more blocks; the first `|` row names the start symbol and later rows encode
epsilon, unary, or binary productions:

```text
{
| S
| S
| S a
| S S S
}
```

Graph labels are treated as opaque terminals. This CLI does not assign
interleaved-Dyck lower/upper semantics.
