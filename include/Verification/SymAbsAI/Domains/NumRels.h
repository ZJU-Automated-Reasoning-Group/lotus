#pragma once

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Expression.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <llvm/IR/Value.h>

namespace symabs_ai {
namespace domains {
class NumRels : public AbstractValue {
public:
  static const uint8_t BOTTOM = 0;
  static const uint8_t LOWER = 1 << 1;
  static const uint8_t GREATER = 1 << 2;
  static const uint8_t EQUAL = 1 << 3;
  static const uint8_t TOP = LOWER | EQUAL | GREATER;

private:
  const FunctionContext &FunctionContext_;
  Expression Left_;
  Expression Right_;
  const bool IsSigned_;
  uint8_t Rel_;

public:
  NumRels(const FunctionContext &fctx, const Expression &left,
          const Expression &right, bool is_signed = false)
      : FunctionContext_(fctx), Left_(left), Right_(right),
        IsSigned_(is_signed), Rel_(BOTTOM) {}

  static unique_ptr<AbstractValue>
  NewSigned(const Expression &left, const Expression &right,
            const DomainConstructor::args &args) {
    return std::move(make_unique<NumRels>(*args.fctx, left, right, true));
  }

  static unique_ptr<AbstractValue>
  NewUnsigned(const Expression &left, const Expression &right,
              const DomainConstructor::args &args) {
    return std::move(make_unique<NumRels>(*args.fctx, left, right, false));
  }

  static unique_ptr<AbstractValue>
  NewZero(const Expression &expr, const DomainConstructor::args &args) {
    ConcreteState::Value zero(*args.fctx, 0, expr.bits(*args.fctx));
    return std::move(make_unique<NumRels>(*args.fctx, expr, zero, true));
  }

  virtual bool joinWith(const AbstractValue &av_other) override;
  virtual bool meetWith(const AbstractValue &av_other) override;
  virtual bool updateWith(const ConcreteState &state) override;
  virtual z3::expr toFormula(const ValueMapping &vmap,
                             z3::context &zctx) const override;
  virtual void prettyPrint(PrettyPrinter &out) const override;

  virtual void havoc() override { Rel_ = TOP; }

  virtual AbstractValue *clone() const override { return new NumRels(*this); }

  virtual bool isTop() const override { return Rel_ == TOP; }
  virtual bool isBottom() const override { return Rel_ == BOTTOM; }
  virtual void resetToBottom() override { Rel_ = BOTTOM; }

  virtual bool isJoinableWith(const AbstractValue &other) const override {
    auto *other_val = static_cast<const NumRels *>(&other);
    if (other_val) {
      if (other_val->IsSigned_ == IsSigned_ && other_val->Left_ == Left_ &&
          other_val->Right_ == Right_) {
        return true;
      }
    }
    return false;
  }

  Expression left() const { return Left_; }
  Expression right() const { return Right_; }
  uint8_t rel() const { return Rel_; }

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    archive(Left_, Right_);
    archive(IsSigned_);
    archive(Rel_);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void load_and_construct(Archive &archive,
                                 cereal::construct<NumRels> &construct) {
    auto &fctx = cereal::get_user_data<FunctionContext>(archive);
    auto left = Expression::LoadFrom(archive);
    auto right = Expression::LoadFrom(archive);
    bool is_signed;
    archive(is_signed);
    construct(fctx, left, right, is_signed);
    archive(construct->Rel_);
  }
#endif
};
} // namespace domains
} // namespace symabs_ai

#ifdef ENABLE_DYNAMIC
CEREAL_REGISTER_TYPE(symabs_ai::domains::NumRels);
#endif
