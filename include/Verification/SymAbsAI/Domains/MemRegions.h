#pragma once

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/Expression.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/MemoryModel.h"
#include "Verification/SymAbsAI/Domains/Boolean.h"
#include "Verification/SymAbsAI/Domains/Combinators.h"
#include "Verification/SymAbsAI/Domains/Product.h"
#include "Verification/SymAbsAI/Domains/SimpleConstProp.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

namespace symabs_ai {
namespace domains {
class NoAlias : public BooleanValue {
private:
  const RepresentedValue Left_;
  const RepresentedValue Right_;
  const memory::BlockModel *MM_;

protected:
  virtual z3::expr makePredicate(const ValueMapping &vmap) const override;

public:
  NoAlias(const FunctionContext &fctx, const RepresentedValue left,
          const RepresentedValue right);

  virtual void prettyPrint(PrettyPrinter &out) const override;

  virtual ~NoAlias() {}

  virtual AbstractValue *clone() const override { return new NoAlias(*this); }

  virtual bool isJoinableWith(const AbstractValue &other) const override {
    auto *other_val = static_cast<const NoAlias *>(&other);
    if (other_val) {
      if (other_val->Left_ == Left_ && other_val->Right_ == Right_) {
        return true;
      }
    }
    return false;
  }

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    ResultStore::ValueWrapper wrap_val_l(Left_);
    ResultStore::ValueWrapper wrap_val_r(Right_);
    archive(wrap_val_l, wrap_val_r);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void load_and_construct(Archive &archive,
                                 cereal::construct<NoAlias> &construct) {
    ResultStore::ValueWrapper wrap_val_l;
    ResultStore::ValueWrapper wrap_val_r;
    archive(wrap_val_l, wrap_val_r);
    auto &fctx = cereal::get_user_data<FunctionContext>(archive);
    auto *lval = fctx.findRepresentedValue(wrap_val_l);
    auto *rval = fctx.findRepresentedValue(wrap_val_r);
    construct(fctx, *lval, *rval);
  }
#endif
};

class ValidRegion : public BooleanValue {
private:
  const RepresentedValue Ptr_;
  const memory::BlockModel *MM_;

protected:
  z3::expr makePredicate(const ValueMapping &vmap) const override;

public:
  ValidRegion(const FunctionContext &fctx, const RepresentedValue ptr);

  virtual void prettyPrint(PrettyPrinter &out) const override;

  const RepresentedValue &getRepresentedPointer() const { return Ptr_; }

  bool isValid() const { return Val_ == TRUE; };

  virtual AbstractValue *clone() const override {
    return new ValidRegion(*this);
  }

  virtual bool isJoinableWith(const AbstractValue &other) const override {
    auto *other_val = static_cast<const ValidRegion *>(&other);
    if (other_val) {
      if (other_val->Ptr_ == Ptr_) {
        return true;
      }
    }
    return false;
  }

  virtual ~ValidRegion() {}

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    ResultStore::ValueWrapper wrap_val(Ptr_);
    archive(wrap_val);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void load_and_construct(Archive &archive,
                                 cereal::construct<ValidRegion> &construct) {
    ResultStore::ValueWrapper wrap_val;
    archive(wrap_val);
    auto &fctx = cereal::get_user_data<FunctionContext>(archive);
    auto *rval = fctx.findRepresentedValue(wrap_val);
    construct(fctx, *rval);
  }
#endif
};

class ConstantRegion : public SimpleConstProp {
private:
  const memory::BlockModel *MM_;
  const RepresentedValue Ptr_;

public:
  ConstantRegion(const FunctionContext &fctx, RepresentedValue value);

  virtual bool updateWith(const ConcreteState &cstate) override;

  virtual z3::expr toFormula(const ValueMapping &vmap,
                             z3::context &zctx) const override;

  virtual void prettyPrint(PrettyPrinter &out) const override;

  virtual bool isJoinableWith(const AbstractValue &other) const override;

  virtual AbstractValue *clone() const override;

  virtual ~ConstantRegion() {}

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    ResultStore::ValueWrapper wrap_val(Ptr_);
    archive(wrap_val);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void load_and_construct(Archive &archive,
                                 cereal::construct<ConstantRegion> &construct) {
    ResultStore::ValueWrapper wrap_val;
    archive(wrap_val);
    auto &fctx = cereal::get_user_data<FunctionContext>(archive);
    auto *rval = fctx.findRepresentedValue(wrap_val);
    construct(fctx, *rval);
  }
#endif
};

class VariableRegion : public BooleanValue {
private:
  const RepresentedValue Ptr_;
  const Expression Expr_;
  const Expression Fact_;
  const memory::BlockModel *MM_;

protected:
  virtual z3::expr makePredicate(const ValueMapping &vmap) const override;

public:
  VariableRegion(const FunctionContext &fctx, RepresentedValue ptr,
                 const Expression &expr, const Expression &factor);

