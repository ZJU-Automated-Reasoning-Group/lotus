# Interleaved-Dyck approximation datasets

The `taint/` and `valueflow/` directories are the datasets
used by **A Better Approximation for Interleaved Dyck Reachability** by Giovanna Kobus Conrado and Andreas Pavlogiannis.

Each input is a DOT graph whose edges use these labels:

- `op--N` / `cp--N`: opening/closing parenthesis of type `N`;
- `ob--N` / `cb--N`: opening/closing bracket of type `N`; and
- `normal`: an unconstrained value-flow edge.

The supplied artifact contains no license file. Keep that provenance in mind when redistributing the datasets.
