# Mutual refinement of CFL reachability

This is an illustrative implementation of the algorithm proposed in
*Mutual Refinements of Context-Free Language Reachability* (SAS 2023), based
on the [upstream implementation](https://github.com/sdingcn/mutual-refinement/).

## Relationship to other Lotus CFL components

Mutual refinement is the projected-CFL refinement layer in Lotus's broader
interleaved-Dyck family:

- [`InterleavedDyck`](../InterleavedDyck/README.md) links against this library
  and reuses its CNF saturation and derivation-tracing machinery in a staged
  under/overapproximation pipeline.
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
