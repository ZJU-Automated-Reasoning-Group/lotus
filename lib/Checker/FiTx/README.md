# FiTx — Typestate-Based Bug Checker Engine

FiTx is a **typestate analysis framework** for daily development–friendly bug detection (e.g. use-after-free, leaks, double-lock). It is based on the design in Suzuki et al., *"Balancing Analysis Time and Bug Detection: Daily Development-friendly Bug Detection in Linux"*, USENIX ATC 2024.

## Architecture

```
LLVM Module
    ↓ IRGenerator (FunctionPass)
fitx::Function (Core IR: blocks, instructions, ordered blocks)
    ↓ FrameworkPass (ModulePass)
defineStates() → StateManagers (one per checker)
    ↓ Analyzer::analyze()
CFG traversal, typestate propagation, return-code aware summaries
    ↓ BugNotificationTiming (IMMEDIATE, END_OF_LIFE, FUNCTION_END, MODULE_END)
Bug reports
```

- **Core** (`Core/`): Value, Instruction, BasicBlock, Function (framework IR); ValueCollection, AliasValues (may-alias, store-based, intra-procedural); ValueTypeAlias (instruction-level).
- **Framework_IR** (`Framework_IR/`): Builds framework IR from LLVM (IRGenerator, Analyzer); runs before FrameworkPass.
- **Frontend** (`Frontend/`): State, StateTransition (FSM and transition rules); Analyzer (CFG traversal, store/load/call/branch, alias); BasicBlockInformation, FunctionInformation (per-block and per-function state).
- **Detector** (`Detector/`): Each checker (UAF, Leak, Double_lock, etc.) subclasses FrameworkPass, overrides `defineStates()`, and registers a StateManager with states and transitions (paper Table 5: Fun Arg, Store, Use, Alias).

## Typestate and Transitions (Paper §4.1, Table 5)

- **States**: INIT, NORMAL, BUG; merge method (STRICT/FLEX); bug notification timing (IMMEDIATE, END_OF_LIFE, FUNCTION_END, MODULE_END).
- **Transition triggers**: FUNCTION_ARG (call F with arg i), STORE_VALUE (NULL/NON/ANY/CALL_FUNC), USE_VALUE (load), ALIASED_VALUE (store-based may-alias propagation).
- **Aliasing**: May-alias only; recorded on stores (`ptr = value_operand`), used when applying store/alias transitions; not merged across CFG predecessors. Related values (same base LLVM value + field path) are tracked via ValueCollection::getRelatedValues().

## Adding a New Checker

1. Create a detector under `Detector/YourDetector/` (e.g. `Your_Detector.cpp`, `YourUtils.cpp`).
2. In `defineStates()`: create StateManager, define states (init, normal, bug), add transitions (FunctionArgTransitionRule, StoreValueTransitionRule, UseValueTransitionRule, AliasValueTransitionRule as needed).
3. Register the detector in `FrameworkPass::passes` (see e.g. `UAF_Detector.cpp`).

## Build and Tests

From the project root:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
# Run tests
ctest --output-on-failure
```

FiTx is built as part of the Lotus checker tools; see the main AGENTS.md and docs for LLVM path and options.
