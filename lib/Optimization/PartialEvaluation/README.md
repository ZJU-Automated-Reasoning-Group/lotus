# Partial Evaluation (PE) – LLPE for Lotus

This directory contains the **LLPE** (LLVM Partial Evaluator) engine, upgraded
from LLVM 5.0 to **LLVM 14.x** and integrated into Lotus.

The integration pass driver is at `Commit/Integrator.cpp`; the analysis pass
entry point is at `Engine/TopLevel.cpp`.

## Components

- **Engine/**: Abstract evaluation, control-flow and memory reasoning, shadow
  state, and the main analysis loop.
- **Commit/**: Residual-program generation, conditional specialisation, and
  integration into LLVM IR.
- **Support/**: Command-line handling, diagnostics, printing, I/O, and LLVM
  compatibility helpers.
- **Headers**: Stable public headers remain in
  `include/Optimization/PartialEvaluation/`.
- **Library**: The three implementation groups are aggregated into the stable
  `CanaryPE` static library.
- **Passes**: Legacy `ModulePass`es:
  - `llpe-analysis` – LLPE analysis (specialisation context and hypothetical
    constant folding)
  - `llpe` – LLPE integrator (commits specialisation)

## Using the PE passes

Link your tool with `CanaryPE` and any Lotus libraries it depends on. Register
and run the legacy passes as usual:

- Run the analysis: `LLPEAnalysisPass` (ID `llpe-analysis`).
- Run the integrator: `LLPEPass` (ID `llpe`) after the analysis; it calls
  `commit()` on the analysis result.

Example (conceptual): add `llpe-analysis` and `llpe` to your legacy pass
pipeline. Set the root function with `-llpe-root=<name>` (default: `main`).

## Optional integration

The PE library is built via `add_subdirectory(PartialEvaluation)` in
`lib/Optimization/CMakeLists.txt`. It is separate from the scalar, IPO, and
pipeline optimization libraries; tools that need partial evaluation should
link `CanaryPE` explicitly.


## References

I/O Optimisation and elimination via partial evaluation.
Christopher S.F. Smowton
