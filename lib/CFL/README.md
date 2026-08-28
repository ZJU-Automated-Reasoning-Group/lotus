# CFL Library

Context-free-language reachability and graph simplification.

| Subdir | Purpose |
|--------|---------|
| **Classical** | Classical CFL-reachability algorithms. Includes EBNF grammar normalization, matrix/PAG-style labeled graph loading, cubic CFL solving, SC reduction, and CNF/STBDU utilities |
| **CSIndex** | Indexing extended Dyck-CFL reachability for context-sensitive analysis (OOPSLA 22). Backbone discovery, gate graph, tabulation, compression. |
| **InterDyckGraphReduce** | Graph simplification for interleaved Dyck-reachability (PLDI 20). Bracket/paren matching. |
| **InterleavedDyck** | Staged regularization, under/overapproximation, parity grammar, mutual refinement, and on-demand interleaved-Dyck analysis. |
| **MCFL** | Normal-form multiple-context-free-language reachability with proof witnesses and the POPL 2025 interleaved-Dyck underapproximation hierarchy. |
| **MutualRefinement** | Mutual refinement of CFL reachability (SAS 23). |

## Relationship between the interleaved-Dyck components

`InterDyckGraphReduce`, `MutualRefinement`, `InterleavedDyck`, and `MCFL` all
address language-constrained reachability involving two interleaved Dyck
alphabets, but they operate at different layers and are not interchangeable:

```text
                         interleaved-Dyck graph
                                  |
             +--------------------+--------------------+
             |                    |                    |
  InterDyckGraphReduce     InterleavedDyck            MCFL
  specialized graph       staged projected-CFL       general MCFG
  simplification          approximation pipeline     saturation
                                  |
                         MutualRefinement
                         CFL refinement and
                         derivation tracing
```

| Component | Role | Approximation/result | Current integration |
|-----------|------|----------------------|---------------------|
| [InterDyckGraphReduce](InterDyckGraphReduce/README.md) | PLDI 2020 specialized graph simplification for two interleaved Dyck alphabets | Reduces the graph before or during specialized reachability processing | Standalone implementation with its own `CFLGraph` and reduction data structures |
| [MutualRefinement](MutualRefinement/README.md) | SAS 2023 refinement of multiple CFL reachability views | Refines projected CFL overapproximations and retains derivation information | Reused directly by `CanaryInterleavedDyck` |
| [InterleavedDyck](InterleavedDyck/README.md) | Staged interleaved-Dyck analysis combining projected grammars, underapproximation, parity refinement, and on-demand checks | Produces both under- and overapproximation stages | Links against `MutualRefinement`; does not currently call `InterDyckGraphReduce` or `MCFL` |
| [MCFL](MCFL/README.md) | POPL 2025 general normal-form MCFG solver and the `G_d^circ`/`G_d^+` hierarchy | Dimension-indexed underapproximation with concrete witnesses; optional artifact-compatibility behavior | Independent graph/grammar representation; no adapter to the other three modules yet |

The separation is intentional for now: the modules have different grammar
models, graph representations, invariants, and proof objects. Plausible future
integration points include a shared DOT/label adapter, using graph reduction
as preprocessing for MCFL, and feeding MCFL-confirmed pairs into refinement or
condensation. Such composition needs explicit preservation arguments and
conversion code; it should not be inferred from the conceptual relationship
alone.
