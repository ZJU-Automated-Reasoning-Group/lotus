# Classical CFL regression fixtures

These small inputs are consumed by the Classical CFL CTest integration cases;
they are correctness fixtures, not performance benchmarks.

- `chain.grammar` / `chain.txt`: reflexive-transitive closure across all
  relation backends.
- `call.grammar` / `call.txt`: correlated attributed call/return labels.
- `value-flow.ll`: end-to-end context-sensitive value-flow CLI query.

Run the registered cases with:

```bash
ctest --test-dir build -R classical_cfl --output-on-failure
```
