# Interleaved-Dyck approximation

This directory contains a native C++17 implementation of the staged algorithm
from the artifact **A Better Approximation for Interleaved Dyck Reachability**
by Giovanna Kobus Conrado and Andreas Pavlogiannis.

The port retains the artifact's main stages:

1. regular-language filtering through a graph/automaton product;
2. intersection of independently witnessed parenthesis- and bracket-Dyck
   reachability;
3. a sound Dyck-over-the-union underapproximation;
4. edge-tracing mutual refinement;
5. the stronger parity/endpoint grammar; and
6. pairwise on-demand refinement.

The implementation reuses Lotus's `CFL/MutualRefinement` CNF saturation and
derivation-tracing engine. Public graph, DOT parser, individual approximation,
and full-pipeline APIs are declared in
`include/CFL/InterleavedDyck/InterleavedDyck.h`.

The reference artifact did not include a license file in the supplied source
tree. This port is therefore an independent C++ reimplementation based on the
published algorithm and observable artifact behavior, rather than a verbatim
copy of its Go/Python sources. Its DOT benchmark inputs are stored under
`benchmarks/interleaved-dyck-approximation/` with provenance noted there.
