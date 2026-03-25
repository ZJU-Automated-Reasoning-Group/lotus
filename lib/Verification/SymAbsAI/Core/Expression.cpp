/**
 * @file Expression.cpp
 * @brief Implementation of Expression hierarchy for representing program
 * expressions.
 *
 * Expressions represent program values and operations that can be evaluated in
 * both abstract (as Z3 formulas) and concrete (as ConcreteState values)
 * contexts. The hierarchy includes RVExpression (for represented values),
 * ConstantExpression (for constants), and various arithmetic/logical operation
 * expressions.
 *
 * @author rainoftime
 */
#include "Verification/SymAbsAI/Core/Expression.h"

#include "Verification/SymAbsAI/Core/RepresentedValue.h"
#include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <utility>

namespace symabs_ai {
namespace {
/**
 * @brief Expression representing a RepresentedValue (LLVM value).
 *
 * This is the base case for expressions - it represents a single LLVM value
 * that is tracked by the abstract interpreter.
 */
class RVExpression : public ExpressionBase {
public:
  RepresentedValue RV_; // public for Expression::bits(FunctionContext&)

  RVExpression(RepresentedValue rv) : RV_(rv) {}
  RVExpression() {}

  unsigned bits() const override {
    return RV_->getType()->getIntegerBitWidth();
  }

  z3::expr toFormula(const ValueMapping &vmap) const override {
    return vmap[RV_];
  }

  ConcreteState::Value eval(const ConcreteState &cstate) const override {
    return cstate[RV_];
  }

  void prettyPrint(PrettyPrinter &out) const override { out << repr(RV_); }

  bool operator==(const ExpressionBase &other_eb) const override {
    auto *other = dynamic_cast<const RVExpression *>(&other_eb);
    if (!other)
      return false;

    return ((llvm::Value *)RV_) == other->RV_;
  }

#ifdef ENABLE_DYNAMIC
  template <class Archive> void serialize(Archive &archive) { archive(RV_); }
#endif
};

class ConstantExpression : public ExpressionBase {
  ConcreteState::Value Const_;

public:
  ConstantExpression(ConcreteState::Value x) : Const_(std::move(x)) {}
  ConstantExpression() {}

  unsigned bits() const override { return Const_.bits(); }

  z3::expr toFormula(const ValueMapping &vmap) const override {
    return static_cast<z3::expr>(Const_);
  }

  ConcreteState::Value eval(const ConcreteState &cstate) const override {
    return Const_;
  }

  void prettyPrint(PrettyPrinter &out) const override { out << repr(Const_); }

  bool operator==(const ExpressionBase &other_eb) const override {
    auto *other = dynamic_cast<const ConstantExpression *>(&other_eb);
    if (!other)
      return false;

    return repr(Const_) == repr(other->Const_);
  }

#ifdef ENABLE_DYNAMIC
  template <class Archive> void serialize(Archive &archive) { archive(Const_); }
#endif
};

class BinopExpression : public ExpressionBase {
public:
  enum op_t { ADD, SUB, MUL, ULE, EQ };

private:
  Expression A_, B_;
  op_t Op_;

public:
  BinopExpression(op_t op, const Expression &a, const Expression &b)
      : A_(a), B_(b), Op_(op) {}

  unsigned bits() const override {
    switch (Op_) {
    case ADD:
    case SUB:
    case MUL:
      assert(A_.bits() == B_.bits());
      return A_.bits();

    case EQ:
    case ULE:
      return 1;

    default:
      llvm_unreachable("invalid state");
    }
  }

  z3::expr toFormula(const ValueMapping &vmap) const override {
    z3::expr a = A_.toFormula(vmap);
    z3::expr b = B_.toFormula(vmap);

    switch (Op_) {
    case ADD:
      return a + b;

    case SUB:
      return a - b;

    case MUL:
      return a * b;

    case ULE:
      return z3::ule(a, b);

    case EQ:
      return a == b;

    default:
      llvm_unreachable("invalid state");
    }
  }

