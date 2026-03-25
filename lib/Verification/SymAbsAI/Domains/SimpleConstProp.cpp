#include "Verification/SymAbsAI/Domains/SimpleConstProp.h"

#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
// #include "Verification/SymAbsAI/Core/ParamStrategy.h"
#include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"
#include "Verification/SymAbsAI/Utils/Utils.h"
// #include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

// #include <vector>
#include <z3++.h>

using namespace symabs_ai;
using namespace domains;

bool SimpleConstProp::joinWith(const AbstractValue &av_other) {
  auto other = dynamic_cast<const SimpleConstProp &>(av_other);

  if (isTop())
    return false;

  if (other.isTop()) {
    havoc();
    return false;
  }

  // at this point none of the abstract values are top

  if (other.isBottom())
    return false;

  if (isBottom()) {
    Top_ = other.Top_;
    Bottom_ = other.Bottom_;
    Constant_ = other.Constant_;
    return true;
  }

  // both abstract values have constant values check if they're equal
  if (eq(z3::expr(Constant_), z3::expr(other.Constant_))) {
    return false;
  } else {
    havoc();
    return true;
  }
}

bool SimpleConstProp::meetWith(const AbstractValue &av_other) {
  auto other = dynamic_cast<const SimpleConstProp &>(av_other);

  if (isBottom())
    return false;

  if (other.isBottom()) {
    resetToBottom();
    return true;
  }

  // at this point none of the abstract values are bottom

  if (other.isTop())
    return false;

  if (isTop()) {
    Top_ = other.Top_;
    Bottom_ = other.Bottom_;
    Constant_ = other.Constant_;
    return true;
  }

  // both abstract values have constant values
  if (eq(z3::expr(Constant_), z3::expr(other.Constant_))) {
    return false;
  } else {
    resetToBottom();
    return true;
  }
}

bool SimpleConstProp::updateWith(const ConcreteState &cstate) {
  SimpleConstProp other(*FunctionContext_, Value_);
  other.Bottom_ = false;
  other.Top_ = false;
  other.Constant_ = cstate[Value_];
  return joinWith(other);
}

z3::expr SimpleConstProp::toFormula(const ValueMapping &vmap,
                                    z3::context &zctx) const {
  z3::expr result = zctx.bool_val(true);

  if (isBottom())
    result = zctx.bool_val(false);
  else if (isTop())
    result = zctx.bool_val(true);
  else
    result = (vmap[Value_] == (z3::expr)Constant_);

  return result;
}

uint64_t SimpleConstProp::getConstValue() const {
  assert(isConst());
  return Constant_;
}

void SimpleConstProp::prettyPrint(PrettyPrinter &out) const {
  out << Value_ << pp::rightarrow;

  if (isTop())
    out << pp::top;
  else if (isBottom())
    out << pp::bottom;
  else
    out << repr(Constant_);
}

void SimpleConstProp::resetToBottom() {
  Top_ = false;
  Bottom_ = true;
}

bool SimpleConstProp::isJoinableWith(const AbstractValue &other) const {
  if (auto *other_val = dynamic_cast<const SimpleConstProp *>(&other)) {
    if (other_val->Value_ == Value_ &&
        other_val->FunctionContext_ == FunctionContext_) {

      return true;
    }
  }
  return false;
}

namespace {
DomainConstructor::Register _("SimpleConstProp",
                              "classic constant propagataion lattice",
                              SimpleConstProp::New);

} // namespace
