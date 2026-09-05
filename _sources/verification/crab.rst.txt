CRAB - Abstract Interpretation Library
=====================================

CRAB is the abstract interpretation library vendored in Lotus under
``third-party/crab/``. It provides the core abstract domains, fixpoint
engines, and CrabIR infrastructure that CLAM builds on.

**Location**: ``third-party/crab/``

Overview
--------

CRAB is a reusable C++ library for building static analyses over a CFG-based
intermediate representation called CrabIR. In Lotus, it is primarily consumed
through CLAM, but the library itself is worth documenting because it contains
the underlying domain implementations, analyzers, and solver machinery.

**Key Features**:

* Numerical abstract domains such as intervals, congruences, zones, octagons,
  polyhedra, disjunctive intervals, and reduced products
* Forward, backward, and interprocedural analyses
* WTO-based fixpoint iteration with widening heuristics and thresholds
* CrabIR, a strongly typed three-address IR with assumptions and assertions
* Optional integration with Apron, Elina, LDD/Boxes, and PPLite

Repository Structure
--------------------

**Headers** (``third-party/crab/include/crab/``):

* ``cfg/`` - CrabIR CFG definitions and utilities
* ``domains/`` - Abstract domains and supporting data structures
* ``analysis/`` - Forward, backward, dataflow, and interprocedural analyzers
* ``fixpoint/`` - Fixpoint iterators, WTO support, and thresholds
* ``transforms/`` - IR-level transformations
* ``support/`` - Statistics, debugging, and OS helpers

**Sources** (``third-party/crab/lib/``):

* Core numeric and domain implementations such as intervals, congruences,
  wrapped intervals, symbolic terms, and region information

Build Configuration
-------------------

CRAB is built with CMake and can be compiled standalone or as an embedded
dependency.

**Common options**:

* ``CRAB_ENABLE_TESTS`` - Build CRAB's standalone test programs
* ``CRAB_BUILD_LIBS_SHARED`` - Build shared libraries instead of static ones
* ``CRAB_USE_APRON`` - Enable Apron-backed domains
* ``CRAB_USE_ELINA`` - Enable Elina-backed domains
* ``CRAB_USE_LDD`` - Enable Boxes/LDD domains
* ``CRAB_USE_PPLITE`` - Enable PPLite-backed domains

**Standalone build**:

.. code-block:: bash

   mkdir build && cd build
   cmake .. -DCRAB_ENABLE_TESTS=ON
   cmake --build .

Integration With Lotus
----------------------

* CLAM uses CRAB for abstract domains, fixpoint solving, and its internal IR
* Lotus documents CLAM as the primary user-facing verifier, while CRAB is the
  lower-level analysis library underneath it
* CRAB's domain choices surface in CLAM flags such as ``--crab-dom``

If you are interested in running analyses directly from Lotus, start with
:doc:`clam`. If you need to understand where the domains and abstract
interpretation machinery come from, CRAB is the relevant component.

See Also
--------

- :doc:`clam` - Lotus verifier built on top of CRAB
- ``third-party/crab/README.md`` - Upstream-oriented build and usage notes
- https://github.com/seahorn/crab/wiki/Home - CRAB project documentation
