===========================
Sea-DSA — Memory Graph AA
===========================

Overview
========

Sea-DSA is a **context-sensitive, field-sensitive** alias and memory analysis
based on **Data Structure Analysis (DSA)**. It builds **heap graphs** that
summarize program memory and supports modular, interprocedural reasoning.

* **Location**: ``lib/Alias/UnificationBased/seadsa``
* **Tools**: ``lotus-alias-sea-dsa-dg``, ``lotus-alias-seadsa-tool``
* **Use Cases**: Precise mod/ref information, call-graph construction,
  memory-shape reasoning

Memory Graphs
=============

Sea-DSA represents memory as a graph where:

* Nodes correspond to **memory objects** (heap, stack, globals).
* Edges represent **field** and **pointer** relationships.
* Attributes encode properties such as singleton-ness, heap/stack origin,
  and mutation/escape information.

Graphs can be built bottom-up, top-down, and globally, and then merged or
refined through the analysis pipeline.

Usage
=====

.. code-block:: bash

   ./build/bin/lotus-alias-sea-dsa-dg --sea-dsa-dot example.bc
   ./build/bin/lotus-alias-seadsa-tool --sea-dsa-dot --outdir results/ example.bc

lotus-alias-sea-dsa-dg
----------

A simple tool for generating memory graphs using Sea-DSA analysis.

.. code-block:: bash

   ./build/bin/lotus-alias-sea-dsa-dg [options] <input LLVM bitcode file>

Key options:

* ``--sea-dsa-dot`` – Generate DOT files visualizing the memory graphs

This tool provides a straightforward interface to the Sea-DSA analysis for
generating memory graphs that can be visualized using Graphviz.

lotus-alias-seadsa-tool
-----------

Advanced Sea-DSA analysis tool with comprehensive memory graph analysis
capabilities.

.. code-block:: bash

   ./build/bin/lotus-alias-seadsa-tool [options] <input LLVM bitcode file>

Key options:

* ``--sea-dsa-dot`` – Generate DOT files visualizing memory graphs
* ``--sea-dsa-callgraph-dot`` – Generate DOT files of the complete call graph
* ``--outdir <DIR>`` – Specify an output directory for generated files

This tool provides advanced analysis capabilities for understanding memory usage
patterns, pointer relationships, and potential memory-related issues in programs.

Sea-DSA results are consumed by other analyses (e.g., mod/ref, verification
tools) to obtain a precise view of heap structure and aliasing.




