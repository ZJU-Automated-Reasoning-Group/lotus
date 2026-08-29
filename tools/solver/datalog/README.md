# lotus-datalog

`lotus-datalog` is the command-line entry point for the native Lotus
Datalog/lattice engine. It accepts native Datalog syntax, a finite relational
subset of Z3 fixedpoint syntax, or JSON Semantic IR. All three frontends lower to
the same versioned Semantic IR and write deterministic JSON results.

```text
lotus-datalog run <source...|-> [options]
lotus-datalog explain <source...|-> [--analyze] [options]
lotus-datalog validate <source...|-> [options]
lotus-datalog schema
```

`schema` prints a runnable transitive-closure example. `validate` performs all
compile-time analysis without running the fixed point. Input format is detected
from the first significant character, or selected with
`--format auto|json|datalog|z3`. `run` also accepts `--workers N`,
`--grain-size N`, `--pretty`, `--trace-scc`, `--trace-rule`, and `--trace-delta`.
`explain` emits the selected SCC, join, lookup, and estimate plan without running;
`--analyze` runs it with per-operation actual cardinalities.

## Native Datalog syntax

The native text frontend covers the complete portable engine surface exposed by
JSON Semantic IR v1:

```prolog
.decl edge(src: u64, dst: u64)
.decl path(src: u64, dst: u64)
.decl reaches_four()

edge(1, 2).
edge(2, 4).

path(X, Y) :- edge(X, Y).
path(X, Z) :- path(X, Y), edge(Y, Z).
reaches_four :- path(1, 4).

.output path
.output reaches_four
```

Declarations are required. Supported column types are `i64`, `u64`, `f64`,
`string`, and `bool`. Lattice result types are `min<i64>`, `max<i64>`,
`min<f64>`, `max<f64>`, and `set<i64>`; a relation whose final column has one of
these types is a lattice relation. Variables begin with an uppercase letter; `_`
is anonymous. Constants are integers, finite floating-point values, booleans,
quoted strings, or i64 set literals such as `[1, 2, 3]`. `#` and `//` introduce
line comments. Nullary relations use declarations such as `.decl reachable()`.

Positive atoms, stratified negation, filters, head expressions, and multiple heads
are written as follows:

```prolog
accepted(X + 1); audit(X) :-
  input(X), not blocked(X), where X >= 0 && X < 100.

# !blocked(X) is equivalent to not blocked(X)
```

Expressions support parentheses, unary `!`, `+`, and `-`, arithmetic
`+ - * / %`, comparisons `== != < <= > >=`, and Boolean `&& ||`. The
`min_lattice(expr)`, `max_lattice(expr)`, and `set_lattice(expr)` constructors
create lattice values in rule heads.

Built-in aggregates use one source atom and preserve variables grounded by earlier
body items as group keys:

```prolog
total(G, S) :- group(G), aggregate S = sum(V) : value(G, V).
count(G, N) :- group(G), aggregate N = count : value(G, _).
mean(G, M)  :- group(G), aggregate M = mean(V) : value(G, V).
```

Supported aggregate names are `count`, `sum`, `min`, `max`, and `mean`.
Lattice relations need no separate execution syntax:

```prolog
.decl edge(src: u64, dst: u64, weight: i64)
.decl distance(node: u64, value: min<i64>)

distance(1, 0).
distance(Y, D + W) :- distance(X, D), edge(X, Y, W).
```

The text frontend intentionally does not add facilities outside the engine
contract, such as external fact-file directives, modules, user functors, custom
C++ types/reducers/lattices, or Soufflé components. Those embedding-only extension
points remain C++ API features.

Run the checked-in example with:

```text
lotus-datalog run tools/solver/datalog/examples/transitive_closure.dl --pretty
```

Several ordered source files may form one program. This allows declarations,
facts, and rules to live in different files or directories:

```text
lotus-datalog run \
  tools/solver/datalog/examples/multifile/schema.dl \
  tools/solver/datalog/examples/multifile/facts/graph.dl \
  tools/solver/datalog/examples/multifile/rules/reachability.dl
```

The same example has an include-based entry point:

```text
lotus-datalog run tools/solver/datalog/examples/multifile/main.dl
```

Name and arity resolution runs after every source has been parsed, so declarations
may appear before or after the facts and rules that use them. Diagnostics retain
the originating source name, line, and column.

Native sources may also import other native sources:

```prolog
.include "schema.dl"
.include "facts/graph.dl"
.include "rules/reachability.dl"
```

The reusable API exposes multi-source composition through `frontend::SourceUnit`
and `executeInputs()`. `.include` uses the `SourceResolver` callback in
`RunOptions`; the library itself performs no filesystem access. The CLI installs
a resolver that interprets relative paths from the including file's directory.
Resolved source names are used for cycle detection and diagnostics.

## Z3 fixedpoint subset

The Z3 frontend accepts the finite Datalog-shaped subset needed for
interchange with simple μZ programs:

- `define-sort` aliases of `(_ BitVec N)` with `1 <= N <= 64`, and
  `(_ FiniteDomain N)`;
- `Bool`, `declare-rel`, and `declare-var`;
- fact rules, Horn implications, `and`, stratified relation `not`, `true` bodies,
  and whole-relation `query` commands;
- `#b...`, `#x...`, `(_ bvN W)`, Boolean, and finite-domain constants.

For example:

