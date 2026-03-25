# Abstract Execution (AE) Engine

## Overview

The Abstract Execution (AE) engine is a static analysis tool that detects memory safety bugs in C/C++ programs through abstract interpretation. It was migrated from SVF's AE implementation and adapted to work with Lotus's infrastructure.

SVF-faithful parity in Lotus targets the original AE core: buffer overflow and
null pointer dereference detection over the migrated abstract-execution engine.
`UseAfterFreeDetector`, `InvalidFreeDetector`, and `MemLeakDetector` are
Lotus-specific extensions layered on top of that migrated core.

**Based on the paper:**
*"Precise Sparse Abstract Execution via Cross-Domain Interaction"*
Xiao Cheng, Jiawei Wang, Yulei Sui. ICSE 2024.

## Key Features

- **Sparse Analysis**: Only tracks values relevant to bug detection, reducing analysis overhead
- **Cross-Domain Interaction**: Uses Z3 SMT solver to refine interval domains via constraint solving
- **Field-Sensitive**: Tracks individual struct fields for precise heap modeling
- **Flow-Sensitive**: Maintains separate abstract states at each program point
- **Context-Sensitive**: Handles function calls with calling context

## Supported Bug Types

| Bug Type | Detector | Description | Default |
|----------|----------|-------------|---------|
| Buffer Overflow | `BufOverflowDetector` | Detects out-of-bounds array/pointer accesses | Enabled |
| Null Pointer Dereference | `NullptrDerefDetector` | Detects dereferences of null pointers | Enabled |
| Use-After-Free | `UseAfterFreeDetector` | Detects accesses to freed memory | Enabled |
| Invalid Free | `InvalidFreeDetector` | Detects double-free and free of non-heap objects | Enabled |
| Memory Leak | `MemLeakDetector` | Detects allocated memory that is not freed and not reachable | **Disabled** |

## Architecture

### Core Components

```
AbstractInterpretation (main engine)
├── AbstractState (program state at each point)
│   ├── VarToAbsValMap (SSA values → abstract values)
│   └── AddrToAbsValMap (memory objects → abstract values)
├── AbstractValue (union of interval and address)
│   ├── IntervalValue (numeric ranges: [lb, ub])
│   └── AddressValue (sets of memory object IDs)
├── ICFGWTO (Weak Topological Order for loops)
├── RelationSolver (Z3-based constraint refinement)
├── SVFIRWrapper (pointer analysis interface)
└── AEDetector (bug detection)
    ├── BufOverflowDetector
    ├── NullptrDerefDetector
    ├── UseAfterFreeDetector
    ├── InvalidFreeDetector
    └── MemLeakDetector (disabled by default)
```

### Abstract Domains

#### Interval Domain
Represents numeric values as ranges:
- `[5, 5]` - constant 5
- `[0, 100]` - any value between 0 and 100
- `[-∞, +∞]` - any integer (⊤, top)
- `⊥` (bottom) - unreachable/infeasible

Operations: `+`, `-`, `*`, `/`, `join`, `meet`, `widen`, `narrow`

#### Address Domain
Represents pointer values as sets of memory object IDs:
- `{obj_1}` - points to object 1
- `{obj_1, obj_2}` - may point to object 1 or 2
- `{0}` - null pointer
- `∅` - no valid addresses (⊥, bottom)

Operations: `union`, `intersection`, `subset`

### Analysis Algorithm

1. **Initialization**
   - Run pointer analysis (AserPTA) to get points-to information
   - Build call graph and detect recursive SCCs
   - Construct WTO for each function (handles loops)

2. **Abstract Interpretation**
   ```
   for each function in topological order:
       for each WTO component:
           if singleton block:
               process instructions sequentially
           if cycle (loop):
               apply widening/narrowing iteration
   ```

