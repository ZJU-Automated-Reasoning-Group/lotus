#pragma once

namespace lotus::datalog {

class Context;
class Program;
class CompiledProgram;
class Atom;
class Condition;
class Negation;
class AggregateClause;
class Body;
class Scheduler;
class SerialScheduler;
class ThreadScheduler;
class SemanticProgram;

template <typename T> class Expr;
template <typename T> class Var;
template <typename Input, typename Output> class AggregatorSpec;
template <typename... Ts> class Relation;

} // namespace lotus::datalog