  ConcreteState::Value eval(const ConcreteState &cstate) const override {
    auto a = A_.eval(cstate);
    auto b = B_.eval(cstate);
    assert(a.bits() == b.bits());

    if (a.bits() <= 64) {
      uint64_t ua = a;
      uint64_t ub = b;
      z3::context *ctx = &a.getZ3();
      uint64_t one = 1;
      uint64_t mask = a.bits() < 64 ? (one << a.bits()) - one : ~0L;

      switch (Op_) {
      case ADD:
        return ConcreteState::Value(ctx, (ua + ub) & mask, a.bits());

      case SUB:
        return ConcreteState::Value(ctx, (ua - ub) & mask, a.bits());

      case MUL:
        return ConcreteState::Value(ctx, (ua * ub) & mask, a.bits());

      case ULE:
        return ConcreteState::Value(ctx, ua <= ub, 1);

      case EQ:
        return ConcreteState::Value(ctx, ua == ub, 1);

      default:
        llvm_unreachable("invalid operator");
      }
    } else {
      if (cstate.getModel() == nullptr) {
        panic("this kind of binary expression is not supported in "
              "dynamic analysis");
      }
      return cstate.getModel()->eval(toFormula(cstate.getValueMapping()));
    }
  }

  void prettyPrint(PrettyPrinter &out) const override {
    A_.prettyPrint(out);

    switch (Op_) {
    case ADD:
      out << " + ";
      break;

    case SUB:
      out << " - ";
      break;

    case MUL:
      out << " * ";
      break;

    case ULE:
      out << " <=(unsigned) ";
      break;

    case EQ:
      out << " == ";
      break;

    default:
      out << " ? ";
      break;
    }

    B_.prettyPrint(out);
  }

  bool operator==(const ExpressionBase &other_eb) const override {
    auto *other = dynamic_cast<const BinopExpression *>(&other_eb);
    if (other) {
      return this->Op_ == other->Op_ && this->A_ == other->A_ &&
             this->B_ == other->B_;
    } else {
      return false;
    }
  }

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    archive(A_, B_, Op_);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void
  load_and_construct(Archive &archive,
                     cereal::construct<BinopExpression> &construct) {
    auto a = Expression::LoadFrom(archive);
    auto b = Expression::LoadFrom(archive);
    op_t op;
    archive(op);
    construct(op, a, b);
  }

#endif
};

class Conversion : public ExpressionBase {
private:
  unsigned Bits_;
  Expression Expr_;
  bool SignExtend_;

public:
  Conversion(unsigned bits, const Expression &expr, bool sign_extend)
      : Bits_(bits), Expr_(expr), SignExtend_(sign_extend) {}

  unsigned bits() const override { return Bits_; }

  z3::expr toFormula(const ValueMapping &vmap) const override {
    z3::expr e = Expr_.toFormula(vmap);
    unsigned e_bits = e.get_sort().bv_size();

    if (e_bits == Bits_)
      return e;

    assert(e_bits < Bits_ && "not implemented");
    if (SignExtend_)
      return z3_ext::sext(Bits_ - e_bits, e);
    else
      return z3_ext::zext(Bits_ - e_bits, e);
  };

  ConcreteState::Value eval(const ConcreteState &cstate) const override {
    auto cval = Expr_.eval(cstate);

    if (Bits_ <= 64 && cval.bits() <= 64) {
      if (!SignExtend_) {
        uint64_t val = cval;
        return ConcreteState::Value(&cval.getZ3(), val, Bits_);
      } else {
        int64_t val = cval;
        return ConcreteState::Value(&cval.getZ3(), val, Bits_);
      }
    } else {
      auto *model = cstate.getModel();
      if (model == nullptr) {
        panic("this kind of conversion is not supported in dynamic "
              "analysis");
      }
      return model->eval(toFormula(cstate.getValueMapping()));
    }
  }

  void prettyPrint(PrettyPrinter &out) const override {
    // conversions are implicit in pretty-printed expressions
    return Expr_.prettyPrint(out);
  }

