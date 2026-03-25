# Third-party libraries

Vendored or in-tree copies of external dependencies used by this project.

| Library | Directory | Description | Upstream |
|--------|-----------|-------------|----------|
| **CUDD** | `Solvers/CUDD/` | CU Decision Diagram package — BDD/ADD/ZDD manipulation | [ivmai/cudd](https://github.com/ivmai/cudd) (mirror); original by Fabio Somenzi, University of Colorado |
| **WPDS** | `Solvers/WPDS/` | Weighted pushdown system library (WALi-style) for interprocedural dataflow | Wisconsin/GrammaTech WALi lineage; see e.g. [WALi-OpenNWA](https://github.com/WaliDev/WALi-OpenNWA) |
| **spdlog** | `spdlog/` | Fast C++ logging library (header-only) | [gabime/spdlog](https://github.com/gabime/spdlog) |

## Usage

- **Include path**: The project adds `third-party/` to the global include path. Use `#include <spdlog/spdlog.h>`, `#include "Solvers/CUDD/cudd.h"`, and `#include "Solvers/WPDS/..."` as in the rest of the codebase.
- **CMake**: CUDD and WPDS are built via `third-party/CMakeLists.txt`; link targets `CanaryCUDD`, `wpds`, `wpds++` (and interfaces `ewpds`, `wpdsplusplus_util`) as needed. spdlog is header-only; optional target `spdlog::spdlog` exposes include directories.

## Updating

When updating a vendored copy, preserve the layout and include paths above so existing `#include` directives and CMake targets continue to work.
