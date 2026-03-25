#pragma once

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/Expression.h"
#include "Verification/SymAbsAI/Core/ResultStore.h"
#include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

namespace symabs_ai {
namespace domains {
template <typename T> class Wrapper : public AbstractValue {
private:
  unique_ptr<AbstractValue> Value_;

public:
  Wrapper(unique_ptr<AbstractValue> &&avalue) : Value_(std::move(avalue)) {
    assert(static_cast<T *>(Value_.get()) != nullptr);
  }

  T &value() { return *static_cast<T *>(Value_.get()); }
  const T &value() const { return *static_cast<const T *>(Value_.get()); }

  virtual void prettyPrint(PrettyPrinter &out) const override {
    Value_->prettyPrint(out);
  }

  virtual bool joinWith(const AbstractValue &other) override {
    auto *spec = static_cast<const Wrapper<T> *>(&other);
    assert(spec);
    return Value_->joinWith(*spec->Value_.get());
  }

  virtual bool meetWith(const AbstractValue &other) override {
    auto *spec = static_cast<const Wrapper<T> *>(&other);
    assert(spec);
    return Value_->meetWith(*spec->Value_.get());
  }

  virtual bool updateWith(const ConcreteState &cstate) override {
    return Value_->updateWith(cstate);
  }

  virtual z3::expr toFormula(const ValueMapping &vmap,
                             z3::context &ctx) const override {
    return Value_->toFormula(vmap, ctx);
  }

  virtual void havoc() override { Value_->havoc(); }
  virtual void resetToBottom() override { Value_->resetToBottom(); }
  virtual bool isTop() const override { return Value_->isTop(); }
  virtual bool isBottom() const override { return Value_->isBottom(); }

  virtual void widen() override { Value_->widen(); }

  virtual AbstractValue *clone() const override {
    return new Wrapper(unique_ptr<AbstractValue>(Value_->clone()));
  }

  virtual bool isJoinableWith(const AbstractValue &av_other) const override {
    auto *other = static_cast<const Wrapper<T> *>(&av_other);
    if (other == nullptr)
      return false;
    return Value_->isJoinableWith(*other->Value_);
  }

  virtual void gatherFlattenedSubcomponents(
      std::vector<const AbstractValue *> *result) const override {
    Value_->gatherFlattenedSubcomponents(result);
  }

  virtual void abstractConsequence(const AbstractValue &other) override {
    Value_->abstractConsequence(other);
  }

  virtual bool operator<=(const AbstractValue &av_other) const override {
    auto *other = static_cast<const Wrapper<T> *>(&av_other);
    assert(other != nullptr);
    return *Value_ <= *other->Value_;
  }

  virtual bool operator==(const AbstractValue &av_other) const override {
    auto *other = static_cast<const Wrapper<T> *>(&av_other);
    if (other == nullptr)
      return false;

    return *Value_ == *(other->Value_);
  }
};

template <typename This, typename Base> class Cut : public Wrapper<Base> {
  // Implemented in This:
  //     bool isInAllowedState()

protected:
  Cut(unique_ptr<AbstractValue> &&avalue) : Wrapper<Base>(std::move(avalue)) {}

public:
  virtual bool joinWith(const AbstractValue &other) override {
    bool changed = Wrapper<Base>::joinWith(other);
    if (changed && !static_cast<This *>(this)->isInAllowedState())
      static_cast<AbstractValue *>(this)->havoc();
    return changed;
  }

  virtual bool meetWith(const AbstractValue &other) override {
    bool changed = Wrapper<Base>::meetWith(other);
    if (changed && !static_cast<This *>(this)->isInAllowedState())
      static_cast<AbstractValue *>(this)->resetToBottom();
    return changed;
  }

  virtual bool updateWith(const ConcreteState &cstate) override {
    bool changed = Wrapper<Base>::updateWith(cstate);
    if (changed && !static_cast<This *>(this)->isInAllowedState())
      static_cast<AbstractValue *>(this)->havoc();
    return changed;
  }
};

class If : public AbstractValue {
private:
  Expression Condition_;
  unique_ptr<AbstractValue> Value_;

public:
  If(const Expression &cond, unique_ptr<AbstractValue> &&value)
      : Condition_(cond), Value_(std::move(value)) {}

  virtual void prettyPrint(PrettyPrinter &out) const override {
    out << Condition_ << " ==> ";
    Value_->prettyPrint(out);
  }

  virtual bool joinWith(const AbstractValue &av_other) override {
    assert(isJoinableWith(av_other));
    const If &other = static_cast<const If &>(av_other);
    return Value_->joinWith(*other.Value_);
  }

  virtual bool meetWith(const AbstractValue &other) override {
    assert(false && "not implemented");
    return false;
  }

  virtual bool updateWith(const ConcreteState &cstate) override {
    if ((uint64_t)Condition_.eval(cstate) != 0)
      return Value_->updateWith(cstate);
    else
      return false;
  }

  virtual z3::expr toFormula(const ValueMapping &vmap,
                             z3::context &ctx) const override {
    return !Condition_.toFormula(vmap) || Value_->toFormula(vmap, ctx);
  }

  virtual void havoc() override { Value_->havoc(); }
  virtual void resetToBottom() override { Value_->resetToBottom(); }
  virtual bool isTop() const override { return Value_->isTop(); }
  virtual bool isBottom() const override { return Value_->isBottom(); }

  virtual bool isJoinableWith(const AbstractValue &av_other) const override {
    auto *other = static_cast<decltype(this)>(&av_other);
    if (other == nullptr)
      return false;

    return repr(Condition_) == repr(other->Condition_) &&
           Value_->isJoinableWith(*other->Value_);
  }

  virtual void widen() override { Value_->widen(); }

  virtual AbstractValue *clone() const override {
    return new If(Condition_, unique_ptr<AbstractValue>(Value_->clone()));
  }

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    archive(Condition_);
    archive(Value_);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void load_and_construct(Archive &archive,
                                 cereal::construct<If> &construct) {
    Expression cond = Expression::LoadFrom(archive);
    unique_ptr<AbstractValue> val;
    archive(val);
    construct(cond, std::move(val));
  }
#endif
};
} // namespace domains
} // namespace symabs_ai

#ifdef ENABLE_DYNAMIC
CEREAL_REGISTER_TYPE(symabs_ai::domains::If);
#endif
