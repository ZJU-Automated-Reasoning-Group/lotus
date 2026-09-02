# Benchmarks

Benchmarks produce quantitative results; they are not default correctness
tests and are not added to the normal CTest graph.

- `micro/<Module>/`: small, focused workloads for measuring one mechanism.
- `real-world/<Module>/`: external datasets and whole-program suites.

Every maintained benchmark should document its owner, provenance/license,
input preparation, command line, measured metrics, and expected scale. A small
fixed input used only for pass/fail belongs under `tests/regress/` instead.
