# LotusAA: Native Lotus Alias Analysis Engine

LotusAA is the **native alias analysis framework** of Lotus. It provides a modular engine with **interprocedural**, **flow-sensitive**, and **field-sensitive** reasoning, designed to integrate tightly with other Lotus analyses.

## Key Features

- **Flow-sensitivity**: Respects program order and control flow
- **Context-sensitivity**: Function summaries provide calling context
- **Field-sensitivity**: Tracks memory objects at the field/element level
- **On-the-fly call graph construction**: Alternates between pointer analysis and call graph refinement
- **Points-to graph representation**: Nodes represent memory objects and SSA values; edges represent points-to, load, store, and field relations
- **Staged strong updates**: Tuna-style must-kill forests prune overwritten
  stores before guarded heap histories are expanded

## Architecture

```
Module → Global Initialization → Bottom-Up Function Analysis → Call Graph Construction → Fixpoint Iteration → Points-to Graph
```

The analysis alternates between:
1. Analyzing functions bottom-up using current call graph
2. Resolving indirect calls using pointer analysis results
3. Updating call graph with newly discovered edges
4. Reanalyzing affected functions
5. Repeating until fixpoint

## Directory Structure

- **`Engine/`**: Analysis engines
  - `InterProceduralPass`: Top-level LLVM ModulePass (`LotusAA`)
  - `IntraProceduralAnalysis`: Per-function analysis (`IntraLotusAA`)
  - `TransferFunctions/`: Instruction transfer functions
    - `BasicOps`: Alloca, Arguments, Globals, Constants
    - `PointerInstructions`: Load, Store, PHI, Select, GEP, Casts
    - `CallHandling`: Function calls and summary application
    - `CallGraphSolver`: Indirect call resolution
    - `SummaryBuilder`: Function summary collection

- **`MemoryModel/`**: Points-to graph and memory modeling
  - `PointsToGraph`: Base graph representation
  - `MemObject`: Memory object representation
  - `ObjectLocator`: Memory object location tracking

- **`Support/`**: Configuration and utilities
  - `CallGraphState`: Call graph state management
  - `FunctionPointerResults`: Indirect call target tracking
  - `LotusConfig`: Configuration parameters

## Usage

LotusAA is typically not run as a standalone tool. Instead, it is selected via configuration:

- Clam / Lotus front-ends can choose LotusAA as the primary AA engine
- YAML configurations and command-line flags control whether LotusAA is enabled
- When enabled, LotusAA registers itself with the AA wrapper so that all AA queries go through its results

### Standalone Tool

```bash
lotus-alias-lotus-aa [options] <input bitcode file>
```

### Configuration Options

- `lotus_restrict_inline_depth`: Max inter-procedural inlining depth (default: unbounded via Falcon-compatible sentinel `-2`)
- `lotus_restrict_cg_size`: Max indirect call targets (default: 5)
- `lotus_restrict_inline_size`: Max summary size (default: 100)
- `lotus_restrict_ap_level`: Max access path depth (default: 2)
- `lotus-enable-must-kill`: Enable incremental must-kill load/store matching
  (default: true)

## Path-Sensitive Strong Updates

LotusAA implements the staged load/store matching algorithm from *Efficient
Strong Updates for Path Sensitive Data Dependence Analysis* (Guo and Zhang,
ICSE 2026):

1. `getAliasCondition` directly intersects guarded points-to targets to obtain
   the may-alias condition of a load/store pair.
2. `areMustAliases` fingerprints canonical guarded points-to sets and confirms
   hash matches structurally, avoiding collision-based unsoundness.
3. Each load reuses the kill forest of its immediate dominating must-alias
   load (its anchor) and considers only stores between the anchor and itself.
4. A store kills an older store when their pointers must alias and removing
   the newer store disconnects every older-store-to-load CFG path.
5. Only forest roots are expanded by the existing guarded heap walker. This
   retains LotusAA's summary, undef, and confidence handling while avoiding
   conditions for stores already proven dead.

The optimization follows LotusAA's existing treatment of cyclic CFG regions:
only instructions numbered by the framework's acyclic topological traversal
participate in a must-kill forest.

## Analysis Characteristics

| Characteristic | Value |
|----------------|-------|
| **Analysis Type** | Inclusion-based |
| **Flow-Sensitive** | ✅ Yes |
| **Context-Sensitive** | ✅ Yes (function summaries) |
| **Field-Sensitive** | ✅ Yes |
| **Representation** | Points-to graph |

## See Also

- Parent README: `lib/Alias/README.md`
- Documentation: `docs/source/alias/lotusaa.rst`
