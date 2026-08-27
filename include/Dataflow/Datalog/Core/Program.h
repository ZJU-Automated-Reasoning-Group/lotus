#pragma once

#include "Dataflow/Datalog/Core/Context.h"
#include "Dataflow/Datalog/Core/Error.h"
#include "Dataflow/Datalog/Core/Rule.h"
#include "Dataflow/Datalog/Runtime/Diagnostics.h"
#include "Dataflow/Datalog/Runtime/Scheduler.h"

#include <any>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace lotus::datalog {

struct QueryBinding {
  std::size_t column = 0;
  std::any value;
};

struct QueryGoal {
  RelationId relation = 0;
  std::vector<QueryBinding> bindings;
};

struct CompileOptions {
  std::vector<RelationId> goals;
  std::vector<QueryGoal> query_goals;
  std::size_t index_memory_budget_bytes = static_cast<std::size_t>(-1);
  std::size_t max_arrangements_per_relation = static_cast<std::size_t>(-1);
  std::size_t adaptive_replan_ratio = 4;
};

class Program {
public:
  explicit Program(Context &context) : context_(&context) {}

  void rule(const Atom &head, const Atom &body);
  void rule(const Atom &head, const Body &body);
  void rule(const Atom &head, const Condition &body);
  void rule(const Atom &head, const Negation &body);
  void rule(const Atom &head, const AggregateClause &body);
  void rule(std::initializer_list<Atom> heads, const Atom &body);
  void rule(std::initializer_list<Atom> heads, const Body &body);
  void rule(std::initializer_list<Atom> heads, const Condition &body);
  void rule(std::initializer_list<Atom> heads, const Negation &body);
  void rule(std::initializer_list<Atom> heads, const AggregateClause &body);

  CompiledProgram compile() const;
  CompiledProgram compile(const CompileOptions &options) const;

private:
  void addRule(const Atom &head, Context *body_context,
               std::vector<BodyItemIR> body);
  void addRules(std::initializer_list<Atom> heads, Context *body_context,
                const std::vector<BodyItemIR> &body);

  Context *context_ = nullptr;
  std::vector<RuleIR> rules_;

  friend class SemanticProgram;
};

class CompiledProgram {
public:
  CompiledProgram(CompiledProgram &&) noexcept;
  CompiledProgram &operator=(CompiledProgram &&) noexcept;
  ~CompiledProgram();

  CompiledProgram(const CompiledProgram &) = delete;
  CompiledProgram &operator=(const CompiledProgram &) = delete;

  RunStatus run();
  RunStatus run(const ExecutionOptions &options);
  RunStatus runReadOnly();
  const ExecutionStats &stats() const;
  const ExecutionProfile &profile() const;
  std::string explain(ExplainMode mode = ExplainMode::Plan) const;

  // A compiled program retains the Context's internal storage, so it may be
  // executed after the Context wrapper that produced it has been destroyed.

private:
  struct Impl;
  explicit CompiledProgram(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  friend class Program;
  friend class SemanticProgram;
};

using program = Program;
using compiled_program = CompiledProgram;

} // namespace lotus::datalog
