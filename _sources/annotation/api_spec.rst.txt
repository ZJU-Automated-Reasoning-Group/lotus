API Specification System
========================

Unified specification format for declaring pointer, mod/ref, and related
function effects across analysis boundaries.

**Headers**: ``include/Annotation/APISpec.h``

Overview
--------

The API Specification system provides a unified format for declaring the
semantic effects of library and external functions. It consolidates multiple
spec-file formats (pointer specs, mod/ref specs, etc.) into a single structured
representation that can be consumed by alias analyses, checkers, and optimisers.

The specification system is designed to be lightweight (no LLVM header
dependencies in the public interface) and extensible.

Components
----------

SpecOpKind
~~~~~~~~~~

Describes the kind of operation a function performs on a particular value:

- ``Ignore`` — No effect (no-op).
- ``Alloc`` — Allocates new memory.
- ``Dealloc`` — Deallocates memory.
- ``Copy`` — Copies data between locations.
- ``Exit`` — Terminates the program.
- ``Mod`` — Modifies (writes to) the memory location.
- ``Ref`` — References (reads from) the memory location.

QualifierKind
~~~~~~~~~~~~~

Classifies how a value is qualified in the spec:

- ``Value`` — The value/pointer itself.
- ``Region`` — The pointee region.
- ``Data`` — The data object.
- ``Unknown`` — Unrecognised qualifier.

SelectorKind
~~~~~~~~~~~~

Identifies which program value the spec refers to:

- ``Ret`` — The function return value.
- ``Arg`` — The N-th argument (``Arg<N>``).
- ``AfterArg`` — Memory after the N-th argument (``AfterArg<N>``).
- ``Static`` — An external static address.
- ``Null`` — The null pointer literal.

Value Selectors
~~~~~~~~~~~~~~~

.. code-block:: cpp

   struct ValueSelector {
     SelectorKind kind;
     int index;     // Used for Arg/AfterArg
     bool isValid;
   };

CopyEffect, AllocEffect, ModRefEffect
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Each spec entry records:

- The target value (``ValueSelector``)
- The operation kind (``SpecOpKind``)
- Optional qualifiers and context information

FunctionSpec
~~~~~~~~~~~~

Aggregates all effects for a single function, keyed by function name.

APISpec
~~~~~~~

Top-level loader that parses spec files (``config/ptr.spec``,
``config/modref.spec``, etc.) and provides query access:

.. code-block:: cpp

   #include "Annotation/APISpec.h"

   lotus::APISpec spec;
   spec.loadFromDirectory("./config/");
   spec.loadFromFile("./extra.spec");

   auto effects = spec.queryAllEffects("malloc");
   auto modRef   = spec.queryModRef("memcpy");
   auto pointers = spec.queryPointerEffects("fopen");

Usage
-----

Spec files use a text format with entries like::

   function: malloc
     effect: alloc Ret

   function: memcpy
     effect: mod Arg1
     effect: ref Arg2

The loader parses these into the structured ``APISpec`` representation for use
by alias analyses and checkers.

See Also
--------

- :doc:`./annotation` — General annotation system overview
- :doc:`./modref` — Mod/ref function effect specifications
- :doc:`./pointer_effects` — Pointer effect specifications
