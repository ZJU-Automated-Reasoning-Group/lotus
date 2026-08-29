Native C++ Datalog
==================

Lotus provides a native, strongly typed C++17 Datalog runtime under
``Solvers/Datalog``. The template API lowers rules to a type-erased semantic IR,
then compiles relation dependencies into an SCC-ordered execution plan.

The runtime supports positive recursion, conditions, head expressions,
distinct-aware greedy join planning, specialized full and partial runtime indexes,
stratified aggregation and negation, lattice relations, and bulk-synchronous
parallel evaluation.

.. code-block:: cpp

   #include "Solvers/Datalog/Core/Program.h"

   using namespace lotus::datalog;

   context ctx;
   auto edge = ctx.relation<int, int>("edge");
   auto path = ctx.relation<int, int>("path");
   auto x = ctx.var<int>("x");
   auto y = ctx.var<int>("y");
   auto z = ctx.var<int>("z");

   program p(ctx);
   p.rule(path(x, y), edge(x, y));
   p.rule(path(x, z), path(x, y) && edge(y, z));

   edge.insert(1, 2);
   edge.insert(2, 3);
   auto compiled = p.compile();
   ExecutionOptions options;
   options.worker_count = 4;
   RunStatus status = compiled.run(options);

``Core/Program.h`` is the entry point for the native C++ API. Lower-level APIs
are exposed through focused headers under ``Core``, ``Runtime``, and
``Semantic``; there is no catch-all umbrella header.

The aggregate API provides collecting, streaming, and reducible factories.
Reducible aggregators run with worker-local state. The lattice library includes
minimum, maximum, set-union, dual, product, bounded-set, and
constant-propagation values.

The current semantic and architecture reference is maintained in
``lib/Solvers/Datalog/README.md``.

Command-line engine
-------------------

``lotus-datalog`` is the engine entry point for non-C++ clients. Native Datalog,
Z3 fixedpoint input, and JSON all lower to the same type-erased Semantic IR; the
runtime does not contain format-specific execution paths.

.. code-block:: bash

   ./build/bin/lotus-datalog schema > transitive-closure.json
   ./build/bin/lotus-datalog validate transitive-closure.json
   ./build/bin/lotus-datalog run transitive-closure.json --workers 4 --pretty
   ./build/bin/lotus-datalog schema | ./build/bin/lotus-datalog run -
   ./build/bin/lotus-datalog run tools/solver/datalog/examples/transitive_closure.dl
   ./build/bin/lotus-datalog run tools/solver/datalog/examples/z3_fixedpoint.smt2

The ``.dl`` frontend covers the portable engine semantics: typed set and lattice
relations, inline facts, nullary predicates, multiple heads, positive rules,
stratified negation, filters and expressions, and the built-in aggregates. The Z3
frontend supports the finite relational intersection: bit-vector or finite-domain
sorts, relation and variable declarations, Horn rules, conjunction, stratified
relation negation, and whole-relation queries. Format is detected automatically or
chosen with ``--format auto|json|datalog|z3``.

The reusable frontend API is declared in
``Solvers/Datalog/Frontend/Frontend.h`` and
implemented by the ``LotusDatalogFrontend`` library under
``lib/Solvers/Datalog/Frontend``. JSON, Lotus Datalog, Z3, and dispatch live in
separate translation units and produce a source-aware ``FrontendIR`` which lowers
directly to ``SemanticProgram`` without a JSON round trip.
``SourceUnit``/``executeInputs`` compose declarations, facts, and rules from
multiple files; declarations may follow their uses. Native ``.include`` uses an
injected ``SourceResolver`` so the library never chooses filesystem policy. The
CLI contains no parser implementation and supplies a relative-path resolver.
Output rows are sorted deterministically, making it suitable for future Python
differential and performance harnesses. The complete syntax is documented in
``tools/solver/datalog/README.md``.
