Core Utility Types
==================

``include/Utils/Types/`` contains small compatibility and convenience headers
used throughout the codebase.

**Main components**:

- ``Nullable`` and ``Offset`` for common analysis-side data wrappers.
- ``ScopeExit`` for lightweight RAII cleanup.
- ``range.h`` and vendored terminal-color helpers for ergonomic utility code.

These headers are intentionally low-level and widely reused.

Guidelines
----------

Prefer these wrappers when their semantics make an analysis invariant clearer,
but avoid introducing a utility dependency solely for a one-line convenience.
``ScopeExit`` is especially useful for cleanup that must run along every early
return path; ordinary ownership should still be expressed with standard RAII
types where possible.

See also :doc:`utilities`.
