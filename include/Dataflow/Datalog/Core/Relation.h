#pragma once

#include "Dataflow/Datalog/Core/Atom.h"
#include "Dataflow/Datalog/Core/Expr.h"
#include "Dataflow/Datalog/Core/Forward.h"
#include "Dataflow/Datalog/Semantic/SemanticIR.h"

#include <any>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace lotus::datalog {

template <typename... Ts> class Relation {
public:
  using row_type = std::tuple<Ts...>;

  Relation() = delete;

  const std::string &name() const { return name_; }
  RelationId id() const { return id_; }

  void insert(Ts... values) const {
    std::vector<std::any> row;
    row.reserve(sizeof...(Ts));
    (row.emplace_back(std::move(values)), ...);
    insertRow(std::move(row));
  }

  bool erase(const Ts &...values) const {
    std::vector<std::any> row;
    row.reserve(sizeof...(Ts));
    (row.emplace_back(values), ...);
    return eraseRow(row);
  }

  bool contains(const Ts &...values) const {
    std::vector<std::any> row;
    row.reserve(sizeof...(Ts));
    (row.emplace_back(values), ...);
    return containsRow(row);
  }

  std::vector<row_type> rows() const {
    auto dynamic_rows = getRows();
    std::vector<row_type> result;
    result.reserve(dynamic_rows.size());
    for (const auto &row : dynamic_rows)
      result.push_back(toTuple(row, std::index_sequence_for<Ts...>{}));
    return result;
  }

  template <typename... Args> Atom operator()(Args &&...args) const {
    static_assert(sizeof...(Args) == sizeof...(Ts),
                  "Datalog atom arity does not match its relation");
    AtomIR ir;
    ir.relation = id_;
    ir.relation_name = name_;
    ir.args = makeTerms(std::index_sequence_for<Ts...>{},
                        std::forward_as_tuple(std::forward<Args>(args)...));
    return Atom(context_, std::move(ir));
  }

private:
  Relation(Context *context, RelationId id, std::string name)
      : context_(context), id_(id), name_(std::move(name)) {}

  template <typename T> TermIR makeTerm(const Var<T> &variable) const {
    static_assert((std::is_same_v<T, Ts> || ...),
                  "internal Datalog variable type mismatch");
    ensureContext(variable.context());
    TermIR result;
    result.kind = TermIR::Kind::Variable;
    result.type = typeid(T);
    result.variable = variable.id();
    result.debug_name = variable.name();
    return result;
  }

  template <typename T> TermIR makeTerm(const Expr<T> &expression) const {
    ensureContext(expression.context());
    TermIR result;
    result.kind = TermIR::Kind::Expression;
    result.type = typeid(T);
    result.expression = expression.lower();
    result.debug_name = expression.debugName();
    return result;
  }

  TermIR makeWildcard(std::type_index type) const;

  template <typename Column, typename Value>
  TermIR makeTerm(Value &&value) const {
    static_assert(
        std::is_constructible_v<Column, Value>,
        "Datalog atom argument type does not match its relation column");
    TermIR result;
    result.kind = TermIR::Kind::Constant;
    result.type = typeid(Column);
    result.constant = Column(std::forward<Value>(value));
    result.debug_name = "constant";
    return result;
  }

  template <std::size_t... Is, typename Tuple>
  std::vector<TermIR> makeTerms(std::index_sequence<Is...>,
                                Tuple &&args) const {
    std::vector<TermIR> result;
    result.reserve(sizeof...(Is));
    (result.push_back(
         makeTermForColumn<Is>(std::get<Is>(std::forward<Tuple>(args)))),
     ...);
    return result;
  }

  template <std::size_t I, typename Arg>
  TermIR makeTermForColumn(Arg &&arg) const {
    using Column = std::tuple_element_t<I, std::tuple<Ts...>>;
    if constexpr (std::is_same_v<std::decay_t<Arg>, Var<Column>>) {
      return makeTerm(arg);
    } else if constexpr (std::is_base_of_v<Expr<Column>, std::decay_t<Arg>>) {
      return makeTerm(static_cast<const Expr<Column> &>(arg));
    } else if constexpr (std::is_same_v<std::decay_t<Arg>, Wildcard>) {
      return makeWildcard(typeid(Column));
    } else {
      return makeTerm<Column>(std::forward<Arg>(arg));
    }
  }

  template <std::size_t... Is>
  static row_type toTuple(const std::vector<std::any> &row,
                          std::index_sequence<Is...>) {
    if (row.size() != sizeof...(Ts))
      throw std::logic_error("invalid row arity in Datalog relation storage");
    return row_type(std::any_cast<Ts>(row[Is])...);
  }

  void ensureContext(Context *other) const;
  void insertRow(std::vector<std::any> row) const;
  bool containsRow(const std::vector<std::any> &row) const;
  bool eraseRow(const std::vector<std::any> &row) const;
  std::vector<std::vector<std::any>> getRows() const;

  Context *context_ = nullptr;
  RelationId id_ = 0;
  std::string name_;

  friend class Context;
};

template <typename... Ts> using relation = Relation<Ts...>;

} // namespace lotus::datalog