3. **Widening/Narrowing**
   - **Widening**: Accelerates convergence by extrapolating trends
     - Example: `[0,10] ⊔ [0,20]` → `[0,+∞]`
   - **Narrowing**: Refines over-approximations after fixpoint
     - Example: `[0,+∞] ⊓ [0,100]` → `[0,100]`

4. **Cross-Domain Interaction**
   - Collect path constraints (e.g., `x < 10` from branch conditions)
   - Encode as Z3 formulas
   - Solve to refine interval bounds
   - Example: `x ∈ [-∞,+∞]` + constraint `x < 10` → `x ∈ [-∞,9]`

5. **Bug Detection**
   - At each program point, run detectors on abstract state
   - Check for violations (e.g., offset ≥ object size)
   - Report bugs with source location and trace

## Usage Example

```cpp
#include "Checker/AE/AbstractInterpretation.h"
#include "Checker/AE/AEDetector.h"

// Get the singleton AE instance
AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();

// Configure analysis
ae.setRecursionMode(AbstractInterpretation::WIDEN_NARROW);
ae.setWidenDelay(2);

// Add detectors
ae.addDetector(std::make_unique<BufOverflowDetector>());
ae.addDetector(std::make_unique<NullptrDerefDetector>());
ae.addDetector(std::make_unique<UseAfterFreeDetector>());
ae.addDetector(std::make_unique<InvalidFreeDetector>());

// Memory leak detection (disabled by default - enable explicitly)
ae.setEnableMemLeakCheck(true);
ae.addDetector(std::make_unique<MemLeakDetector>());

// Run analysis
ae.runOnModule(module);
```

## Configuration Options

### Recursion Handling

- **TOP** (fastest, least precise)
  - Sets all recursive function results to ⊤ immediately
  - Use for quick analysis or when recursion is rare

- **WIDEN_ONLY** (moderate)
  - Applies widening only for recursive functions
  - Balances precision and performance

- **WIDEN_NARROW** (default, most precise)
  - Applies widening then narrowing
  - Best precision but slower for deep recursion

### Widening Delay

Controls how many iterations to perform before applying widening:
- `0` - Apply widening immediately (fastest convergence)
- `3` - Default, matches SVF's AE widening delay
- Higher values increase precision but may slow convergence

## Implementation Details

### SVFIR Decoupling

