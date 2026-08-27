#pragma once

#include "Dataflow/Datalog/Core/Expr.h"
#include "Dataflow/Datalog/Core/Lattice.h"
#include "Dataflow/Datalog/Core/Relation.h"
#include "Dataflow/Datalog/Core/TypeSupport.h"
#include "Dataflow/Datalog/Semantic/SemanticIR.h"

#include <any>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace lotus::datalog {

class Context {
public:
  Context();
  ~Context();

  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;

  template <typename... Ts> Relation<Ts...> relation(std::string name) {
    static_assert((std::is_copy_constructible_v<Ts> && ...),
                  "Datalog relation columns must be copy constructible");
    static_assert(sizeof...(Ts) <= sizeof(ColumnMask) * 8,
                  "Datalog relation arity exceeds the runtime index mask");
    std::vector<ColumnType> columns{detail::makeColumnType<Ts>()...};
    RelationId id = addRelation(std::move(name), std::move(columns),
                                RelationKind::Set, {}, {});
    return Relation<Ts...>(this, id, relationName(id));
  }

  template <typename... Ts>
  Relation<Ts...> lattice(std::string name,
                          FunctionProperties properties = {}) {
    static_assert(sizeof...(Ts) >= 1,
                  "lattice relations require a lattice value column");
    static_assert((std::is_copy_constructible_v<Ts> && ...),
                  "Datalog relation columns must be copy constructible");
    static_assert(sizeof...(Ts) <= sizeof(ColumnMask) * 8,
                  "Datalog relation arity exceeds the runtime index mask");
    using Lattice = std::tuple_element_t<sizeof...(Ts) - 1, std::tuple<Ts...>>;
    static_assert(detail::HasJoinMut<Lattice>::value,
                  "lattice value must provide bool joinMut(const T &)");
    std::vector<ColumnType> columns{detail::makeColumnType<Ts>()...};
    auto join = [](std::any &current, const std::any &candidate) {
      return std::any_cast<Lattice &>(current).joinMut(
          std::any_cast<const Lattice &>(candidate));
    };
    RelationId id =
        addRelation(std::move(name), std::move(columns), RelationKind::Lattice,
                    std::move(join), properties);
    return Relation<Ts...>(this, id, relationName(id));
  }

  template <typename T> Var<T> var(std::string name) {
    VarId id = addVariable(std::move(name), typeid(T), false);
    return Var<T>(this, id, variableName(id));
  }

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;

  RelationId
  addRelation(std::string name, std::vector<ColumnType> columns,
              RelationKind kind,
              std::function<bool(std::any &, const std::any &)> lattice_join,
              FunctionProperties lattice_properties);
  VarId addVariable(std::string name, std::type_index type, bool anonymous);
  const std::string &relationName(RelationId id) const;
  const std::string &variableName(VarId id) const;
  TermIR freshWildcard(std::type_index type);
  void insert(RelationId relation, std::vector<std::any> row);
  bool erase(RelationId relation, const std::vector<std::any> &row);
  bool contains(RelationId relation, const std::vector<std::any> &row) const;
  std::vector<std::vector<std::any>> rows(RelationId relation) const;

  friend class Program;
  friend class CompiledProgram;
  friend class SemanticProgram;
  template <typename... Ts> friend class Relation;
  friend Context *detail::mergeContexts(Context *, Context *);
};

template <typename... Ts>
void Relation<Ts...>::ensureContext(Context *other) const {
  if (other && other != context_)
    throw std::invalid_argument(
        "Datalog expression and relation belong to different contexts");
}

template <typename... Ts>
TermIR Relation<Ts...>::makeWildcard(std::type_index type) const {
  return context_->freshWildcard(type);
}

template <typename... Ts>
void Relation<Ts...>::insertRow(std::vector<std::any> row) const {
  context_->insert(id_, std::move(row));
}

template <typename... Ts>
bool Relation<Ts...>::containsRow(const std::vector<std::any> &row) const {
  return context_->contains(id_, row);
}

template <typename... Ts>
bool Relation<Ts...>::eraseRow(const std::vector<std::any> &row) const {
  return context_->erase(id_, row);
}

template <typename... Ts>
std::vector<std::vector<std::any>> Relation<Ts...>::getRows() const {
  return context_->rows(id_);
}

using context = Context;

} // namespace lotus::datalog
