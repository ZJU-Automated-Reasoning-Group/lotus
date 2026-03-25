# GSA: Gated SSA

`gsa::` provides a read-only Gated SSA view over LLVM IR. The analysis builds
immutable gate trees for reachable PHI nodes and does not mutate the module.
Lowering those trees into `select` chains is handled by a separate
materialization pass.

## Key Concepts

- **Gate nodes**: One immutable `GateNode` per tracked PHI, classified as
  `Gamma`, `Mu`, or `Eta`.
- **Gate expressions**: Tree nodes of kind `Bottom`, `LeafValue`, `Select`, or
  `Switch`.
- **Guards**: Explicit edge/arm descriptors (`BranchTrue`, `SwitchCase`,
  `InvokeNormal`, `Opaque`, etc.) attached to expressions.
- **Bottom**: Internal sentinel for unavailable flow; analysis results never
  encode bottom as an LLVM `Value *`. The materializer lowers it to `poison`.
- **Opaque control flow**: Unsupported terminators are represented explicitly in
  the analysis and mark the gate as non-lowerable instead of collapsing it.

## Components

- `ControlDependenceAnalysis.cpp`: Block-level control dependence plus
  reachability/tracked-block queries.
- `GateAnalysis.cpp`: Builds immutable per-function GSA data without creating
  instructions.
- `GsaMaterialization.cpp`: Lowers lowerable GSA expressions to `select` chains
  and optionally replaces the source PHIs.

## Public API

```cpp
#include "IR/GSA/GSA.h"

auto *cda = createControlDependenceAnalysisPass();
auto *ga = createGateAnalysisPass();
auto *gm = createGsaMaterializationPass();

cda->runOnModule(M);
ga->runOnModule(M);

gsa::GateAnalysis &analysis = ga->getGateAnalysis(F);
for (const gsa::GateNode *gate : analysis.gates()) {
  if (!gate->isLowerable())
    continue;
  const gsa::GateExpr *root = gate->getRootExpr();
  (void)root;
}

// Optional, mutating lowering stage.
gm->runOnModule(M);
```

## Command-Line Options

- `-gsa-thinned`: Build thinned GSA (`true` by default).
- `-gsa-replace-phis`: Only affects `GsaMaterializationPass`; when enabled, the
  pass replaces lowerable PHIs with the materialized values.

## Semantics

- Only PHIs in blocks reachable from function entry are tracked.
- `ControlDependenceAnalysis::getCDBlocks()` returns an empty list for
  untracked/unreachable blocks.
- `GateNode::isLowerable()` is `false` when materialization would require
  non-scalar or unsupported guards, such as exception edges or opaque branch
  targets.
- The analysis preserves deterministic function order for `gates()` and
  deterministic arm ordering for `Switch` expressions.
