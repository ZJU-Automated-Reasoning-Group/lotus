===========================================
Strict Relations Alias Analysis — Algorithm
===========================================

Overview
========

Strict Relations Alias Analysis (SRAA) is a pointer-alias analysis that
proves pointers *cannot* alias by establishing **strict (<, ≤) relations**
between symbolic numeric expressions that describe memory objects.
The technique combines:

* An SSI/e-SSA-style live-range–splitting transformation,
* A symbolic Range Analysis pass, and
* A constraint solver for strict inequalities.

* **Location**: ``lib/Alias/Specialized/SRAA``

Highlights
==========

* Proves ``NoAlias`` results by reasoning about strict inequalities on pointer offsets
* Uses ``InterProceduralRA`` to infer value ranges and propagate constraints
* Tracks primitive layouts of aggregate types to compare sub-field accesses

Usage
=====

Register the ``StrictRelations`` pass (``-sraa``) inside an LLVM pass pipeline or via the Lotus AA wrapper.

This document focuses solely on the algorithmic workflow of SRAA as implemented in the artifact.

Inputs / Outputs / Invariants
-----------------------------

* **Inputs:** LLVM IR in SSA form, size metadata for abstract memory objects, target ABI layout rules.
* **Outputs:** Pairs of pointers classified as `NoAlias` (strictly proven) or `MayAlias`.
* **Invariants:** symbolic expressions remain affine, constraints are monotone, and transformations preserve IR semantics.

-------------------------------------
Algorithmic Pipeline (High-Level View)
-------------------------------------

The analysis is structured as a pipeline:

1. **Live-Range Splitting (vSSA)**
2. **Range Analysis (RA)**
3. **Strict-Inequality Generation**
4. **Constraint Propagation**
5. **Alias Disambiguation**
6. **(Optional) PDG node reduction**

Data products exchanged between stages:

* vSSA emits per-version def-use chains and symbolic expressions.
* RA outputs symbolic lower/upper bounds per versioned value.
* Inequality construction synthesizes strict constraints in canonical form.
* The solver produces disjointness facts and contradiction reports for diagnostics.

Each stage is described in detail below.

==============================
1. vSSA / SSI Transformation
==============================

SRAA requires precise symbolic information per program point.
Therefore, the implementation first transforms the IR into a variant of SSI
(Sparse Static Single Information) called **vSSA**.

The vSSA transformation performs:

1. **Splitting at control-flow joins**  
   Insert φ-like nodes for variables whose value-flow merges.

2. **Splitting at control-flow forks**  
   Insert σ-like nodes to preserve flow sensitivity of interval constraints.

3. **Splitting at uses of pointer-index expressions**  
   Ensures every symbolic expression has a single definition site.

Formally:

For each variable *x*:

* Insert φ where `x_in(B1)` ≠ `x_in(B2)`
* Insert σ at outgoing edges when `range(x)` changes on different successors
* Create a unique version `x_i` for each definition/merge/split

Implementation notes:

* vSSA runs after mem2reg and before alias analysis passes that expect canonical pointers.
* Synthetic definitions are tagged so later passes can ignore them when materializing code.
* The transformation keeps debug metadata and dominance info in sync, ensuring downstream passes receive well-formed IR.

This results in a def-use graph suitable for sparse constraint propagation.

===========================
2. Symbolic Range Analysis
===========================

The Range Analysis pass computes symbolic ranges of the form:

x ∈ [L(x), U(x)]

where `L(x)`, `U(x)` are *symbolic linear expressions* derived from:

* loop induction variables  
* pointer arithmetic (GEP)  
* branch conditions  
* integer arithmetic  
* comparisons

Example symbolic constraints inferred:

i_3 = i_2 + 1 ⇒ L(i_3) = L(i_2)+1, U(i_3) = U(i_2)+1
if (i < n) ⇒ U(i) ≤ U(n)-1


Propagation strategy:

1. Seed bounds using constants, known loop trip counts, and structure layout info.
2. Push constraints along vSSA def-use edges in topological order.
3. Relax bounds when a tighter symbolic expression is discovered.
4. Iterate until no bound changes (typically a small number of rounds because the graph is sparse).

