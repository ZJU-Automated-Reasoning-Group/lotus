# Path Expressions

Path expressions over labeled graphs: compute a **regular expression** that describes all paths between two nodes. Migrated from Ultimate's Library-PathExpressions (originally from [PathExpression](https://github.com/johspaeth/PathExpression)); algorithm from Tarjan, "Fast Algorithms for Solving Path Problems", 1981, Chapter 2.

## Components

- **ILabeledGraph<N, L>** – Directed graph with nodes `N` and edge labels `L`. Faithfully matches Ultimate: `getNodes()`/`getEdges()` are **sets**.
- **ILabeledEdge<N, L>** – Edge with `getSource()`, `getTarget()`, `getLabel()`, plus `equals()`/`hashCode()` for set semantics.
- **GenericLabeledGraph<N, L>** – Mutable graph with `addNode(N)`, `addEdge(N, L, N)`; ignores duplicate edges.
- **IRegex<L>** – Regular expression over labels (`Epsilon`, `EmptySet`, `Literal<L>`, `Concatenation`, `Union`, `Star`) with structural `equals()`/`hashCode()` and a visitor interface. Factory and Tarjan/Ultimate simplifications in **Regex<L>**.
- **RegexToTgf / RegexToCompactTgf** – Export regex syntax trees to Trivial Graph Format.
- **PathExpressionComputer<N, L>** – Build from a graph; call `exprBetween(source, target)` to get the path expression from `source` to `target`.

## Relation to APA

This utility computes ordinary regex-style path expressions over graph labels.
It is separate from `Dataflow/APA/Core/PathExpr.h`, which is the
APA-specific transfer-expression AST used by the intraprocedural dataflow
solver.

In short:

- `Utils/Algorithms/PathExpressions/`: generic labeled-graph regex engine
- `Dataflow/APA/`: dataflow transfer-expression engine with lattice evaluation

## Requirements

- **N** (node type): hashable and equality comparable (`std::hash<N>`, `operator==`) to be stored in sets/maps (like Java's `HashSet`).
- **L** (label type): hashable and equality comparable (`std::hash<L>`, `operator==`) and streamable (`operator<<`) for rendering.

## Complexity

For a fixed source, computing path expressions to all nodes: **O(n³ + m)** (n = nodes, m = edges).

## Example

```cpp
#include "Utils/Algorithms/PathExpressions/PathExpressions.h"

using namespace lotus::pathexpressions;

GenericLabeledGraph<int, char> g;
g.addNode(0);
g.addNode(1);
g.addNode(2);
g.addEdge(0, 'a', 1);
g.addEdge(1, 'b', 2);
g.addEdge(0, 'c', 2);

PathExpressionComputer<int, char> comp(g);
auto expr = comp.exprBetween(0, 2);  // paths 0->2: (a·b) ∪ c
```

## References

- R. E. Tarjan, "Fast Algorithms for Solving Path Problems", *J. ACM* 28(3), 1981.
- Ultimate Library-PathExpressions: [Ultimate](https://github.com/ultimate-pa/ultimate), [PathExpression](https://github.com/johspaeth/PathExpression).
