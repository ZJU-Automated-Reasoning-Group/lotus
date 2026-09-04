# CFL Library

Lotus groups its CFL-related implementations by problem family:

| Subdirectory | Purpose |
|---|---|
| [`Classical`](Classical/README.md) | Grammar-driven classical CFL reachability, solver engines, preprocessing, and analysis clients |
| [`CSIndex`](CSIndex/README.md) | Extended-Dyck indexing for context-sensitive reachability |
| [`InterleavedDyck`](InterleavedDyck/README.md) | Shared representations, exact special cases, bounds, underapproximations, refinement, and graph reduction for interleaved-Dyck reachability |

The public header tree under `include/CFL` mirrors this organization. All
interleaved-Dyck APIs live below `include/CFL/InterleavedDyck`.

## Command-line tools

Classical tools retain the `lotus-cfl-*` names documented by the Classical
module. Interleaved-Dyck tools consistently use the
`lotus-cfl-interleaved-dyck-*` prefix:

| Tool | Purpose |
|---|---|
| `lotus-cfl-interleaved-dyck-unary` | Run the adaptive or fixed-counter exact unary analysis |
| `lotus-cfl-interleaved-dyck-staged-bounds` | Compute staged lower and upper bounds |
| `lotus-cfl-interleaved-dyck-mcfl` | Run the dimension-indexed MCFL underapproximation hierarchy |
| `lotus-cfl-interleaved-dyck-mutual-refinement` | Run the file-driven CNF refinement experiment |
| `lotus-cfl-interleaved-dyck-graph-reduction.py` | Orchestrate the graph-reduction helpers |

There are no compatibility headers, CMake target aliases, or legacy command
names for the former flat layout.