The original SVF AE used SVFIR (SVF's intermediate representation) for program analysis. The Lotus migration replaces this with:

| SVF Component | Lotus Replacement |
|---------------|-------------------|
| `SVFIR` | `SVFIRWrapper` (thin wrapper over AserPTA) |
| `SVFStmt` | LLVM `Instruction` |
| `SVFVar` | LLVM `Value` with ID mapping |
| `PAG` (Pointer Assignment Graph) | AserPTA's pointer analysis |
| `CallGraph` | AserPTA's call graph |

### Memory Model

- **Virtual Addresses**: Memory objects are encoded as virtual addresses
  - Format: `0x7f000000 + objID`
  - Special IDs: `0` (null), `1` (black-hole)

- **Field Sensitivity**: GEP offsets are tracked per-object
  - `obj[0]`, `obj[4]`, `obj[8]` are separate abstract locations
  - Field limit: 10000 (configurable via `MaxFieldLimit`)

### Transfer Functions

Each LLVM instruction has a corresponding transfer function:

| Instruction | Transfer Function | Description |
|-------------|-------------------|-------------|
| `alloca` | `updateStateOnAddr` | Allocate stack object |
| `load` | `updateStateOnLoad` | Read from memory |
| `store` | `updateStateOnStore` | Write to memory |
| `getelementptr` | `updateStateOnGep` | Compute pointer offset |
| `add/sub/mul/div` | `updateStateOnBinary` | Arithmetic operations |
| `icmp/fcmp` | `updateStateOnCmp` | Comparison operations |
| `call` | `updateStateOnCall` | Function call |
| `ret` | `updateStateOnRet` | Function return |
| `phi` | `updateStateOnPhi` | SSA phi node |

## Testing

### Checkpoints

The AE engine supports checkpoint-based testing via stub functions:

```c
// Expect safe buffer access
void SAFE_BUFACCESS(void* data, int size);

// Expect buffer overflow
void UNSAFE_BUFACCESS(void* data, int size);

// Example usage
int arr[10];
SAFE_BUFACCESS(arr, 5);    // Should pass
UNSAFE_BUFACCESS(arr, 15); // Should detect overflow
```

### Test Cases

See `tests/AE/` for example test cases covering:
- Buffer overflows (stack, heap, global)
- Null pointer dereferences
- Use-after-free
- Invalid free (double-free, free of stack)

## Performance Considerations

### Scalability

- **Sparse Analysis**: Only tracks values used in bug-relevant operations
- **Demand-Driven**: Pointer analysis runs on-demand
- **Widening**: Ensures termination even for unbounded loops

### Optimization Tips

1. **Reduce Widening Delay**: Set to 0 for faster analysis
2. **Use TOP Mode**: For recursive-heavy code
3. **Limit Field Sensitivity**: Reduce `MaxFieldLimit` if analysis is slow
4. **Disable Unused Detectors**: Only enable detectors you need
5. **Memory Leak Detection**: Disabled by default due to higher false positive rate

## Detector Details

### Memory Leak Detection

**Status**: Disabled by default (enable with `setEnableMemLeakCheck(true)`)

The memory leak detector identifies allocated memory that is:
1. Not freed before function return
2. Not reachable from any live pointer
3. Not escaped (passed to external functions or stored in globals)

**Detection Strategy**:
- Tracks allocations: `malloc`, `calloc`, `realloc`, `new`, `new[]`, `strdup`
- Tracks deallocations: `free`, `delete`, `delete[]`
- Checks reachability at function returns and when last reference is overwritten
- Marks objects as "escaped" if passed to external functions (conservative)

**Example**:
```c
void leak_example() {
    int *p = malloc(100);  // Allocation tracked
    p = NULL;              // Last reference lost → LEAK DETECTED
}

void no_leak_escaped() {
    int *p = malloc(100);
    external_func(p);      // Object escapes → NO LEAK (conservative)
}

void no_leak_freed() {
    int *p = malloc(100);
    free(p);               // Properly freed → NO LEAK
}
```

**Limitations**:
- May miss leaks if object escapes through complex paths
- May report false positives for intentional long-lived allocations
- Does not track inter-procedural ownership transfer precisely

**Why disabled by default**:
Memory leak detection has a higher false positive rate than other detectors because:
1. Legitimate long-lived allocations may be flagged
2. Conservative escape analysis may miss some leaks
3. Inter-procedural analysis is limited

Enable only when specifically looking for memory leaks, and manually review results.

## Limitations

1. **Soundness**: Over-approximation may miss bugs (false negatives)
2. **Precision**: May report infeasible bugs (false positives)
3. **Scalability**: Large programs with complex loops may be slow
4. **External Functions**: Limited modeling of library functions

## Future Work

- [x] Add memory leak detection (disabled by default)
- [ ] Add integer overflow detection
- [ ] Improve external function modeling
- [ ] Add path-sensitive analysis
- [ ] Parallelize analysis across functions
- [ ] Improve inter-procedural leak tracking

## References

1. Xiao Cheng, Jiawei Wang, Yulei Sui. "Precise Sparse Abstract Execution via Cross-Domain Interaction." ICSE 2024.
2. Patrick Cousot, Radhia Cousot. "Abstract Interpretation: A Unified Lattice Model for Static Analysis of Programs by Construction or Approximation of Fixpoints." POPL 1977.
3. François Bourdoncle. "Efficient Chaotic Iteration Strategies with Widenings." FMPA 1993.

## Contact

For questions or issues, please refer to the main Lotus documentation.
