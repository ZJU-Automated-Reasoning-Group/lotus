#pragma once

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Expression.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/ResultStore.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <map>

#include <z3++.h>

using namespace symabs_ai;

namespace symabs_ai {
namespace domains {
class BitMask : public AbstractValue {
private:
  /**
   * Unset bits in Zeros_ must be unset in every represented value.
   */
  uint64_t Zeros_;

  /**
   * Set bits in Ones_ must be set in every represented value.
   */
  uint64_t Ones_;

  const FunctionContext &FunctionContext_;
  RepresentedValue Left_;
  RepresentedValue Right_;
  int Bitwidth_;

  BitMask(const BitMask &) = default;

  void assertValid() const;

public:
  BitMask(const FunctionContext &fctx, RepresentedValue left,
          RepresentedValue right = RepresentedValue());

  static unique_ptr<AbstractValue>
  NewSingle(const Expression &expr, const DomainConstructor::args &args) {
    return std::move(
        make_unique<BitMask>(*args.fctx, expr.asRepresentedValue()));
  }

  static unique_ptr<AbstractValue>
  NewRelational(const Expression &left, const Expression &right,
                const DomainConstructor::args &args) {
    return std::move(make_unique<BitMask>(*args.fctx, left.asRepresentedValue(),
                                          right.asRepresentedValue()));
  }

  virtual bool joinWith(const AbstractValue &av_other) override;
  virtual bool meetWith(const AbstractValue &av_other) override;
  virtual bool updateWith(const ConcreteState &state) override;
  virtual z3::expr toFormula(const ValueMapping &vmap,
                             z3::context &zctx) const override;
  virtual void havoc() override;
  virtual void prettyPrint(PrettyPrinter &out) const override;

  virtual AbstractValue *clone() const override { return new BitMask(*this); }

  virtual bool isTop() const override;
  virtual bool isBottom() const override { return (~Zeros_ & Ones_) != 0; }

  RepresentedValue getLeft() { return Left_; }
  RepresentedValue getRight() { return Right_; }

  virtual void resetToBottom() override;
  virtual bool isJoinableWith(const AbstractValue &other) const override;

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    archive(Left_, Right_, Zeros_, Ones_);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void load_and_construct(Archive &archive,
                                 cereal::construct<BitMask> &construct) {
    RepresentedValue left, right;
    archive(left, right);
    auto &fctx = cereal::get_user_data<FunctionContext>(archive);
    construct(fctx, left, right);
    archive(construct->Zeros_, construct->Ones_);
  }
#endif
};
} // namespace domains
} // namespace symabs_ai

#ifdef ENABLE_DYNAMIC
CEREAL_REGISTER_TYPE(symabs_ai::domains::BitMask);
#endif
