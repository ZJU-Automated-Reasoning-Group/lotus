#include "Verification/SymAbsAI/Domains/Predicates.h"

#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Expression.h"
// #include "Verification/SymAbsAI/Core/FloatingPointModel.h"
// #include "Verification/SymAbsAI/Core/ParamStrategy.h"
// #include "Verification/SymAbsAI/Core/repr.h"
// #include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

using namespace symabs_ai;
using namespace domains;

namespace // anonymous
{
Predicates::pred_t equality_pred =
    (Predicates::pred_t)([](const Expression &left, const Expression &right) {
      return left.equals(right);
    });

unique_ptr<AbstractValue> NewEquality(const Expression &left,
                                      const Expression &right,
                                      const DomainConstructor::args &args) {
  return make_unique<PredicatesWrapper<&equality_pred>>(*args.fctx, left,
                                                        right);
}
} // namespace

bool Predicates::joinWith(const AbstractValue &av_other) {
  auto other = dynamic_cast<const Predicates &>(av_other);
  if (other.isBottom()) {
    return false;
  }
  if (isTop()) {
    return false;
  }
  if (isBottom()) {
    Val_ = other.getValue();
    return true; // other != BOTTOM is checked above
  }
  if (other.isTop()) {
    Val_ = TOP;
    return true; // this != TOP is checked above
  }
  if (other.Val_ == Val_) {
    return false;
  } else {
    Val_ = TOP;
    return true;
  }
}

bool Predicates::meetWith(const AbstractValue &av_other) {
  auto other = dynamic_cast<const Predicates &>(av_other);
  if (other.isTop()) {
    return false;
  }
  if (isBottom()) {
    return false;
  }
  if (isTop()) {
    Val_ = other.getValue();
    return true; // other != TOP is checked above
  }
  if (other.isBottom()) {
    Val_ = BOTTOM;
    return true; // this != BOTTOM is checked above
  }
  if (other.Val_ == Val_) {
    return false;
  } else {
    Val_ = BOTTOM;
    return true;
  }
}

bool Predicates::updateWith(const ConcreteState &cstate) {
  if (isTop()) {
    return false;
  }
  uint64_t res = (uint64_t)Predicate_.eval(cstate);
  if (res) {
    if (Val_ == BOTTOM) {
      Val_ = TRUE;
      return true;
    } else if (Val_ == TRUE) {
      return false;
    }
  } else {
    if (Val_ == BOTTOM) {
      Val_ = FALSE;
      return true;
    } else if (Val_ == FALSE) {
      return false;
    }
  }
  Val_ = TOP;
  return true;
}

z3::expr Predicates::toFormula(const ValueMapping &vmap,
                               z3::context &zctx) const {
  assert((Z3_context)zctx == FunctionContext_.getZ3());
  switch (Val_) {
  case BOTTOM:
    return zctx.bool_val(false);
  case TOP:
    return zctx.bool_val(true);
  case TRUE:
    return Predicate_.toFormula(vmap);
  case FALSE:
    return !Predicate_.toFormula(vmap);
  }
  return zctx.bool_val(false);
}

void Predicates::prettyPrint(PrettyPrinter &out) const {
  out << Predicate_ << " : ";

  switch (Val_) {
  case BOTTOM:
    out << pp::bottom;
    break;
  case TOP:
    out << pp::top;
    break;
  case TRUE:
    out << "always TRUE";
    break;
  case FALSE:
    out << "always FALSE";
    break;
  }
}

bool Predicates::isJoinableWith(const AbstractValue &other) const {
  if (auto *other_val = dynamic_cast<const Predicates *>(&other)) {
    if (other_val->Predicate_ == Predicate_) {
      return true;
    }
  }
  return false;
}

namespace {
DomainConstructor::Register
    Equality("Equality",
             "predicate based domain of equalities between numerical scalars",
             NewEquality);
} // namespace
