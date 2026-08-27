#include "Dataflow/Datalog/Core/Context.h"

#include "Dataflow/Datalog/EngineInternal.h"

#include <any>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lotus::datalog {

using internal::RelationStorage;

Context *detail::mergeContexts(Context *lhs, Context *rhs) {
  if (lhs && rhs && lhs != rhs)
    throw std::invalid_argument(
        "Datalog expressions belong to different contexts");
  return lhs ? lhs : rhs;
}

Context::Context() : impl_(std::make_shared<Impl>()) {}
Context::~Context() = default;

RelationId Context::addRelation(
    std::string name, std::vector<ColumnType> columns, RelationKind kind,
    std::function<bool(std::any &, const std::any &)> lattice_join) {
  std::unique_lock<std::mutex> lock(impl_->execution_mutex, std::try_to_lock);
  if (!lock.owns_lock() || impl_->running)
    throw std::logic_error("Datalog relation definitions may not change while "
                           "a program is running");
  if (name.empty())
    throw std::invalid_argument("Datalog relation name must not be empty");
  if (!impl_->relation_names.insert(name).second)
    throw std::invalid_argument("duplicate Datalog relation name '" + name +
                                "'");
  if (columns.size() > sizeof(ColumnMask) * 8)
    throw std::invalid_argument(
        "Datalog relation arity exceeds index mask width");

  RelationId id = impl_->relations.size();
  RelationIR definition{id, std::move(name), std::move(columns), kind,
                        std::move(lattice_join)};
  impl_->relations.push_back(
      std::make_unique<RelationStorage>(std::move(definition)));
  ++impl_->schema_generation;
  return id;
}

VarId Context::addVariable(std::string name, std::type_index type,
                           bool anonymous) {
  std::unique_lock<std::mutex> lock(impl_->execution_mutex, std::try_to_lock);
  if (!lock.owns_lock() || impl_->running)
    throw std::logic_error("Datalog variable definitions may not change while "
                           "a program is running");
  if (!anonymous && name.empty())
    throw std::invalid_argument("Datalog variable name must not be empty");
  VarId id = impl_->variables.size();
  impl_->variables.push_back({std::move(name), type, anonymous});
  ++impl_->schema_generation;
  return id;
}

const std::string &Context::relationName(RelationId id) const {
  return impl_->relations.at(id)->definition().name;
}

const std::string &Context::variableName(VarId id) const {
  return impl_->variables.at(id).name;
}

TermIR Context::freshWildcard(std::type_index type) {
  VarId id = addVariable("_", type, true);
  TermIR result;
  result.kind = TermIR::Kind::Variable;
  result.type = type;
  result.variable = id;
  result.anonymous = true;
  result.debug_name = "_";
  return result;
}

void Context::insert(RelationId relation, std::vector<std::any> row) {
  std::unique_lock<std::mutex> lock(impl_->execution_mutex, std::try_to_lock);
  if (!lock.owns_lock() || impl_->running)
    throw std::logic_error(
        "Datalog relations may not be mutated while a program is running");
  impl_->relations.at(relation)->insertBase(std::move(row));
}

bool Context::contains(RelationId relation,
                       const std::vector<std::any> &row) const {
  return impl_->relations.at(relation)->contains(row);
}

std::vector<std::vector<std::any>> Context::rows(RelationId relation) const {
  return impl_->relations.at(relation)->materializeRows();
}

} // namespace lotus::datalog
