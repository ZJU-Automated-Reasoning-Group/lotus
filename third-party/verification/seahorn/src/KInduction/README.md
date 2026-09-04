# K-Induction Engine

k-induction verification that reuses Seahorn PathBMC.

## Design

- **Base case (k)**: Unwind loops to at most **k** iterations and run PathBMC.
  - If SAT → **BUG** (counterexample within k iterations).
  - If UNSAT → no counterexample within k iterations.
- **Inductive step (k)**: Prove that the property is *k-inductive*:
  1. Havoc (over-approximate) the initial state at the entry.
  2. Instrument loop headers with a fresh iteration counter.
  3. For steps **≤ k**, turn assertion/error checks into assumptions (induction hypothesis).
  4. For step **k+1**, keep checks enabled.
  5. Unwind loops to at most **k+1** iterations and run PathBMC.
     - If UNSAT → **SAFE** (property is k-inductive).
     - If SAT → not k-inductive yet; increase k.

The engine reuses PathBMC’s VC generation + solving; it does not modify PathBMC.
Unwinding is implemented using Seahorn's existing `LoopPeeler` and `CutLoops`.

Notes / current limitations:
- State havocing is conservative and currently targets scalar allocas + scalar globals (harder to prove, but sound if proved).
- The step counter is global (incremented at each loop header); this is a coherent “step” notion but may be coarse for programs with many loops.

## Dependencies

- Seahorn PathBMC (PathBmcEngine), CutPointGraph, OperationalSemantics (BvOpSem).
- Loop peeler + loop cutter: `seahorn::createLoopPeelerPass(unsigned Num)`, `seahorn::createCutLoopsPass()`.
- Requires CLAM for full PathBMC (path solving with Crab); falls back to encode-only if unavailable.

## Options

- `k-min`, `k-max`: range of k to try (default 1..∞ until timeout).
- `timeout`: total CPU timeout for the engine.
- `entry`: entry function (default `main`).

## Usage

- As an LLVM pass: run after CutPointGraph, ShadowMem, etc. (same as BmcPass). Use `createKInductionPass()` and add to the pass pipeline before or instead of PathBMC.
- From the seahorn tool: e.g. `--run=kinduction` or a dedicated `kinduction` frontend that builds the same pipeline as seahorn but runs KInduction instead of PathBMC.

## Files

- `KInductionEngine.hh` / `KInductionEngine.cc`: orchestration (clone, peel, run PathBMC, interpret result).
- `KInductionPass.cc`: LLVM ModulePass that runs the engine on the entry function.
