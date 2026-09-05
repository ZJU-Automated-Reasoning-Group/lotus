Platform and Console Utilities
==============================

``include/Utils/Platform/`` and ``lib/Utils/Platform/`` provide small utilities
for timing, progress reporting, and platform queries.

**Main components**:

- ``Timer`` for timeout checks and elapsed-time measurement.
- ``ProgressBar`` for terminal progress reporting.
- ``System`` for environment and system-level helpers.
- ``subprocess`` for running external processes and capturing their output.

These headers are shared by tools that need lightweight CLI-facing utilities.

Guidelines
----------

Use ``Timer`` for time budgets and elapsed-time reporting rather than relying
on ad hoc wall-clock calculations.  Long-running tools should make progress
reporting optional so redirected output remains machine-readable.  Calls that
launch external programs should surface the command status and captured error
output to the user or caller.

See also :doc:`utilities`.
