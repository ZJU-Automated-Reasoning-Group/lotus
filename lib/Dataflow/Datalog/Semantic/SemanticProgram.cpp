#include "Dataflow/Datalog/Semantic/SemanticProgram.h"

#include <stdexcept>
#include <utility>

namespace lotus::datalog {

SemanticProgram::SemanticProgram() : program_(context_) {}
SemanticProgram::~SemanticProgram() = default;

RelationId SemanticProgram::addRelation(
    std::string name, std::vector<ColumnType> columns, RelationKind kind,
    std::function<bool(std::any &, const std::any &)> lattice_join,
    FunctionProperties lattice_properties) {
  if (kind == RelationKind::Lattice && !lattice_join)
    throw std::invalid_argument("lattice relation requires a join operation");
  return context_.addRelation(std::move(name), std::move(columns), kind,
                              std::move(lattice_join), lattice_properties);
}

VarId SemanticProgram::addVariable(std::string name, std::type_index type,
                                   bool anonymous) {
  return context_.addVariable(std::move(name), type, anonymous);
}

void SemanticProgram::addFact(RelationId relation, std::vector<std::any> row) {
  context_.insert(relation, std::move(row));
}

void SemanticProgram::addRule(RuleIR rule) {
  program_.rules_.push_back(std::move(rule));
}

CompiledProgram SemanticProgram::compile() const { return program_.compile(); }

CompiledProgram SemanticProgram::compile(const CompileOptions &options) const {
  return program_.compile(options);
}

std::vector<std::vector<std::any>>
SemanticProgram::rows(RelationId relation) const {
  return context_.rows(relation);
}

} // namespace lotus::datalog
