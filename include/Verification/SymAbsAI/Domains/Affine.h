#pragma once

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Expression.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

namespace symabs_ai {
namespace domains {
class Affine : public AbstractValue {
private:
  enum state_t {
    TOP,
    BOTTOM,
    VALUE,
  };

  const FunctionContext &FunctionContext_;
  state_t State_;
  int64_t Delta_; // FIXME this breaks for large Deltas.
  llvm::Value *Left_;
  llvm::Value *Right_;

public:
  Affine(const FunctionContext &fctx, llvm::Value *left, llvm::Value *right)
      : FunctionContext_(fctx), State_(BOTTOM), Left_(left), Right_(right) {
    z3::sort sort_left = fctx.sortForType(left->getType());
    z3::sort sort_right = fctx.sortForType(right->getType());

    assert(sort_left.is_bv() && sort_right.is_bv());
    assert(sort_left.bv_size() == sort_right.bv_size());
  }

  static unique_ptr<AbstractValue> New(Expression left, Expression right,
                                       const DomainConstructor::args &args) {
    return std::move(make_unique<Affine>(*args.fctx, left.asRepresentedValue(),
                                         right.asRepresentedValue()));
  }

  virtual bool joinWith(const AbstractValue &av_other) override;
  virtual bool meetWith(const AbstractValue &av_other) override;
  virtual bool updateWith(const ConcreteState &state) override;
  virtual z3::expr toFormula(const ValueMapping &vmap,
                             z3::context &zctx) const override;
  virtual void havoc() override;
  virtual void prettyPrint(PrettyPrinter &out) const override;

  virtual AbstractValue *clone() const override { return new Affine(*this); }
  virtual bool isTop() const override { return State_ == TOP; }
  virtual bool isBottom() const override { return State_ == BOTTOM; }

  virtual void resetToBottom() override { State_ = BOTTOM; }
  virtual bool isJoinableWith(const AbstractValue &other) const override;

  llvm::Value *getLeft() { return Left_; }
  llvm::Value *getRight() { return Right_; }
  decltype(Delta_) getDelta() { return Delta_; }
};
} // namespace domains
} // namespace symabs_ai
