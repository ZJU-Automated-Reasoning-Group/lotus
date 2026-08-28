# Graph Simplification for Interleaved Dyck-Reachability

This repository contains an implementation of the paper
- Yuanbo Li, Qirun Zhang, Thomas Reps. Fast Graph Simplification for Interleaved Dyck-Reachability. In *PLDI 2020*.
## Interleaved Dyck language
The implementation currently supports the interleaved Dyck language of two Dyck languages representing brackets and parentheses, respectively.

## Input format
The implementation accepts an input graph in the dot format. A labeled edge is encoded as
```
0->1[label="ob--1"]
```
 - 0 and 1 are the vertex ids;
 - "ob" means an open bracket. Similarly, we have "cb" for a close bracket, "op" for an open parenthesis, and "cp" for a close parenthesis;
 - "--1" means the id of the bracket/parenthesis is 1.

## Usage
Copy your dot file and name it as ``current.dot`` in the directory

run ./graph_reduce.sh

## Example
``example/example.dot`` contains the motivation example (Figure 1b) in the PLDI 2020 paper.
```
cp example/example.dot current.dot
./graph_reduce.sh
```

The resulting graph is in ``current.dot``.

## Relationship to other Lotus CFL components

This module is one of several related implementations for interleaved-Dyck
reachability in Lotus:

- [`MCFL`](../MCFL/README.md) provides a general multiple-context-free grammar
  solver and dimension-indexed, witness-producing underapproximations.
- [`MutualRefinement`](../MutualRefinement/README.md) refines multiple
  context-free projections of the same graph problem.
- [`InterleavedDyck`](../InterleavedDyck/README.md) builds a staged analysis on
  top of projected CFL solving and `MutualRefinement`.

`InterDyckGraphReduce` is currently standalone and uses the specialized public
types under `include/CFL/InterDyckGraphReduce/`. It is conceptually suitable as
a preprocessing stage for the other analyses, but no graph adapter or
preservation contract currently connects them. Keeping the implementations
separate avoids implying that a reduction proven for one reachability model is
automatically valid for every MCFL grammar or refinement stage.
