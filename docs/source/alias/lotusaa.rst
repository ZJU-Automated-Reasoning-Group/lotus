========================
LotusAA — Lotus AA Engine
========================

Overview
========

LotusAA is the **native alias analysis framework** of Lotus. It provides a
modular engine with **interprocedural**, **flow-sensitive**, and
**field-sensitive** reasoning, designed to integrate tightly with other Lotus
analyses.

* **Location**: ``include/Alias/InclusionBased/LotusAA/``

Components
==========

* **Engine/** – Inter/intra-procedural analysis engines
* **MemoryModel/** – Points-to graph and memory modeling
* **Support/** – Configuration and utility functions

**Features**: Modular design for extensible pointer analysis.

Design
======

LotusAA organizes pointer information into a **points-to graph**:

* Nodes represent **memory objects** and **SSA values**.
* Edges represent **points-to**, **load**, **store**, and **field** relations.
* The graph is updated by a worklist-based solver that processes IR
  instructions according to a set of transfer functions.

The engine can operate in several modes (e.g., whole-program vs. module
local) and is designed to interoperate with higher-level analyses such as
dependence and verification passes.

Path-Sensitive Strong Updates
=============================

LotusAA implements the staged load/store matching algorithm from *Efficient
Strong Updates for Path Sensitive Data Dependence Analysis* (Guo and Zhang,
ICSE 2026). The algorithm prunes overwritten stores before guarded heap
histories are expanded, so loads only see stores that can still define them.

The matching proceeds in stages:

* ``getAliasCondition`` intersects guarded points-to targets directly to
  obtain the may-alias condition of a load/store pair.
* ``areMustAliases`` fingerprints canonical guarded points-to sets and
  confirms hash matches structurally, avoiding collision-based unsoundness.
* Each load reuses the kill forest of its immediate dominating must-alias
  load (its anchor) and considers only stores between the anchor and itself.
* A store kills an older store when their pointers must alias and removing
  the newer store disconnects every older-store-to-load CFG path.
* Only forest roots are expanded by the existing guarded heap walker. This
  retains LotusAA's summary, undef, and confidence handling while avoiding
  conditions for stores already proven dead.

The optimization is controlled by the ``-lotus-enable-must-kill`` flag
(default: true). It follows LotusAA's existing treatment of cyclic CFG
regions: only instructions numbered by the framework's acyclic topological
traversal participate in a must-kill forest.

Usage
=====

LotusAA is typically not run as a standalone tool. Instead, it is selected
via configuration:

* Clam / Lotus front-ends can choose LotusAA as the primary AA engine.
* YAML configurations under ``yaml-configurations/`` and command-line flags
  control whether LotusAA is enabled and how aggressively it runs.

When enabled, LotusAA registers itself with the AA wrapper so that all AA
queries issued by other passes go through its results.


