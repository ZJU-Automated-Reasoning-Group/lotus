# Classical CFL smoke benchmark

`chain.grammar` describes reflexive-transitive closure over `a`. Run each
backend and compare `relation_edges` and dumped triples:

```bash
for solver in baseline pocr hybrid; do
  build/bin/lotus-cfl-classical \
    --grammar benchmarks/cfl-classical/chain.grammar \
    --graph benchmarks/cfl-classical/chain.txt \
    --solver "$solver" --json-stats
done
```

The fixture is intentionally small enough for unit and sanitizer runs. Larger
performance experiments should record compiler mode, node/edge counts, peak
RSS, backend, and the complete grammar.
