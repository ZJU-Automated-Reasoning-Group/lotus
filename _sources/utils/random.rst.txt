Random Number Utilities
=======================

``include/Utils/Random/`` and ``lib/Utils/Random/`` provide Lotus's shared
random-number helper.

**Main component**:

- ``RNG`` wraps MT19937-style pseudo-random generation for repeatable internal
  experiments.

This utility is primarily used by testing, fuzzing support, and experimental
analysis code that needs deterministic randomization.

Reproducibility
---------------

Use an explicit seed when a random choice affects a test, benchmark, or bug
report.  Recording that seed makes a failing run reproducible and lets a
research experiment be repeated exactly.  Random choices should not affect
the semantic result of a production analysis unless that behavior is clearly
exposed to its caller.

See also :doc:`utilities`.
