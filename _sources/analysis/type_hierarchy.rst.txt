Type Hierarchy
==============

``TypeHierarchy`` provides class-hierarchy recovery for C++-style programs.

**Headers**: ``include/Analysis/TypeHierarchy/``

**Implementation**: ``lib/Analysis/TypeHierarchy/``

Overview
--------

This subsystem reconstructs inheritance and virtual-function-table structure
from LLVM IR and debug information. It is useful for analyses that need dynamic
type information or devirtualization-style reasoning.

Main components
---------------

- ``TypeHierarchy`` defines the generic hierarchy-query interface.
- ``DIBasedTypeHierarchy`` and ``DIBasedTypeHierarchyData`` recover hierarchy
  information from debug metadata.
- ``LLVMVFTable`` and ``LLVMVFTableData`` model virtual-function tables.
- ``TypeHierarchyAnalysis`` packages the functionality as an analysis pass.

Typical use cases
-----------------

- Build class-hierarchy facts for indirect-call resolution.
- Recover subtype relationships for object-oriented analyses.
- Provide a basis for vtable- and dynamic-dispatch reasoning.

Limitations
-----------

Recovery depends on the type and debug metadata present in the module.  A
hierarchy query is therefore evidence for resolving a dynamic dispatch, not a
guarantee that every runtime type has been recovered.  Clients should preserve
an unknown or conservative target case when metadata is missing, incomplete,
or inconsistent across linked modules.

