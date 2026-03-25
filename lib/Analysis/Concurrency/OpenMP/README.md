# OpenMP Concurrency Analysis

This directory holds OpenMP-specific concurrency analysis components.

Current contents:

- `OpenMPTaskGraph`: models OpenMP task creation, taskwait/taskgroup
  boundaries, and `depend` edges for happens-before reasoning
- `OpenMPModel.h`: OpenMP runtime/API classification helpers used by
  `ThreadAPI` and the task analysis

This split keeps OpenMP-specific logic separate from generic concurrency
utilities and MPI-specific analysis.