```text
lotus-datalog run tools/solver/datalog/examples/z3_fixedpoint.smt2 --pretty
```

The frontend is not an SMT solver or a complete Z3 compatibility layer. Spacer,
arithmetic/bit-vector constraints, arrays, ADTs, quantifiers, rule names, and
fixedpoint transformation options are rejected or ignored only where explicitly
documented (`set-option` and `set-logic`). A Z3 `query` must name a whole relation
and selects it for Lotus's canonical JSON output; Lotus does not reproduce Z3's
`sat`/`unsat` answer format.

## Program shape

```json
{
  "schema_version": 1,
  "relations": [
    {
      "name": "edge",
      "columns": ["i64", "i64"],
      "kind": "relation",
      "facts": [[1, 2], [2, 3]]
    },
    {
      "name": "path",
      "columns": ["i64", "i64"]
    }
  ],
  "rules": [
    {
      "head": {"relation": "path", "args": ["$x", "$y"]},
      "body": [
        {"atom": {"relation": "edge", "args": ["$x", "$y"]}},
        {"where": {"op": ">", "args": ["$y", 0]}}
      ]
    }
  ],
  "outputs": ["path"]
}
```

Supported scalar column types are `i64`, `u64`, `f64`, `string`, and `bool`.
Lattice value types are `min<i64>`, `max<i64>`, `min<f64>`, `max<f64>`, and
`set<i64>`. A lattice relation must use a lattice type for its final column; all
preceding columns form the key.
Relations with zero columns are supported by the Semantic IR as nullary
predicates; their only possible tuple is `[]`.
Because LLVM JSON numbers are signed 64-bit integers, `u64` values above
`9223372036854775807` are represented as decimal strings in JSON input and
output, for example `"18446744073709551615"`.

Variables are strings beginning with `$`. `_` is an anonymous variable. Constants
may be written directly or as `{"const": value}`. An atom has the form
`{"relation": "name", "args": [...]}`.

## Rule bodies

Body entries are one of:

```json
{"atom": {"relation": "edge", "args": ["$x", "$y"]}}
{"not": {"relation": "blocked", "args": ["$y"]}}
{"where": {"op": ">", "args": ["$y", 0]}}
{"aggregate": {
  "output": "$sum",
  "op": "sum",
  "value": "$weight",
  "source": {"relation": "weighted", "args": ["$key", "$weight"]}
}}
```

Rules use either `head` or `heads`. Supported aggregate functions are `count`,
`sum`, `min`, `max`, and `mean`. The aggregate source is one atom; variables bound
before the aggregate act as group keys. Negated atoms and filter expressions must
refer only to variables grounded by earlier body entries.

Expressions use `{"op": operator, "args": [...]}`. Numeric operators are `+`,
`-`, `*`, `/`, `%`, `==`, `!=`, `<`, `<=`, `>`, and `>=`; booleans additionally
support `!`, `&&`, and `||`. Unary numeric operators are `unary-` and `unary+`.
`min_lattice`, `max_lattice`, and `set_lattice` construct lattice values.

## Capability boundary

All three serialized frontends lower through the same source-aware `FrontendIR`
and then directly into `SemanticProgram`; `.dl` and `.smt2` are not serialized to
JSON and parsed again.

| Capability | JSON v1 | Native `.dl` | Z3 subset | Native C++ API |
| --- | --- | --- | --- | --- |
| `i64/u64/f64/string/bool` relations | yes | yes | finite `u64`/`bool` | arbitrary registered C++ types |
| nullary relations and inline facts | yes | yes | yes | yes |
| positive recursion and multiple recursive atoms | yes | yes | yes | yes |
| stratified negation | yes | yes | relation `not` | yes |
| filters and checked expressions | yes | yes | no SMT constraints | host C++ expressions |
| multiple rule heads | yes | yes | no | yes |
| count/sum/min/max/mean | yes | yes | no | built-in and custom aggregators |
| min/max/set lattice values | yes | yes | no | built-in and custom lattices |
| multi-source programs | one JSON document | yes, including `.include` | yes | embedding-defined |
| custom scheduler/cancellation/rerun session | execution API only | execution API only | execution API only | yes |

“Complete frontend coverage” therefore means the portable built-in semantic
surface. Host callbacks, arbitrary C++ value types, user reducers/lattices, and
long-lived additive rerun sessions are embedding capabilities and intentionally
have no portable textual representation.

JSON Semantic IR version 1 uses checked arithmetic. Integer overflow, division by
zero, remainder by zero, and non-finite floating-point inputs or results fail with
a structured evaluation error. Native C++ expressions retain normal C++ operator
semantics instead. All input programs and output envelopes carry
`"schema_version": 1`; other versions are rejected.

The output contains requested relation rows in canonical order and execution
statistics. Parallel statistics distinguish rule, merge, and aggregate tasks. This
stable interface is intended for later Python-based semantic differential tests and
performance measurement.

Failures are also JSON values. For example:

```json
{
  "schema_version": 1,
  "status": "error",
  "error": {
    "category": "evaluation",
    "code": "integer_overflow",
    "expression": "+",
    "message": "integer overflow in addition"
  }
}
```

The JSON frontend intentionally exposes a portable fixed set of scalar and lattice
types. Native C++ programs can additionally use custom types, streaming and custom
reducible aggregators, and the dual, product, bounded-set, and
constant-propagation lattice classes from `Lattice.h`.
