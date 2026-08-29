#pragma once

#include "Solvers/Datalog/Core/Expr.h"
#include "Solvers/Datalog/Core/Forward.h"
#include "Solvers/Datalog/Semantic/SemanticIR.h"

#include <utility>

namespace lotus::datalog {

struct Wildcard {};
inline constexpr Wildcard _{};

class Atom {
public:
  Atom() = delete;

  Context *context() const { return context_; }
  const AtomIR &ir() const { return ir_; }

private:
  Atom(Context *context, AtomIR ir) : context_(context), ir_(std::move(ir)) {}

  Context *context_ = nullptr;
  AtomIR ir_;

  template <typename... Ts> friend class Relation;
  friend class Program;
  friend class Body;
};

class Condition {
public:
  explicit Condition(const Expr<bool> &predicate)
      : context_(predicate.context()), ir_{predicate.lower()} {}

  Context *context() const { return context_; }
  const FilterIR &ir() const { return ir_; }

private:
  Context *context_ = nullptr;
  FilterIR ir_;

  friend class Body;
  friend class Program;
};

inline Condition where(const Expr<bool> &predicate) {
  return Condition(predicate);
}

class Negation {
public:
  Context *context() const { return context_; }
  const NegAtomIR &ir() const { return ir_; }

private:
  explicit Negation(const Atom &atom)
      : context_(atom.context()), ir_{atom.ir()} {}

  Context *context_ = nullptr;
  NegAtomIR ir_;

  friend Negation neg(const Atom &);
  friend class Body;
  friend class Program;
};

inline Negation neg(const Atom &atom) { return Negation(atom); }

using atom = Atom;

} // namespace lotus::datalog
