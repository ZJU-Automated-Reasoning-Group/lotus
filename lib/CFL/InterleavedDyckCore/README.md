# Interleaved-Dyck Core

`CanaryInterleavedDyckCore` owns the shared typed input model used by Lotus's
interleaved-Dyck analyses. It deliberately contains no reachability algorithm.

## Shared representation

The public API in `include/CFL/InterleavedDyckCore/Graph.h` defines:

- `LabelKind` and `Label` for typed parenthesis, bracket, and neutral symbols;
- `Vertex`, `Edge`, `Pair`, and their hashers;
- a deduplicating `Graph`; and
- the common DOT parser used by the benchmark corpus.

Artifact labels `op--N`, `cp--N`, `ob--N`, `cb--N`, and `normal` retain their
typed identities. Unary aliases `+1`, `-1`, `+2`, `-2`, and `eps` parse to ID
zero labels, which is convenient for exact unary tests.

## Consumers

| Consumer | Use of the shared graph |
|---|---|
| `InterleavedDyckApproximation` | Direct typed lower/upper pipeline input |
| `AdaptiveInterleavedDyck` | Unary projection followed by bidirected validation |
| `MCFL::InterleavedDyckSolver` | Adapter to the generic string-labeled MCFL graph |

The shared graph is the comparison boundary for
`benchmarks/interleaved-dyck-approximation`. Algorithm-specific graph indexes
and grammar IRs remain private to their consumers.

The core preserves input arcs exactly and never adds complement reverse arcs.
Bidirecting is an algorithm policy: Adaptive can reject or explicitly
symmetrize; Approximation and MCFL analyze the directed graph as supplied.

## Why grammars are not shared concrete classes

`mutual_refinement::CnfGrammar` represents an integer-encoded Chomsky-normal
form grammar. `mcfl::Grammar` represents variable-arity multiple-context-free
predicates and tuple-producing rules. Neither is a subtype or lossless
encoding of the other, so merging them would erase important invariants. They
share the typed input graph through explicit adapters instead.
