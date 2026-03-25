# Dataflow diff testing (lotus-dfa-diff)

`lotus-dfa-diff` runs overlapping dataflow analyses from lib/Dataflow on the same LLVM bitcode and dumps results in a **canonical format** so that outputs can be compared (e.g. with `diff`). This supports differential testing: generate random C, compile to bitcode, run multiple engines, and diff their results to find discrepancies.

## Engines and overlap

| Analysis        | Elimination | Mono | WPDS |
|----------------|-------------|------|------|
| Liveness       | `-elim-live` | `runLiveVariablesAnalysis` | `runLivenessAnalysis` |
| Reachable      | `-elim-reachable` | `runReachableAnalysis` | — |
| Uninit vars    | `-elim-uninit` | `runUninitVariablesAnalysis` | `runUninitializedVariablesAnalysis` |
| Reaching defs  | `-elim-rd`  | —    | —    |
| Constant prop  | `-elim-constprop` | (Inter)Mono constant prop | WPDS constant prop |

Currently only **liveness** is wired for diff (Elimination vs Mono, both intraprocedural). Other analyses can be added by extending the tool and the canonical dump format.

## Usage

```bash
# Run both engines and write elim.txt / mono.txt into OUT_DIR
lotus-dfa-diff --analysis=liveness --engine=both --out-dir=/tmp/dfa /path/to/file.bc

# Run APA with a specific elimination backend
lotus-dfa-elim --analysis=liveness --elim-method=adt-simple /path/to/file.bc

# Run only one engine (for debugging)
lotus-dfa-diff --analysis=liveness --engine=elim --out-dir=/tmp/dfa /path/to/file.bc
```

## Canonical format

Per function, one line per instruction:

```
FUNC <function_name>
 inst_<id> IN: <sorted,comma-separated value ids>
```

Value ids are stable: `arg0`, `arg1`, … for arguments, then `i0`, `i1`, … for instructions in BB order. This allows a direct `diff elim.txt mono.txt` for the same bitcode.

## Fuzz script

Use the fuzz script to generate random C and run the diff:

```bash
# From repo root
./fuzz/diff_dfa.sh              # CSmith random C → compile → diff (needs CSmith)
./fuzz/diff_dfa.sh foo.c        # compile foo.c → diff
./fuzz/diff_dfa.sh foo.bc       # diff on existing bitcode
```

Bitcode must be readable by the same LLVM version used to build lotus (e.g. LLVM 14). If your system `clang` emits opaque-pointer bitcode, set `CLANG` to the clang from that LLVM build when compiling C files.
