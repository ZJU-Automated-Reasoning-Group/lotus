# Third-party libraries

Vendored or in-tree copies of external dependencies used by this project.

| Library | Directory | Description | Upstream |
|--------|-----------|-------------|----------|
| **CUDD** | `CUDD/` | CU Decision Diagram package — BDD/ADD/ZDD manipulation | [ivmai/cudd](https://github.com/ivmai/cudd) (mirror); original by Fabio Somenzi, University of Colorado |
| **WPDS** | `WPDS/` | Weighted pushdown system library (WALi-style) for interprocedural dataflow | Wisconsin/GrammaTech WALi lineage; see e.g. [WALi-OpenNWA](https://github.com/WaliDev/WALi-OpenNWA) |
| **WALi/OpenNWA** | `WALi-OpenNWA/` | Full WALi weighted automata library and OpenNWA nested-word automata implementation | [WaliDev/WALi-OpenNWA](https://github.com/WaliDev/WALi-OpenNWA) |
| **spdlog** | `spdlog/` | Fast C++ logging library (header-only) | [gabime/spdlog](https://github.com/gabime/spdlog) |
| **CRAB** | `crab/` | Abstract interpretation library used by the vendored CLAM backend | [seahorn/crab](https://github.com/seahorn/crab) |
| **Verification backends** | `verification/` | Upstream-derived CLAM, SeaHorn, and SMACK sources | See `verification/README.md` |
| **FPsolve** | `fpsolve/` | Fixed-point solver based on Newton's method over omega-continuous semirings | TUM FPsolve project; see the vendored `fpsolve/README.md` |
| **Horn-ICE** | `horn-ice/` | Horn-ICE invariant/contract synthesis components and CHC verifier frontend | Adapted from [horn-ice/hice-dt](https://github.com/horn-ice/hice-dt) |
| **MDE** | `mde/` | Multilevel Deduplication Engine for compact data representation and cached set operations | Vendored MDE source; see `mde/README.md` |
| **Seal** | `seal/` | Symbolic automata tooling for stateful systems, including LLVM transforms and Z3 support code | Academic Seal artifact; see `seal/README.md` |
| **Stingx toolchain** | `stingx_toolchain/` | Local copy of the Stingx backend used by experimental invariant-generation scripts | Vendored Stingx backend source |

## Usage

- **Include path**: The project adds `third-party/` to the global include path. Use `#include <spdlog/spdlog.h>`, `#include "CUDD/cudd.h"`, and `#include "WPDS/..."` as in the rest of the codebase.
- **CMake**: CUDD, WPDS, and spdlog are built via `third-party/CMakeLists.txt`; link targets `CanaryCUDD`, `wpds`, `wpds++` (and interfaces `ewpds`, `wpdsplusplus_util`) as needed. WALi/OpenNWA is opt-in with `-DLOTUS_ENABLE_WALI_OPENNWA=ON` and exposes `WALi::wali`. FPsolve, Horn-ICE, and Seal are opt-in with `-DLOTUS_ENABLE_FPSOLVE=ON`, `-DLOTUS_ENABLE_HORN_ICE=ON`, and `-DLOTUS_ENABLE_SEAL=ON`. MDE and the Stingx toolchain are vendored for local use but are not added by the top-level third-party CMake file by default. CRAB is configured from `cmake/ConfigureClamCrab.cmake` and is discovered from `third-party/crab/` by default.

## Updating

When updating a vendored copy, preserve the layout and include paths above so existing `#include` directives and CMake targets continue to work.
