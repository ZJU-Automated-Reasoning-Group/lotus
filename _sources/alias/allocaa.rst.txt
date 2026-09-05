=========================
AllocAA — Allocation-Based AA
=========================

Overview
========

AllocAA is a **lightweight, heuristic-based alias analysis** that reasons
primarily about **allocation sites**. It is designed to be extremely fast and
cheap in memory, making it suitable as a **baseline analysis** or as a
pre-filter before more expensive passes.

* **Location**: ``lib/Alias/Specialized/AllocAA``
* **Style**: Flow-insensitive, context-insensitive
* **Goal**: Quickly classify obvious non-aliases and simple must-alias cases

Algorithm Sketch
================

AllocAA tracks a small amount of information per allocation site:

* Distinguishes **stack**, **global**, and selected **heap** allocation sites.
* Builds coarse-grained groups of pointers that are known to originate from
  the same site.
* Uses simple syntactic patterns (alloca, globals, selected intrinsics) to
  propagate allocation provenance through assignments and casts.

The analysis never attempts to construct a full constraint graph; instead, it
answers queries using this coarse site-to-pointer mapping.

Strengths and Limitations
=========================

**Strengths**

* Very low analysis overhead.
* Works well as a **first-pass filter** for large code bases.
* Easy to combine with more precise analyses (e.g., LotusAA, AserPTA).

**Limitations**

* No interprocedural reasoning beyond basic call edges.
* Field-insensitive and path-insensitive.
* Can produce many **MayAlias** results for complex pointer patterns.

Usage
=====

AllocAA is exposed through the generic AA wrapper and via dedicated tools.
In most cases you do not run it directly; instead you enable it in the
pass pipeline (e.g., through the Lotus AA driver or Clam configuration).

AllocAA is integrated as LLVM ModulePass for basic alias queries.

