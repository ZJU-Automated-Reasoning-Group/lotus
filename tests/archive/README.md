# Archived test inputs

This directory preserves historical inputs that are not registered as Lotus
tests. Archived inputs are not copied into the build tree and must not be used
as an implicit fixture search path.

To reactivate an input, move the smallest useful set to
`tests/regress/<OwningModule>/`, add a deterministic oracle, and register its
consumer in CMake.
