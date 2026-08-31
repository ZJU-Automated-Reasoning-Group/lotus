# Alias analysis tools

This directory contains command-line frontends for the alias, points-to, and
indirect-call analyses implemented in `lib/Alias/`.

## Build

Build Lotus normally to get the static alias-analysis tools:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The resulting binaries are written under `build/bin/`.

Dynamic alias-analysis tools are optional and are only built when
`LOTUS_ENABLE_DYNAA=ON`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLOTUS_ENABLE_DYNAA=ON
cmake --build build -j
```

## Input format

Most tools in this directory consume LLVM bitcode or textual LLVM IR.

```bash
clang -emit-llvm -c test.c -o test.bc
build/bin/<tool> test.bc
```

Use `build/bin/<tool> --help` to see the full option set for a specific tool.

## Tools

| Tool | Purpose | Notes |
| --- | --- | --- |
| `lotus-alias-aser-aa` | Run AserPTA pointer analysis | Implemented by `tools/alias/lotus-alias-aser-aa.cpp`; inclusion-based analysis with selectable context sensitivity (`ci`, `1-cfa`, `2-cfa`, `origin`) and solver (`basic`, `wave`, `deep`). |
| `lotus-alias-fspta` | Run exhaustive sparse flow-sensitive PTA | Builds the Lotus SVFG/MemorySSA, solves per-node memory `IN/OUT` state, and supports `--points-to-sets=mutable|hash-consed` plus memory-region partition selection. |
| `lotus-alias-sparrow-aa` | Run SparrowAA / Andersen analysis | Implemented by `tools/alias/lotus-alias-sparrow-aa.cpp`; flow-insensitive subset-based analysis with configurable call-site sensitivity via `--andersen-k-cs`. |
| `lotus-alias-lotus-aa` | Run LotusAA | Implemented by `tools/alias/lotus-alias-lotus-aa.cpp`; native Lotus interprocedural pointer analysis, with LotusAA-specific output flags such as `-lotus-print-pts` and `-lotus-print-cg`. |
| `lotus-alias-dyck-aa` | Run DyckAA | Implemented by `tools/alias/lotus-alias-dyck-aa.cpp`; unification-based analysis that can print call-graph statistics with `--print-cg`. |
| `lotus-alias-tpa` | Run TPA | Implemented by `tools/alias/lotus-alias-tpa.cpp`; semi-sparse, flow- and context-sensitive pointer analysis with optional prepass dumping and CFG `.dot` output. |
| `lotus-alias-fpa` | Run function-pointer analysis | Implemented by `tools/alias/lotus-alias-fpa.cpp`; indirect-call target analysis with FLTA, MLTA, MLTA+DF, and KELP modes. |
| `lotus-alias-call-graph` | Build a call graph with a selected backend | Implemented by `tools/alias/lotus-alias-call-graph.cpp`; supports `dyck`, `lotus`, several `fpa-*` modes, and `aserpta-*` modes. |
| `lotus-alias-sea-dsa-dg` | Dump Sea-DSA memory graphs | Implemented by `tools/alias/lotus-alias-sea-dsa-dg.cpp`; useful for inspecting per-function memory graphs and enabling graph emission with `--sea-dsa-dot`. |
| `lotus-alias-seadsa-tool` | Run extended Sea-DSA utilities | Implemented by `tools/alias/lotus-alias-seadsa-tool.cpp`; includes memory-graph dumping and other Sea-DSA related driver options. |
| `dynaa-instrument` | Instrument a program for dynamic alias logging | Built only with `LOTUS_ENABLE_DYNAA=ON`. |
| `dynaa-check` | Compare dynamic logs against a static AA | Built only with `LOTUS_ENABLE_DYNAA=ON`. |
| `dynaa-log-dump` | Decode `pts.log` files | Built only with `LOTUS_ENABLE_DYNAA=ON`. |
| `dynaa` | Dynamic alias-analysis runtime driver | Built only with `LOTUS_ENABLE_DYNAA=ON`; see `tools/alias/dynaa/README.md`. |

## Common workflows

### Compare static pointer analyses

```bash
build/bin/lotus-alias-aser-aa test.bc --analysis-mode=1-cfa --solver=wave
build/bin/lotus-alias-fspta test.bc --print-pts --dump-stats
build/bin/lotus-alias-fspta test.bc --points-to-sets=hash-consed
build/bin/lotus-alias-fspta test.bc --dump-svfg=fspta.dot --print-memory
build/bin/lotus-alias-sparrow-aa test.bc --andersen-k-cs=1 --print-pts
build/bin/lotus-alias-tpa test.bc --k-limit=1 --print-indirect-calls
```

### Inspect indirect-call targets

```bash
build/bin/lotus-alias-fpa test.bc --analysis-type=2
build/bin/lotus-alias-call-graph test.bc --cg-type=lotus --emit-cg-as-json
```

### Dump graph artifacts

```bash
build/bin/lotus-alias-tpa test.bc --cfg-dot-dir out/cfg
build/bin/lotus-alias-sea-dsa-dg test.bc --sea-dsa-dot
build/bin/lotus-alias-dyck-aa test.bc --print-cg
```

## Tool-specific notes

- `lotus-alias-aser-aa` and `lotus-alias-tpa` may consult external pointer-spec/config files. When in
  doubt, run them from the repository root so bundled config files under
  `config/` are found.
- `lotus-alias-tpa` looks for `ptr.spec` under `LOTUS_CONFIG_DIR`, then `config/ptr.spec`
  relative to the current working directory.
- `lotus-alias-lotus-aa` prints only a completion message unless LotusAA-specific flags such
  as `-lotus-print-pts` or `-lotus-print-cg` are enabled.
- `lotus-alias-call-graph` emits DOT by default and can also emit JSON with
  `--emit-cg-as-json`.
- Sea-DSA tooling depends on the Sea-DSA integration being available in the
  current build.

## Related documentation

- `lib/Alias/README.md` compares the analyses implemented in Lotus.
- `tools/alias/dynaa/README.md` documents the dynamic alias-analysis workflow.
