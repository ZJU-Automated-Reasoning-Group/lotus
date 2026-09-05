PDG — Program Dependence Graph
==============================

Overview
========

The **Program Dependence Graph (PDG)** is a fine-grained representation of
data and control dependences.
It is built on top of the ICFG and is used for slicing, security analyses,
and other dependence-aware queries.

* **Location**: ``lib/IR/PDG/``, ``include/IR/PDG/``

Core Passes
===========

The PDG infrastructure provides several LLVM passes:

* ``DataDependencyGraph`` – builds def-use, read-after-write (RAW), and
  alias-based **data dependence** edges.
* ``ControlDependencyGraph`` – computes **control dependences** between
  basic blocks and instructions.
* ``ProgramDependencyGraph`` – combines data and control dependences and
  exposes a unified query interface.

Unified Query API
=================

The public query layer lives in ``include/IR/PDG/Analysis/Query.h`` and is
organized around five service objects:

* ``SliceQuery`` – forward/backward slices, chops, and thin slices
* ``DependenceQuery`` – reachability and shortest-path style queries
* ``DataFlowQuery`` – reaching definitions, liveness, and control-region views
* ``TransformQuery`` – motion legality and scheduling helpers
* ``DiffQuery`` – structural differencing and impact summaries

All services share the same option/result model:

* ``PDGQueryOptions`` – edge preset, scope, context mode, traversal limits,
  cache policy, and explanation mode
* ``PDGQueryScope`` – whole graph, explicit node set, function, or prior query
  result
* ``PDGQueryResult`` – nodes, induced edges, predecessor map, witness paths,
  distances, and diagnostics
* ``PDGCriteriaResolver`` – seeds queries from nodes, LLVM values, function
  names, callee names, source locations, property specs, and Cypher results

High-Level Usage
================

PDG is exposed as an LLVM ``ModulePass``. A typical usage pattern:

.. code-block:: cpp

   #include "IR/PDG/Core/ProgramDependencyGraph.h"
   using namespace llvm;
   using namespace pdg;

   legacy::PassManager PM;
   PM.add(new DataDependencyGraph());
   PM.add(new ControlDependencyGraph());
   auto *pdgPass = new ProgramDependencyGraph();
   PM.add(pdgPass);
   PM.run(module);

   ProgramGraph *G = pdgPass->getPDG();
   SliceQuery slicer(*G);

   PDGCriteria criteria;
   criteria.values.push_back(src);

   PDGQueryOptions options;
   options.edge_preset = PDGEdgePreset::Data;
   options.context_mode = PDGContextMode::ContextSensitive;

   auto result = slicer.forward(criteria, options, &module);

For interactive querying and slicing, see the :doc:`../user_guide/pdg_query_language`
and the ``lotus-ir-pdg-query`` tool described in :doc:`../tools/ir`.

The Cypher selector language implementation is separated from these concrete
analysis services under ``include/IR/PDG/QueryLanguage/`` and
``lib/IR/PDG/QueryLanguage/``.

The Cypher query rule set was recently expanded (commit 3217fac4) with
additional rules for pattern matching over the PDG IR, including new
security-focused query templates. See ``tools/ir/examples/`` for the full
collection of example queries, including the
:doc:`../tools/ir/examples` cookbook.

`lotus-ir-pdg-query` Analysis Mode
=========================

``lotus-ir-pdg-query`` still accepts raw Cypher queries, but it can now also run the
PDG analysis services directly:

* ``--analysis slice-forward`` / ``slice-backward`` / ``chop``
* ``--analysis shortest-path`` / ``reaching-defs`` / ``control-region``
* ``--analysis live`` / ``dead`` / ``diff``

Analysis mode uses Cypher as a selector front end:

* ``--criteria-query`` selects analysis seeds
* ``--target-query`` selects targets for binary analyses
* ``--scope-function`` or ``--scope-query`` restrict analysis scope
* ``--edge-preset`` chooses one of ``all``, ``data``, ``control``,
  ``parameter``, ``interprocedural``, ``value-flow``, or
  ``transform-legality``
* ``--context-sensitive`` enables call/return matching
* ``--thin`` enables thin slicing semantics
* ``--format text|json|dot`` controls rendering
