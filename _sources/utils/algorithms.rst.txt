Path Expression Algorithms
==========================

``include/Utils/Algorithms/`` contains algorithmic helpers that do not fit into
the general-purpose container layer.

**Current focus**: ``PathExpressions/``

- ``PathExpressionComputer`` computes path summaries over labeled graphs.
- ``Regex`` stores the resulting path-expression representation.
- ``LabeledGraph`` is the graph interface used by the algorithm.
- ``RegexToTgf`` and ``RegexToCompactTgf`` export debugging views.

This code is useful when analyses need compact descriptions of all paths between
two nodes rather than explicit path enumeration.

Practical use
-------------

Build a ``LabeledGraph`` from the analysis relation, then ask
``PathExpressionComputer`` for an expression between the chosen endpoints.
The resulting ``Regex`` is a summary of paths, not an enumerated witness set;
use the TGF exporters when inspecting or debugging the generated expression.
For a single reachability decision, a dedicated graph algorithm may be simpler
and cheaper than computing a full path expression.

See also :doc:`utilities`.
