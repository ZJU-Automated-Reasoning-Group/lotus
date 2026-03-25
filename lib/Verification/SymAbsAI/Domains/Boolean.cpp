#include "Verification/SymAbsAI/Domains/Boolean.h"

#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

namespace symabs_ai {
namespace domains {
bool BooleanValue::joinWith(const AbstractValue &av_other) {
  const auto *other = dynamic_cast<const BooleanValue *>(&av_other);
  assert(other);
  if (other->isBottom()) {
    return false;
  }
  if (isTop()) {
    return false;
  }
  if (isBottom()) {
    Val_ = other->Val_;
    return true; // other != BOTTOM is checked above
  }
  if (other->isTop()) {
    Val_ = TOP;
    return true; // this != TOP is checked above
  }
  if (other->Val_ == Val_) {
    return false;
  } else {
    Val_ = TOP;
    return true;
  }
}

bool BooleanValue::meetWith(const AbstractValue &av_other) {
  const auto *other = dynamic_cast<const BooleanValue *>(&av_other);
  assert(other);
  if (other->isTop()) {
    return false;
  }
  if (isBottom()) {
    return false;
  }
  if (isTop()) {
    Val_ = other->Val_;
    return true; // other != TOP is checked above
  }
  if (other->isBottom()) {
    Val_ = BOTTOM;
    return true; // this != BOTTOM is checked above
  }
  if (other->Val_ == Val_) {
    return false;
  } else {
    Val_ = BOTTOM;
    return true;
  }
}

z3::expr BooleanValue::toFormula(const ValueMapping &vmap,
                                 z3::context &zctx) const {

  assert((Z3_context)zctx == Fctx_.getZ3());
  switch (Val_) {
  case BOTTOM:
    return zctx.bool_val(false);
  case TOP:
    return zctx.bool_val(true);
  case TRUE:
    return makePredicate(vmap);
  case FALSE:
    return !makePredicate(vmap);
  }
  return zctx.bool_val(false);
}

bool BooleanValue::updateWith(const ConcreteState &cstate) {
  if (isTop()) {
    return false;
  }

  const z3::model *model = cstate.getModel();
  if (model == nullptr) {
    // In this case, we are in a dynamic analysis and therefore we cannot
    // do anything useful here.
    // However, sound over-approximation is also not required, so we keep
    // the current value.
    return false;
  }

  z3::expr formula = makePredicate(cstate.getValueMapping());
  z3::expr res = model->eval(formula, true);

  if (expr_to_bool(res)) {
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
} // namespace domains
} // namespace symabs_ai
