Microbenchmark Helpers
======================

``include/Utils/Benchmark/`` contains lightweight benchmarking helpers used to
measure internal analysis performance.

**Main components**:

- ``Microbench.h`` provides templated benchmark wrappers.
- ``ccutils::Stats`` accumulates timing summaries and simple statistics.

This module is header-only and is mainly intended for local experiments,
regression measurement, and algorithm comparisons.

Usage notes
-----------

Keep benchmark setup outside the measured callback so the result represents
the operation under study rather than allocation or fixture construction.
Repeat measurements on the same input and report the input size, build mode,
and machine configuration with results.  These helpers are deliberately small;
they provide timing summaries, not a replacement for a full benchmarking
framework.

See also :doc:`utilities`.