  bool operator==(const ExpressionBase &other_eb) const override {
    auto *other = dynamic_cast<const Conversion *>(&other_eb);
    if (!other)
      return false;

    return this->Bits_ == other->Bits_ &&
           this->SignExtend_ == other->SignExtend_ &&
           this->Expr_ == other->Expr_;
  }

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    archive(Bits_, Expr_, SignExtend_);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void load_and_construct(Archive &archive,
                                 cereal::construct<Conversion> &construct) {
    unsigned bits;
    archive(bits);
    auto expr = Expression::LoadFrom(archive);
    bool sign_extend;
    archive(sign_extend);
    construct(bits, expr, sign_extend);
  }
#endif
};

class Z3Wrapper : public ExpressionBase {
private:
  std::function<z3::expr(const ValueMapping &vmap)> ToFormula_;
  std::string Repr_;

public:
  template <typename F>
  Z3Wrapper(F f, std::string repr) : ToFormula_(f), Repr_(std::move(repr)) {}

  unsigned bits() const override {
    assert(false && "Not supported!");
    return 0;
  }

  z3::expr toFormula(const ValueMapping &vmap) const override {
    return ToFormula_(vmap);
  }

  ConcreteState::Value eval(const ConcreteState &cstate) const override {
    auto *model = cstate.getModel();
    if (model == nullptr) {
      panic("Z3 expressions are not supported in dynamic analysis");
    }

    return model->eval(ToFormula_(cstate.getValueMapping()));
  }

  void prettyPrint(PrettyPrinter &out) const override { out << Repr_; }

  bool operator==(const ExpressionBase &other_eb) const override {
    auto *other = dynamic_cast<const Z3Wrapper *>(&other_eb);
    if (other)
      return Repr_ == other->Repr_;
    else
      return false;
  }
};
} // namespace

Expression::Expression(const RepresentedValue &rv) {
  Instance_ = make_shared<RVExpression>(rv);
}

Expression::Expression(const ConcreteState::Value &x) {
  Instance_ = make_shared<ConstantExpression>(x);
}

Expression Expression::operator-(const Expression &other) const {
  return Expression(
      make_shared<BinopExpression>(BinopExpression::SUB, *this, other));
}

Expression Expression::operator*(const Expression &other) const {
  return Expression(
      make_shared<BinopExpression>(BinopExpression::MUL, *this, other));
}

Expression Expression::operator+(const Expression &other) const {
  return Expression(
      make_shared<BinopExpression>(BinopExpression::ADD, *this, other));
}

Expression Expression::ule(const Expression &other) const {
  return Expression(
      make_shared<BinopExpression>(BinopExpression::ULE, *this, other));
}

Expression Expression::equals(const Expression &other) const {
  return Expression(
      make_shared<BinopExpression>(BinopExpression::EQ, *this, other));
}

Expression Expression::zeroExtend(unsigned bits) const {
  return Expression(make_shared<Conversion>(bits, *this, false));
}

Expression Expression::signExtend(unsigned bits) const {
  return Expression(make_shared<Conversion>(bits, *this, true));
}

bool Expression::operator==(const ExpressionBase &other_eb) const {
  auto *other = dynamic_cast<const Expression *>(&other_eb);

  if (!other)
    return false;

  const auto *thisInst = Instance_.get();
  const auto *otherInst = other->Instance_.get();
  if (!thisInst || !otherInst)
    return false;

  // Avoid using smart-pointer `operator*` inside `typeid` to keep clang from
  // warning about potential side effects being evaluated.
  if (typeid(*thisInst) != typeid(*otherInst))
    return false;

  return *thisInst == *otherInst;
}

RepresentedValue Expression::asRepresentedValue() const {
  auto *rv_expr = dynamic_cast<RVExpression *>(Instance_.get());
  if (rv_expr == nullptr)
    panic("A represented value expression is required in this context");

  return rv_expr->RV_;
}

unsigned Expression::bits(const FunctionContext &fctx) const {
  // a bit of a hack to get a bit size even for RVExpressions of pointer
  // type
  if (auto *as_rv = dynamic_cast<RVExpression *>(Instance_.get())) {
    return fctx.bitsForType(as_rv->RV_->getType());
  }

  return bits();
}
} // namespace symabs_ai

#ifdef ENABLE_DYNAMIC
CEREAL_REGISTER_TYPE(symabs_ai::RVExpression);
CEREAL_REGISTER_TYPE(symabs_ai::ConstantExpression);
CEREAL_REGISTER_TYPE(symabs_ai::BinopExpression);
CEREAL_REGISTER_TYPE(symabs_ai::Conversion);
#endif
