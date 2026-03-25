#include "Verification/SymAbsAI/Domains/Affine.h"

#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
// #include "Verification/SymAbsAI/Core/ParamStrategy.h"
// #include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Utils/Utils.h"
// #include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <z3++.h>

namespace symabs_ai {
namespace domains {

bool Affine::joinWith(const AbstractValue &av_other) {
  auto other = dynamic_cast<const Affine &>(av_other);

  if (isTop() || other.isTop()) {
    bool changed = !isTop();
    State_ = TOP;
    return changed;
  }

  if (other.isBottom())
    return false;

  if (isBottom()) {
    // other is not top or bottom here (would've been handled in previous
    // cases)
    State_ = VALUE;
    Delta_ = other.Delta_;
    return true;
  }

  // both `this' and `other' are proper values (not top or bottom)
  if (Delta_ == other.Delta_) {
    return false;
  } else {
    State_ = TOP;
    return true;
  }
}

bool Affine::meetWith(const AbstractValue &av_other) {
  auto other = dynamic_cast<const Affine &>(av_other);

  if (isBottom() || other.isBottom()) {
    bool changed = !isBottom();
    State_ = BOTTOM;
    return changed;
  }

  if (other.isTop())
    return false;

  if (isTop()) {
    // other is not top or bottom here (would've been handled in previous
    // cases)
    State_ = VALUE;
    Delta_ = other.Delta_;
    return true;
  }

  // both `this' and `other' are proper values (not top or bottom)
  if (Delta_ == other.Delta_) {
    return false;
  } else {
    State_ = BOTTOM;
    return true;
  }
}

bool Affine::updateWith(const ConcreteState &state) {
  uint64_t left = state[Left_];
  uint64_t right = state[Right_];

  Affine aval(FunctionContext_, Left_, Right_);
  aval.State_ = VALUE;
  aval.Delta_ = (int64_t)(left - right);
  return joinWith(aval);
}

z3::expr Affine::toFormula(const ValueMapping &vmap, z3::context &zctx) const {
  if (isTop())
    return zctx.bool_val(true);

  if (isBottom())
    return zctx.bool_val(false);

  unsigned bw = FunctionContext_.sortForType(Left_->getType()).bv_size();
  z3::expr delta = zctx.bv_val((uint64_t)Delta_, bw);

  return vmap[Left_] == vmap[Right_] + delta;
}

void Affine::havoc() { State_ = TOP; }

void Affine::prettyPrint(PrettyPrinter &out) const {
  if (isTop()) {
    out << pp::top;
    return;
  }

  if (isBottom()) {
    out << pp::bottom;
    return;
  }

  out << Left_ << " = " << Right_;

  if (Delta_ != 0) {
    if (Delta_ > 0)
      out << " + " << Delta_;
    else
      out << " - " << -Delta_; // FIXME this breaks for Delta_ == MININT
  }
}

bool Affine::isJoinableWith(const AbstractValue &other) const {
  if (const auto *other_val = dynamic_cast<const Affine *>(&other)) {
    if (other_val->Left_ == Left_ && other_val->Right_ == Right_) {
      return true;
    }
  }
  return false;
}

namespace {
DomainConstructor::Register
    _("Affine",
      "relational domain of affine equalities between pairs of variables",
      Affine::New);
} // namespace
} // namespace domains
} // namespace symabs_ai
