==================================
UnderApproxAA — Must-Alias Analysis
==================================

Overview
========

UnderApproxAA is a **sound but incomplete** alias analysis that computes
**must-alias** information: if two pointers are reported as aliasing, they
are **guaranteed** to point to the same memory; otherwise the result is
unknown.

* **Location**: ``include/Alias/Specialized/UnderApproxAA/`` / ``lib/Alias/Specialized/UnderApproxAA/``
* **Style**: Intra-procedural, under-approximate
* **Goal**: Cheap, reliable must-alias facts for safety-critical reasoning

Components
==========

* **Canonical** – Canonical form representations
* **EquivDB** – Equivalence class database
* **UnderApproxAA** – Core under-approximation algorithm

**Features**: Conservative alias analysis for safety-critical applications.

Algorithm
=========

UnderApproxAA maintains an **equivalence relation** over pointer-producing
values using a union-find data structure:

1. **Seeding (Atomic Rules)**  
   Scan instructions and apply local, syntactic rules (identity,
   bitcast/addrspace casts, zero-offset GEPs, trivial PHIs/selects, etc.)
   to add must-alias pairs to a worklist.

2. **Propagation (Semantic Rules)**  
   Process the worklist and unify equivalence classes. When classes merge,
   re-check watched PHI and select instructions. If all operands belong to
   the same class, new must-alias facts are inferred.

The resulting equivalence classes encode must-alias sets. Queries are
essentially ``O(α(N))`` union-find lookups.

Characteristics
===============

**Strengths**

* No false positives: any reported alias is guaranteed.
* Very fast construction and query time.
* Simple data structures and clear semantics.

**Limitations**

* Intra-procedural only (no cross-function reasoning).
* Focused on stack/global allocations; heap modeling is limited.
* May miss many true aliases (by design, under-approximate).

Usage
=====

UnderApproxAA is exposed via the AA wrapper and can be enabled where
**must-alias** information is preferable to may-alias over-approximation.
It is especially useful as a building block in verification and
sanity-checking passes.
















