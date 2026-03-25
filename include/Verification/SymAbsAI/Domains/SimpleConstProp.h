#pragma once

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Expression.h"
#include "Verification/SymAbsAI/Core/ResultStore.h"
#include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Utils/Utils.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <iostream>
#include <memory>

#include <llvm/IR/Value.h>
#include <llvm/IR/ValueSymbolTable.h>

namespace symabs_ai {
class FunctionContext;

namespace domains {
class SimpleConstProp : public AbstractValue {
protected:
  const FunctionContext *FunctionContext_;
  RepresentedValue Value_;
  bool Top_{}, Bottom_{};
  ConcreteState::Value Constant_;

public:
  SimpleConstProp(const FunctionContext &fctx, RepresentedValue value)
      : FunctionContext_(&fctx), Value_(value) {
    resetToBottom();
  }

  static unique_ptr<AbstractValue> New(const Expression &expr,
                                       const DomainConstructor::args &args) {
    return std::move(
        make_unique<SimpleConstProp>(*args.fctx, expr.asRepresentedValue()));
  }

  virtual bool joinWith(const AbstractValue &av_other) override;

  virtual bool meetWith(const AbstractValue &av_other) override;

  virtual bool updateWith(const ConcreteState &cstate) override;

  virtual z3::expr toFormula(const ValueMapping &vmap,
                             z3::context &zctx) const override;

  virtual void havoc() override {
    Bottom_ = false;
    Top_ = true;
  }

  virtual AbstractValue *clone() const override {
    return new SimpleConstProp(*this);
  }

  virtual bool isTop() const override { return Top_; }
  virtual bool isBottom() const override { return Bottom_; }
  bool isConst() const { return !isTop() && !isBottom(); }

  /**
   * Returns the constant value stored in this AbstractValue (if it is a
   * constant (i.e. it is neither top nor bottom), otherwise assertions will
   * fail.
   */
  uint64_t getConstValue() const;

  /**
   * Returns the LLVM Value representing the variable whose constness is
   * described by this AbstractValue.
   */
  llvm::Value *getVariable() const { return Value_; }

  virtual void prettyPrint(PrettyPrinter &out) const override;

  virtual void resetToBottom() override;
  virtual bool isJoinableWith(const AbstractValue &other) const override;

private:
#ifdef ENABLE_DYNAMIC
  friend class cereal::access;
  SimpleConstProp() {}

  template <class Archive> void save(Archive &archive) const {
    archive(Value_, Bottom_, Top_);
    if (!Bottom_ && !Top_)
      archive(Constant_);
  }

  template <class Archive> void load(Archive &archive) {
    FunctionContext_ = &cereal::get_user_data<FunctionContext>(archive);
    archive(Value_, Bottom_, Top_);
    if (!Bottom_ && !Top_)
      archive(Constant_);
  }
#endif
};
} // namespace domains
} // namespace symabs_ai

#ifdef ENABLE_DYNAMIC
CEREAL_REGISTER_TYPE(symabs_ai::domains::SimpleConstProp);
#endif
