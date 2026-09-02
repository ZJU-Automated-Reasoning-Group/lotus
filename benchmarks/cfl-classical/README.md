# Classical CFL smoke benchmark

`chain.grammar` describes reflexive-transitive closure over `a`. Run each
backend and compare `relation_edges` and dumped triples:

```bash
for solver in sparse-set sparse-bitvector transitive-closure; do
  build/bin/lotus-cfl-classical \
    --grammar benchmarks/cfl-classical/chain.grammar \
    --graph benchmarks/cfl-classical/chain.txt \
    --solver "$solver" --json-stats
done
```

For reproducible artifacts, use `--relation-output relation.csv`,
`--stats-output stats.json`, `--json-stats`, and optionally `--start-only`.
Transitive-closure statistics include specialized relation edges, propagated
pairs, duplicates, and estimated container payload. Payload estimates exclude
allocator/container-node overhead; use RSS or allocator instrumentation for
memory comparisons.

The fixture is intentionally small enough for unit and sanitizer runs. Larger
performance experiments should record compiler mode, node/edge counts, peak
RSS, backend, and the complete grammar.

`call.grammar` and `call.txt` exercise automatic, per-kind inference of the
correlated `i` domain.

`value-flow.ll` exercises the second SVF-style CFL client through a matched
interprocedural pointer flow. It is used to check that all three
storage/specialization backends answer `main::seed -> main::result`.

## External SVF comparison

Use `scripts/cfl/compare_svf_cfl.py` with an independently built SVF oracle.
Write its JSON output under `build/`, `/tmp`, or a CI artifact directory; parity
reports are generated data and are intentionally not stored in this benchmark
directory.

Node and edge counts should not be asserted equal because Lotus and SVF use different object and field abstractions. Annotation outcomes are the semantic comparison boundary.
