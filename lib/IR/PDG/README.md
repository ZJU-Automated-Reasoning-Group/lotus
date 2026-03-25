# PDG: Program Dependence Graph

The **Program Dependence Graph (PDG)** is a fine-grained representation of data
and control dependences. It is built on top of the ICFG and is used for
slicing, security analyses, and other dependence-aware queries.

The PDG is field-sensitive and flow-insensitive. The query layer now exposes
both context-insensitive and call/return-matched context-sensitive traversals
through a single API in ``include/IR/PDG/Analysis/PDGQuery.h``.

## Key Features

- **Data Dependencies**: Def-use chains, read-after-write (RAW), and alias-based memory dependencies
- **Control Dependencies**: Execution order and branch condition dependencies
- **Interprocedural**: Tracks dependencies across function boundaries
- **Field-Sensitive**: Handles structure fields and array elements separately
- **Parameter Trees**: Tree structures for field-sensitive parameter analysis

## Query Services

- **`SliceQuery`**: forward slices, backward slices, chops, and thin slices
- **`DependenceQuery`**: reachability, shortest paths, all shortest paths, and
  distance queries
- **`DataFlowQuery`**: reaching definitions, def-use/use-def chains, liveness,
  and control-region queries
- **`TransformQuery`**: motion legality and dependence-aware scheduling helpers
- **`DiffQuery`**: structural PDG differencing and impact summaries
- **`PDGCriteriaResolver`**: criteria resolution from nodes, LLVM values,
  function names, callee names, source locations, property specs, and Cypher
  selections

Each service consumes a common option/result vocabulary:

- ``PDGQueryOptions``: edge preset, scope, context mode, limits, cache policy,
  and explanation mode
- ``PDGQueryScope``: whole graph, explicit node set, function, or prior result
- ``PDGQueryResult``: nodes, induced edges, predecessor map, witness paths,
  distances, and diagnostics

## Usage

```cpp
#include "IR/PDG/Analysis/PDGQuery.h"
#include "IR/PDG/Core/ProgramDependencyGraph.h"

// Run PDG passes
legacy::PassManager PM;
PM.add(new DataDependencyGraph());
PM.add(new ControlDependencyGraph());
auto *pdgPass = new ProgramDependencyGraph();
PM.add(pdgPass);
PM.run(module);

ProgramGraph *G = pdgPass->getPDG();
pdg::SliceQuery slicer(*G);

pdg::PDGCriteria criteria;
criteria.values.push_back(src);

pdg::PDGQueryOptions options;
options.edge_preset = pdg::PDGEdgePreset::Data;
options.context_mode = pdg::PDGContextMode::ContextSensitive;

auto result = slicer.forward(criteria, options, &module);
if (!result.nodes.empty()) {
  // Criteria reaches at least one node through the selected dependence edges.
}
```

## Edge Presets

The public query API uses fixed edge presets instead of raw edge-type sets:

- ``All``
- ``Data``
- ``Control``
- ``Parameter``
- ``Interprocedural``
- ``ValueFlow``
- ``TransformLegality``

Thin slicing is selected with ``options.slice_flavor = SliceFlavor::Thin``.

## Alias Analysis Selection

PDG data-dependence construction supports alias analysis selection:

- `-pdg-aa=<type>`: Selects over-approximate AA (default: `andersen`)
- `-pdg-aa-under=<type>`: Optionally enables under-approximate AA (default: `underapprox`)

Supported AA types: `andersen`, `andersen-1cfa`, `andersen-2cfa`, `dyck`, `cfl-anders`, `cfl-steens`, `combined`, `underapprox`

## Available Passes

- `-pdg`: Generate the program dependence graph (inter-procedural)
- `-cdg`: Generate the control dependence graph (intra-procedural)
- `-ddg`: Generate the data dependence graph (intra-procedural)
- `-dot-*`: Visualization passes (dot format)

## `pdg-query`

``tools/ir/pdg-query.cpp`` supports both raw Cypher queries and analysis mode.

Examples:

- ``pdg-query --analysis slice-backward --criteria-query "MATCH (n:INST_RET) RETURN n" foo.bc``
- ``pdg-query --analysis chop --criteria-query "MATCH (a) WHERE a.func = 'main' RETURN a" --target-query "MATCH (b:INST_RET) RETURN b" --edge-preset value-flow foo.bc``
- ``pdg-query --analysis diff --criteria-query "MATCH (n) WHERE n.func = 'old_f' RETURN n" --target-query "MATCH (n) WHERE n.func = 'new_f' RETURN n" --format json foo.bc``
- ``pdg-query --property-file spec.prp --direction backward --context-sensitive foo.bc``

Analysis mode accepts ``--scope-function``, ``--scope-query``,
``--context-sensitive``, ``--thin``, and ``--format text|json|dot``.

## See Also

- Headers: `include/IR/PDG/`
- Documentation: `docs/source/ir/pdg.rst`
- Query API: `include/IR/PDG/Analysis/PDGQuery.h`
