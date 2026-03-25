#pragma once

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/Expression.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <iostream>
#include <vector>

#include <llvm/IR/CFG.h>

namespace symabs_ai {
class ResultStore;
class ParamStrategy;

namespace configparser {
class Config;
} // namespace configparser

/***
 * Represents an analysis domain parameterized by expressions.
 *
 * Each domain constructor has a given name and textual description which are
 * used in the user interface and exposed to the Python configuration API.
 * Domain names can be hierarchical with two levels of hierarchy separated
 * with a ".".
 *
 * The actual work of constructing abstract values is performed by a factory
 * function stored by in a `DomainConstructor` object. This function takes
 * `DomainConstructor::args` as an argument -- a structure referencing the
 * objects necessary for the construction of `AbstractValue` as well as a
 * vector of expression parameters. The number of parameters the factory
 * function expects in this vector is specified by the domain constructor
 * arity that is specified during its construction. There are several
 * alternative constructors taking functions with signatures not using the
 * `args` structure to simplify the common uses (i.e. non-relational or
 * binary domains).
 *
 * To use a `DomainConstructor` object, call its `makeBottom` method. If the
 * arity is greater than zero, the object will automatically find out "good"
 * parameters for the domain. See the documentation for `makeBottom` for a
 * more detailed description of auto-parameterization.
 *
 * Every domain constructor has to be registered by creating a corresponding
 * global object of type `DomainConstructor::Register`. See `NumRels.cpp` for
 * examples of domain registration and initialization.
 */
class DomainConstructor {
public:
  struct args {
    llvm::SmallVector<Expression, 2> parameters;
    const FunctionContext *fctx;
    llvm::BasicBlock *location;
    bool is_after_bb;
  };

private:
  // basic form of a "factory function" that this class wraps
  typedef std::function<unique_ptr<AbstractValue>(const args &)> factory_func_t;

  // alternative factory functions that the domain implementers can supply
  // instead of factory_func_t if it's more convenient
  typedef std::function<unique_ptr<AbstractValue>(const FunctionContext &,
                                                  llvm::BasicBlock *, bool)>
      alt_ffunc_0;
  typedef std::function<unique_ptr<AbstractValue>(Expression, const args &)>
      alt_ffunc_1;
  typedef std::function<unique_ptr<AbstractValue>(Expression, Expression,
                                                  const args &)>
      alt_ffunc_2;

  std::string Name_;
  std::string Description_;
  int Arity_;
  factory_func_t FactoryFunc_;

  static std::vector<DomainConstructor> *KnownDomains_;

  DomainConstructor autoParameterize(int desired_arity) const;

public:
  DomainConstructor() : Name_("<invalid>"), Arity_(-1) {}

  DomainConstructor(std::string name, std::string desc, int arity,
                    factory_func_t factory_func)
      : Name_(std::move(name)), Description_(std::move(desc)), Arity_(arity),
        FactoryFunc_(factory_func) {
    assert(arity >= 0);
  }

  DomainConstructor(std::string name, std::string desc,
                    alt_ffunc_0 factory_func);

  DomainConstructor(std::string name, std::string desc,
                    alt_ffunc_1 factory_func);

  DomainConstructor(std::string name, std::string desc,
                    alt_ffunc_2 factory_func);

  static DomainConstructor product(std::vector<DomainConstructor>);

  DomainConstructor rename(std::string name, std::string desc) {
    return DomainConstructor(name, desc, Arity_, FactoryFunc_);
  }

  class Register {
  public:
    Register(DomainConstructor domain) {
      static std::vector<DomainConstructor> known_domains;
      DomainConstructor::KnownDomains_ = &known_domains;

#ifndef NDEBUG
      for (auto &d : known_domains)
        assert(d.name() != domain.name());
#endif

      known_domains.push_back(domain);
    }

    template <typename F>
    Register(std::string name, std::string desc, F f)
        : Register(DomainConstructor(name, desc, f)) {}

    Register(std::string name, std::string desc, DomainConstructor dc)
        : Register(dc.rename(name, desc)) {}
  };

  /**
   * Apply a given parametrization strategy to fix some of the parameters
   * of this domain constructor.
   *
   * See the documentation of `ParamStrategy` for more on parametrization
   * strategies.
   */
  DomainConstructor parameterize(const ParamStrategy &);

  /**
   * Constructs a default abstract domain from configuration.
   */
  DomainConstructor(const configparser::Config &config);

  /**
   * Makes a new `AbstractValue` representing this domain's bottom.
   *
   * If this domain constructor's arity is nonzero, the method will try to
   * fix the missing parameters using default parametrization strategies:
   * `ParamStrategy::AllValues` and `ParamStrategy::AllValuePairs`.
   */
  unique_ptr<AbstractValue> makeBottom(const FunctionContext &fctx,
                                       llvm::BasicBlock *loc, bool after) const;

  unique_ptr<AbstractValue>
  makeBottom(const DomainConstructor::args &args) const;

  const std::string &name() const { return Name_; }
  const std::string &description() const { return Description_; }
  int arity() const { return Arity_; }
  bool isInvalid() const { return Arity_ < 0; }

  /**
   * Returns all domains registered using `DomainConstructor::Register`.
   */
  static const std::vector<DomainConstructor> &all();

  friend std::ostream &operator<<(std::ostream &out,
                                  const DomainConstructor &dom) {
    out << "<DomainConstructor " << dom.name();

    if (dom.arity() > 0) {
      out << "(";
      for (int i = 0; i < dom.arity() - 1; i++) {
        out << "_, ";
      }
      out << "_)";
    }

    return out << ">";
  }
};
} // namespace symabs_ai
