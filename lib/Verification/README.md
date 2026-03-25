# Program Verification

Lotus provides multiple verification and analysis backends based on abstract interpretation and symbolic execution.

## Analysis

Pre-verification analysis passes:

- [`Analysis/`](Analysis/) – Module analysis utilities
  - `CheckModule.cpp` – Verify module integrity
  - `ClassifyInstructions.cpp` – Instruction classification
  - `ClassifyLoops.cpp` – Loop analysis
  - `CountInstr.cpp` – Instruction counting
  - `GetTestTargets.cpp` – Test target extraction

- [`Transform/`](Transform/) – IR transformations for verification
  - Loop/control-flow transformations
  - Memory instrumentation
  - Nondeterminism injection

## Backend

Verification backends:

- **CLAM** – Abstract interpretation with numerical domains (intervals, octagons, polyhedra)
- **Sifa** – Symbolic interpretation with fluid abstractions
- **SymAbsAI** – Program-level abstract interpretation framework
- **Seahorn** – Horn clause-based verification

### CLAM

[CLAM](clam/)  provides:
- Numerical abstract domains (intervals, octagons, boxes, polyhedra)
- Property checking (null pointer, bounds, use-after-free)
- SeaDsa-based heap abstraction
- Integration with Crab domains

### Sifa

[Sifa](Sifa/) (Symbolic Interpretation with Fluid Abstractions) implements:
- ICFG interpretation with RegexDAG representation
- Fluid abstraction (via SMT-based symbolic abstraction)
- Multiple domains: Reachability, Interval, Octagon, Eq, ExplicitValue
- Region-based memory model via alias analysis

### SymAbsAI

[SymAbsAI](SymAbsAI/) is a full abstract interpretation framework:
- Fixpoint engine with fragment decomposition
- Abstract domains: Intervals, Octagons, MemRange, Congruence, etc.
- Instruction semantics to SMT conversion

### Seahorn

[Seahorn](seahorn/) provides Horn clause verification:
- BvOpSem – Bit-precise operational semantics
- ClpOpSem – Concrete memory model
- Horn clause generation and solving
- Counterexample generation

## Failure-Directed Trimming

[FailureDirectedTrimming](FailureDirectedTrimming/) implements program trimming (Ferles et al., ESEC/FSE 2017):
- Equi-safe program reduction
- Safety condition inference
- Instrumentation for path pruning

## Dependencies

- LLVM 14
- Z3 (for SMT solving)
- Boost (for CLAM/CRAB)
- SeaDsa (included)

## References

- CLAM: https://github.com/seahorn/clam
- CRAB: https://github.com/seahorn/crab
- SeaDsa: https://github.com/seahorn/sea-dsa
- Seahorn: https://github.com/seahorn/seahorn
