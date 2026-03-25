# MPI Static Analysis

This module provides static analysis for MPI programs in LLVM IR. It focuses on
communication structure in the SPMD model rather than shared-memory threading.

## Components

- `MPIProcessModel`: extracts MPI operations and records metadata such as
  communicator, rank, tag, request, and window handles. It is the emitter of
  normalized MPI facts, not the final semantic owner of point-to-point or
  request truth.
- `MPICollectiveAnalysis`: owns collective protocol composition, collective
  compatibility, and rank-guarded collective reasoning.
- `MPIRMAAnalysis`: tracks RMA windows, synchronization epochs, and possible
  RMA races.
- `MPIRankAnalysis`: symbolic rank reasoning used by the collective checker.

## Authoritative Facts

The following internal facts are the primary reasoning surfaces for the MPI
subsystem:

- `MPIProcessSetFact` / `MPIParticipantSet`: canonical process/rank scope facts
- `MPIRequestSetFact`: request lifecycle and completion-scope facts
- `MPIChannelAutomaton`: channel state, ambiguity, and discharge facts
- `MPIChannelObligation`: projected point-to-point and request/discharge facts
- `CollectiveProtocolFrontier`: collective grouping/proof state
- `RMASynchronizationFact`: RMA epoch, completion, and synchronization facts
- `MPIFunctionSummary`: projected function exit-state across channel/request and
  collective effects

Legacy result buckets and summary counters are projected from these facts for
compatibility.

## Entry Point

Use [MPIAnalysis.h](lotus/include/Analysis/Concurrency/MPI/MPIAnalysis.h):

```c++
mpi::MPIAnalysis analysis(module);
analysis.runAnalysis();
const auto &results = analysis.getResults();
```

The top-level results include:

- orphaned non-blocking requests
- potential blocking send/recv deadlocks
- mismatched collectives
- conditional collectives
- unsynchronized RMA operations
- potential RMA races
- leaked windows

## Supported Modeling

- Point-to-point: blocking and non-blocking send/recv, plus `MPI_Sendrecv`
- Collectives: barriers and common blocking/non-blocking collectives
- Requests: `Wait*`, `Test*`, `Request_free`, `Cancel`
- Symbol aliases: `PMPI_*`, `__wrap_MPI_*`, and OpenMPI internal
  `ompi_mpi_*` symbols are normalized to MPI semantics
- Communicators: basic alias/canonicalization support for duplicated or split
  communicators
- RMA: `Put`, `Get`, `Accumulate`, selected atomic ops, and lock/fence-style
  synchronization

## Limitations

- Deadlock detection is static and conservative; unresolved request/channel
  facts degrade to explicit model gaps.
- Collective checking is summary-driven but still conservative when
  communicator, participant, or helper-summary scopes are unresolved.
- Unknown ranks, tags, or communicators are handled conservatively.
- PSCW RMA synchronization is modeled, but unresolved access/exposure scopes
  still degrade to model gaps rather than strong proofs.
- Some public result buckets remain compatibility projections of richer
  automaton/summary state rather than direct user-facing semantic APIs.

## Tests

See:

- `tests/unit/Concurrency/MPIAnalysisTest.cpp`
- `tests/unit/Concurrency/MPIRankAnalysisTest.cpp`

## Related Work

- MC-CChecker, EuroMPI 2018
- MC-Checker, SC 2014
- Dynamic Data Race Detection for MPI-RMA Programs, EuroMPI 2021
