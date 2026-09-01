# CFL Library

Context-free-language reachability and graph simplification.

| Subdir | Purpose |
|--------|---------|
| **Classical** | Classical CFL-reachability algorithms. Includes EBNF grammar normalization, matrix/PAG-style labeled graph loading, cubic CFL solving, SC reduction, and CNF/STBDU utilities |
| **CSIndex** | Indexing extended Dyck-CFL reachability for context-sensitive analysis (OOPSLA 22). Backbone discovery, gate graph, tabulation, compression. |
| **InterDyckGraphReduce** | Graph simplification for interleaved Dyck-reachability (PLDI 20). Bracket/paren matching. |
| **InterleavedDyckCore** | Shared typed labels, graph, pair types, and DOT parser for interleaved-Dyck datasets. |
| **UnaryInterleavedDyck** | Exact Adaptive and POPL 2022 FixedCounter algorithms for bidirected unary `D1`-interleaved-`D1`. |
| **InterleavedDyckApproximation** | Staged lower/upper bounds, parity grammar, mutual refinement, and on-demand checks for typed interleaved Dyck (SOAP 25?). |
| **MCFL** | Normal-form multiple-context-free-language reachability and the POPL 2025 typed underapproximation hierarchy. |
| **MutualRefinement** | Integer CNF saturation, derivation tracing, and generic mutual-refinement experiments (SAS 23). |

## Relationship between the interleaved-Dyck components

`InterDyckGraphReduce`, `MutualRefinement`, `InterleavedDyckApproximation`, and
`MCFL` all address constrained reachability involving two interleaved Dyck
alphabets. Their guarantees and abstraction levels differ:

```text
                    typed D_k interleaved D_k
                              |
                 +------------+------------+
                 |                         |
          certified lower bounds     sound upper bounds
          MCFL G_d grammars          Interleaved-Dyck
          union-Dyck grammar         Approximation
                                            |
                                     MutualRefinement

                 bidirected unary D_1 interleaved D_1
                              |
                     UnaryInterleavedDyck
                              |
                 +------------+------------+
                 |                         |
        FixedCounterSolver          AdaptiveSolver
          cubic control           quadratic control
                 |                         |
                 +------------+------------+
                              |
                       exact partition
```

## Choosing a solver

| Question | API | Guarantee |
|---|---|---|
| Reachability for a client-supplied MCFG | `mcfl::Solver` | Exact for that grammar |
| Certified typed interleaved-Dyck pairs | `mcfl::InterleavedDyckSolver` | Dimension-indexed underapproximation |
| POPL 2022 exact baseline | `unary_interleaved_dyck::FixedCounterSolver` | Exact component partition after unary projection |
| Exact bidirected unary interleaved Dyck | `unary_interleaved_dyck::AdaptiveSolver` | Exact component partition after unary projection |
| Typed candidates plus lower/upper refinement | `interleaved_dyck_approximation::Solver` | Underapproximation and progressively tighter overapproximations |

## Directed and bidirected inputs

| Component | Policy |
|---|---|
| `InterleavedDyckCore` | Preserves exactly the arcs in the input file |
| `InterleavedDyckApproximation` | Analyzes the directed graph as supplied; adds no reverse arcs |
| `MCFL::InterleavedDyckSolver` | Analyzes supplied directed arcs; semantic epsilon self-paths are added internally |
| `UnaryInterleavedDyck` | Both algorithms reject non-bidirected input by default; explicit symmetrization returns a sound overapproximation for the original directed graph |
| `InterDyckGraphReduce` | Uses specialized reverse orientations/synthetic terminals inside its reduction proof; does not present them as original input arcs |

Any experiment that adds complement reverse arcs must report that transformation
and the resulting change from exact-original to overapproximate-original
semantics.

## Command-line tools

| Tool | Module | Purpose |
|---|---|---|
| `lotus-cfl-classical` | Classical | Run a supplied grammar and text/DOT/JSON graph with the baseline, POCR, or hybrid backend |
| `lotus-cfl-mcfl` | MCFL | Run the dimension-indexed `G_d` underapproximation hierarchy |
| `lotus-cfl-interleaved-dyck-approximation` | InterleavedDyckApproximation | Report staged typed lower/upper bounds |
| `lotus-cfl-unary-interleaved-dyck` | UnaryInterleavedDyck | Select `adaptive` or `fixed-counter` exact unary analysis |
| `lotus-cfl-mutual-refinement` | MutualRefinement | Run generic file-driven `naive` or `refine` CNF experiments |
| `lotus-cfl-inter-dyck-graph-reduce.py` | InterDyckGraphReduce | Orchestrate in-place graph simplification through the two compiled helper tools |

Each module README documents its input contract and options. There is no
combined comparison binary; experiments compose the individual tools or use
the shared typed graph APIs directly.

| Component | Role | Approximation/result | Current integration |
|-----------|------|----------------------|---------------------|
| [InterleavedDyckCore](InterleavedDyckCore/README.md) | Shared typed graph and DOT parser | No analysis result | Common input for Approximation, UnaryInterleavedDyck, and MCFL adapter |
| [InterDyckGraphReduce](InterDyckGraphReduce/README.md) | PLDI 2020 specialized graph simplification for two interleaved Dyck alphabets | Produces a reduced DOT graph, not a reachability relation | File-oriented CLI with private legacy summary structures; output can be reloaded through `InterleavedDyckCore` |
| [MutualRefinement](MutualRefinement/README.md) | Grammar-agnostic integer CNF saturation and derivation tracing | Returns grammar-relative reachability edges and contributing-edge closure; assigns no interleaved-Dyck lower/upper semantics | Low-level engine reused by `CanaryInterleavedDyckApproximation` |
| [Interleaved-Dyck Approximation](InterleavedDyckApproximation/README.md) | Domain pipeline combining typed label parsing, projected grammars, union-Dyck lower bounds, parity refinement, and on-demand checks | Produces a certified lower bound and progressively tighter upper bounds; exact only when they coincide | Links against `MutualRefinement` and owns all pipeline policy |
| [UnaryInterleavedDyck](UnaryInterleavedDyck/README.md) | Adaptive and fixed-counter algorithms for the bidirected unary specialization | Exact component partition | Both algorithms share projection, quotient, and Dyck backend; independent of MCFL |
| [MCFL](MCFL/README.md) | General normal-form MCFG solver and the `G_d^circ`/`G_d^+` hierarchy | Exact for a supplied MCFG; dimension-indexed typed underapproximation for the generated hierarchy | Accepts `InterleavedDyckCore` graphs through an adapter |

`InterleavedDyckCore` is the shared dataset boundary. Approximation consumes it
directly, UnaryInterleavedDyck uses its unary projection and Dyck backend, and
MCFL adapts it to its generic terminal-labeled graph. Algorithm-specific
grammar and solver IRs stay separate: MutualRefinement uses integer CNF,
whereas MCFL uses variable-arity MCFG predicates.
