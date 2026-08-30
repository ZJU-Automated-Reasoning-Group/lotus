# Multiple-context-free-language reachability

`CanaryMCFL` is a native C++17 implementation of the algorithms in
*Program Analysis via Multiple Context Free Language Reachability* (POPL
2025). It contains two layers:

- a reusable all-pairs solver for non-deleting, non-permuting MCFGs in the
  paper's five-rule normal form; and
- generators and a staged driver for the paper's `G_d^circ` and `G_d^+`
  underapproximations of interleaved-Dyck reachability.

## Which API should I use?

The two MCFL APIs have different notions of exactness:

| API | Input language/model | Guarantee | Output |
|---|---|---|---|
| `Solver` | Any supported MCFG supplied by the client | Exact for that grammar | All-pairs relation, tuple facts, and witnesses |
| `InterleavedDyckSolver` | Typed interleaved Dyck through `G_d^circ` or `G_d^+` | Sound underapproximation | One pair set and statistics per dimension |

“Exact for an MCFG” does not mean “exact for general interleaved Dyck.” The
`G_d` grammars recognize only a subset of the typed target language.

For the staged typed lower/upper approximation pipeline, use the separately
named **Interleaved-Dyck Approximation** module currently stored under the
directory `CFL/InterleavedDyckApproximation`. For exact bidirected unary
reachability, use the independent `CFL/UnaryInterleavedDyck` module.

## Generic solver

The public API is split across `include/CFL/MCFL/Graph.h`, `Grammar.h`, and
`Solver.h`. `Grammar` has explicit builders for all normal-form rules:

1. a one-component terminal or epsilon seed;
2. prepending one terminal to a tuple component;
3. appending one terminal to a tuple component;
4. inserting an independent terminal or epsilon component; and
5. concatenating the components of one or more derived predicates.

`Grammar::validate()` checks arities, start-symbol requirements, linearity,
non-deletion, and the non-permuting condition. `Solver::solve()` implements the
paper's worklist saturation. It indexes facts by nonterminal and by individual
component endpoints, uses endpoint constraints to join Type-5 premises, and
applies the paper's plain-reachability pruning between adjacent tuple
components. A stronger grammar-specific feasibility relation can be supplied
with `SolverOptions::gap_reachable`.

Every reported pair retains a proof DAG. `ReachabilityResult::witness()`
reconstructs a concrete labeled graph path without rerunning the analysis.
Implicit epsilon self-paths are omitted from the returned edge sequence.

## Interleaved Dyck hierarchy

`include/CFL/MCFL/InterleavedDyck.h` generates both grammar families for any
positive dimension:

- `InterleavedGrammarVariant::Simple` is `G_d^circ`: split each projected Dyck
  word into at most `d` components and interleave the components.
- `InterleavedGrammarVariant::Full` is `G_d^+`: add the paper's insertion and
  nesting productions.

For every dimension, reported pairs are certified members of the typed target
relation. A pair that is not reported is unresolved: it may become derivable
at a larger dimension or may lie outside the grammar hierarchy used in the
run.

The staged solver preserves the artifact's graph-side behavior:

1. discard delimiter IDs that lack either an opening or a closing edge;
2. add semantic epsilon self-paths;
3. contract eligible neutral edges and pairs proven mutually reachable by the
   previous dimension;
4. split weakly connected components;
5. use projected parenthesis/bracket Dyck reachability to prune tuple gaps;
6. run MCFL saturation; and
7. expand condensed results back to original vertices. By default, only pairs
   connected in the original graph are retained.

The reference artifact expands every condensed pair as a full Cartesian
product. For a one-way neutral edge this also creates the reverse pair, which
is not graph reachable. Set
`CondensationExpansionPolicy::ArtifactCompatible`, or pass
`--artifact-compatible` to the CLI, when exact reproduction of that observable
artifact behavior is required. The default reachability-filtered policy avoids
this false reverse pair.

`normal` edges are accepted as neutral symbols, matching the artifact's DOT
benchmarks. Public staged results omit reflexive pairs.

## Relationship to other Lotus CFL components

MCFL belongs to the same interleaved-Dyck analysis family as
[`InterDyckGraphReduce`](../InterDyckGraphReduce/README.md),
[`MutualRefinement`](../MutualRefinement/README.md), and
[Interleaved-Dyck Approximation](../InterleavedDyckApproximation/README.md),
but it has a distinct role:

- `InterDyckGraphReduce` specializes in graph simplification for two Dyck
  alphabets. Its reduced DOT output can be reloaded through the shared core,
  but using it before an MCFL grammar still requires the corresponding
  preservation argument; MCFL does not invoke it automatically.
- `MutualRefinement` provides grammar-agnostic CNF saturation and derivation
  tracing used by `InterleavedDyckApproximation`. MCFL instead saturates
  multi-component nonterminals directly.
- Interleaved-Dyck Approximation is a staged lower/upper-bound pipeline.
  MCFL's `G_d^circ` and `G_d^+` results are complementary, witness-producing
  underapproximations and are not currently wired into that pipeline.
- `UnaryInterleavedDyck` computes the exact answer for the bidirected unary
  specialization through Adaptive and FixedCounter algorithms, independently
  of the MCFG solver.

The typed hierarchy accepts the shared `interleaved_dyck::Graph` directly and
adapts it to the generic string-labeled MCFL graph. This lets Approximation and
MCFL load the same benchmark file without duplicate parsers. The generic MCFL
`Graph` remains necessary because arbitrary MCFG terminals are not limited to
the fixed typed interleaved-Dyck alphabet.

The implementations remain separate because MCFL tuple facts, grammar rules,
and proof DAGs do not have a lossless one-to-one mapping to the specialized
graph and CNF representations used by the other modules. See the
[CFL overview](../README.md) for the component-level comparison.

## Command line

Configure Lotus with `-DLOTUS_ENABLE_CFL=ON` and build
`lotus-cfl-mcfl`:

```sh
cmake -S . -B build -DLOTUS_ENABLE_CFL=ON
cmake --build build --target lotus-cfl-mcfl
build/bin/lotus-cfl-mcfl --dimension 2 input.dot
```

Use `--simple` for `G_d^circ`, `--no-condense` to disable the artifact's cycle
elimination, `--artifact-compatible` for the artifact's condensed
cross-product expansion, `--stats` for saturation counters, and
`--print-pairs` to emit the final relation.

The exact unary algorithms have their own
`lotus-cfl-unary-interleaved-dyck` executable; see the
[UnaryInterleavedDyck README](../UnaryInterleavedDyck/README.md).

## Fidelity checks

The focused tests reproduce the paper's complete length-4 and length-6
language tables:

| Grammar | Length 4 | Length 6 |
|---|---:|---:|
| `G_1^circ` | 6 | 18 |
| `G_2^circ` | 10 | 58 |
| `G_3^circ` | 10 | 70 |
| `G_1^+` | 8 | 40 |
| `G_2^+` | 10 | 70 |
| `G_3^+` | 10 | 70 |
| Exact interleaved Dyck | 10 | 70 |

The artifact's five small public taint cases also reproduce the published
non-reflexive pair counts for both dimensions (fakebanker 249/251, faketaobao
57/59, jollyserv 155/164, loozfon 76/93, and uranai 143/143).

## Provenance and licensing

The supplied reference artifact is GPLv3, while Lotus combines permissively
licensed components. This directory is therefore an independent C++
implementation from the published definitions and pseudocode, checked against
the artifact's observable input/output behavior. It does not copy or translate
the artifact's Go/Python source.