The RA solver is **linear**, **sparse**, and propagates constraints until fixed point. It records justification edges so later stages can reconstruct the proof chain that produced every bound.

====================================
3. Strict-Inequality Construction
====================================

Given pointer expressions:


p = base + offset_p
q = base + offset_q



SRAA constructs strict inequalities such as:


offset_p + size_p ≤ offset_q
offset_q + size_q ≤ offset_p
offset_p < offset_q



These are derived from:

* value ranges from RA  
* control-flow–guarded conditions  
* loop bounds  
* comparisons in the IR (e.g., `if (i < j)` introduces `i < j` constraint)

Each inequality is represented in canonical form:

ax + by + ... < k

Canonicalization steps:

* Normalize coefficients so gcd is 1, making syntactic deduplication easier.
* Replace non-strict constraints generated by RA (≤) with equivalent strict forms when object sizes allow.
* Attach provenance (IR instruction, dominance frontier) for auditing and debugging.

The system contains only affine expressions.

================================
4. Constraint Propagation Solver
================================

SRAA maintains a strict-inequality constraint graph:

* Nodes: symbolic variables (offsets, indices, derived expressions)
* Edges: inequalities of form `x < y + c`

Algorithm:

1. Initialize graph with inequalities from Range Analysis.
2. For every edge `(x → y)` labeled `+c`, propagate:  
   `L(y) ≥ L(x) + c` and `U(x) ≤ U(y) - c`
3. When two expressions conflict:
   If `offset_p + size_p ≤ offset_q` is provably true, register a **disjointness fact**.
4. Detect contradictory cycles (negative-weight cycles in the negated system) and mark the originating constraints as infeasible for diagnostics.
5. Continue propagation until a fixed point.

Practical considerations:

* The solver caches shortest paths for repeated queries in interprocedural contexts.
* Worklists are partitioned per strongly connected component to reduce redundant relaxations.

This is a *Bellman–Ford–like relaxation* over a directed constraint graph,
but runs sparsely due to vSSA def-use structure.

=========================================
5. Alias Disambiguation (Final Decision)
=========================================

For two memory objects with computed offsets:

*Let:*

* P ranges over `[Lp, Up]`
* Q ranges over `[Lq, Uq]`
* And their sizes: `size(P)` and `size(Q)`

SRAA proves **NoAlias** if any of the following hold:

1. ::
       Up + size(P) ≤ Lq
2. ::
       Uq + size(Q) ≤ Lp
3. Strict relation holds::
       Up < Lq   OR   Uq < Lp

Example:

* `p = base + i * 4`, `0 ≤ i < n`
* `q = base + (n + j) * 4`, `0 ≤ j < m`
* `size(p)` = `size(q)` = 4

RA yields `Up = 4(n-1)` and `Lq = 4n`. Therefore `Up + size(p) = 4n ≤ Lq`, and SRAA reports `NoAlias`.

If none of the strict inequalities are provable, SRAA returns **MayAlias**.

Notes:

* Proofs use purely symbolic comparisons.
* No heap-shape or points-to analysis is required.
* SRAA is *context-insensitive* but *interprocedural*:
  formal-actual parameters are connected via synthetic φ-like merges.

============================================
6. PDG Memory-Node Reduction (Applicability)
============================================

For dependence-graph construction (as used in the paper’s applicability study):

If two memory objects are proven disjoint,
the PDG pass merges or eliminates corresponding memory nodes.

Optimization details:

* Proven `NoAlias` edges are annotated so later passes can skip building memory dependences entirely.
* When only one direction is proven (e.g., `p` before `q`), the PDG records a directed reduction, shrinking the transitive closure.

The result is fewer nodes and fewer dependence edges,
which improves precision in downstream analyses.

======================
Summary of Properties
======================

* **Sparse** due to vSSA/SSI transformation.
* **Linear** constraint solving on affine strict inequalities.
* **Provably sound** for proving disjointness (NoAlias).
* **Flow-sensitive** but **context-insensitive**.
* **Explainable**: every `NoAlias` result carries a justification chain for debugging.
* Produces **strict (<, ≤) disjointness proofs**, not approximate heuristics.
* Complements and composes with LLVM’s baseline alias analyses.

================
End of document.
================