  virtual void prettyPrint(PrettyPrinter &out) const override;

  virtual ~VariableRegion() {}

  virtual AbstractValue *clone() const override {
    return new VariableRegion(*this);
  }

  virtual bool isJoinableWith(const AbstractValue &other) const override {
    auto *other_val = static_cast<const VariableRegion *>(&other);
    if (other_val) {
      if (other_val->Expr_ == Expr_ && other_val->Fact_ == Fact_) {
        return true;
      }
    }
    return false;
  }
#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    ResultStore::ValueWrapper wrap_val(Ptr_);
    archive(Expr_, Fact_);
    archive(wrap_val);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void load_and_construct(Archive &archive,
                                 cereal::construct<VariableRegion> &construct) {
    ResultStore::ValueWrapper wrap_val;
    auto expr = Expression::LoadFrom(archive);
    auto fact = Expression::LoadFrom(archive);
    archive(wrap_val);

    auto &fctx = cereal::get_user_data<FunctionContext>(archive);
    auto *ptr = fctx.findRepresentedValue(wrap_val);
    construct(fctx, *ptr, expr, fact);
  }
#endif
};

class MemoryRegion : public Product {
private:
  RepresentedValue Ptr_;

public:
  MemoryRegion(const FunctionContext &fctx, RepresentedValue ptr);

  virtual void prettyPrint(PrettyPrinter &out) const override;

  static unique_ptr<AbstractValue> Create(const FunctionContext &fctx,
                                          llvm::BasicBlock *bb, bool after);

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    assert(Values_.size() <= (size_t)UINT32_MAX);
    uint32_t size = Values_.size();
    archive(size);

    ResultStore::ValueWrapper wrap_val(Ptr_);
    archive(wrap_val);

    for (auto &component : Values_)
      archive(component);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void load_and_construct(Archive &archive,
                                 cereal::construct<MemoryRegion> &construct) {
    uint32_t size;
    archive(size);

    auto &fctx = cereal::get_user_data<FunctionContext>(archive);

    ResultStore::ValueWrapper wrap_val;
    archive(wrap_val);
    auto *ptr = fctx.findRepresentedValue(wrap_val);

    construct(fctx, *ptr);

    for (uint32_t i = 0; i < size; i++) {
      unique_ptr<AbstractValue> component;
      archive(component);
      construct->add(std::move(component));
    }

    construct->finalize();
  }
#endif
};

class RestrictedVarRegion : public Cut<RestrictedVarRegion, VariableRegion> {
private:
  // These exist only for serialization
  const FunctionContext &Fctx_;
  const RepresentedValue Ptr_;
  const Expression Expr_;
  const Expression Fact_;

public:
  RestrictedVarRegion(const FunctionContext &fctx, RepresentedValue ptr,
                      const Expression &expr, const Expression &factor)
      : Cut<RestrictedVarRegion, VariableRegion>(
            make_unique<VariableRegion>(fctx, ptr, expr, factor)),
        Fctx_(fctx), Ptr_(ptr), Expr_(expr), Fact_(factor) {}

public:
  bool isInAllowedState() {
    return value().isTop() || value().isBottom() ||
           value().getValue() == BooleanValue::TRUE;
  }

  virtual AbstractValue *clone() const {
    auto *res = new RestrictedVarRegion(Fctx_, Ptr_, Expr_, Fact_);
    res->joinWith(*this);
    return res;
  }

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    ResultStore::ValueWrapper wrap_val(Ptr_);
    archive(Expr_, Fact_);
    archive(wrap_val);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void
  load_and_construct(Archive &archive,
                     cereal::construct<RestrictedVarRegion> &construct) {
    ResultStore::ValueWrapper wrap_val;
    auto expr = Expression::LoadFrom(archive);
    auto fact = Expression::LoadFrom(archive);
    archive(wrap_val);

    auto &fctx = cereal::get_user_data<FunctionContext>(archive);
    auto *ptr = fctx.findRepresentedValue(wrap_val);
    construct(fctx, *ptr, expr, fact);
  }
#endif
};
} // namespace domains
} // namespace symabs_ai

#ifdef ENABLE_DYNAMIC
CEREAL_REGISTER_TYPE(symabs_ai::domains::ValidRegion);
CEREAL_REGISTER_TYPE(symabs_ai::domains::NoAlias);
CEREAL_REGISTER_TYPE(symabs_ai::domains::VariableRegion);
CEREAL_REGISTER_TYPE(symabs_ai::domains::RestrictedVarRegion);
CEREAL_REGISTER_TYPE(symabs_ai::domains::ConstantRegion);
CEREAL_REGISTER_TYPE(symabs_ai::domains::MemoryRegion);
#endif
