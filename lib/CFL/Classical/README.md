# Classical CFL Reachability

The implementation mirrors `include/CFL/Classical`:

- `Core/` contains solver-independent grammars, graphs, relations, validation,
  and recursive-state-machine support.
- `Solvers/Engines/` contains solving algorithms: the classical transitive
  closure, PEARL, POCR/FOCR, Sqid, and Stg.
- `Solvers/Preprocessing/` contains graph simplification and RSM foldability.
- `Clients/Alias/` and `Clients/ValueFlow/` are the two analysis clients.

Algorithms belong under `Solvers/Engines`; POCR, PEARL, Sqid, and Stg are
engines, not clients.
