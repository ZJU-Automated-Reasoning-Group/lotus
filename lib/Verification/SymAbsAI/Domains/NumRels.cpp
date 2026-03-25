#include "Verification/SymAbsAI/Domains/NumRels.h"

#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Expression.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
// #include "Verification/SymAbsAI/Core/ParamStrategy.h"
// #include "Verification/SymAbsAI/Core/RepresentedValue.h"
// #include "Verification/SymAbsAI/Core/ResultStore.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"
// #include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"
#include "Verification/SymAbsAI/Utils/Utils.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <algorithm>
#include <vector>

#include <z3++.h>

namespace symabs_ai {
namespace domains {
bool NumRels::joinWith(const AbstractValue &av_other) {
  auto other = dynamic_cast<const NumRels &>(av_other);
  bool changed = Rel_ != other.Rel_;
  Rel_ = Rel_ | other.Rel_;
  return changed;
}

bool NumRels::meetWith(const AbstractValue &av_other) {
  auto other = dynamic_cast<const NumRels &>(av_other);
  bool changed = Rel_ != other.Rel_;
  Rel_ = Rel_ & other.Rel_;
  return changed;
}

namespace {
// generic version for both signed and unsigned comparisons
template <typename T> void updateRel(T left, T right, uint8_t *rel) {
  if (left < right)
    *rel |= NumRels::LOWER;

  if (left == right)
    *rel |= NumRels::EQUAL;

  if (left > right)
    *rel |= NumRels::GREATER;
}
} // namespace

bool NumRels::updateWith(const ConcreteState &state) {
  ConcreteState::Value left = Left_.eval(state);
  ConcreteState::Value right = Right_.eval(state);
  uint8_t old = Rel_;

  if (IsSigned_)
    updateRel<int64_t>(left, right, &Rel_);
  else
    updateRel<uint64_t>(left, right, &Rel_);

  return old != Rel_;
}

z3::expr NumRels::toFormula(const ValueMapping &vmap, z3::context &zctx) const {
  z3::expr result = zctx.bool_val(true);
  z3::expr left = Left_.toFormula(vmap);
  z3::expr right = Right_.toFormula(vmap);

  // Ensure both expressions are bitvectors with matching bitwidths
  // for comparison operations
  unsigned left_bw = 0, right_bw = 0;

  if (left.is_bv()) {
    left_bw = left.get_sort().bv_size();
  } else if (left.is_bool()) {
    // Convert boolean to 1-bit bitvector
    left = z3::ite(left, zctx.bv_val(1, 1), zctx.bv_val(0, 1));
    left_bw = 1;
  }

  if (right.is_bv()) {
    right_bw = right.get_sort().bv_size();
  } else if (right.is_bool()) {
    // Convert boolean to 1-bit bitvector
    right = z3::ite(right, zctx.bv_val(1, 1), zctx.bv_val(0, 1));
    right_bw = 1;
  }

  // Now both should be bitvectors - adjust bitwidths to match
  if (left_bw != 0 && right_bw != 0 && left_bw != right_bw) {
    // Adjust bitwidths to match - use the larger bitwidth
    unsigned target_bw = std::max(left_bw, right_bw);
    if (left_bw < target_bw) {
      left = z3_ext::zext(target_bw - left_bw, left);
    }
    if (right_bw < target_bw) {
      right = z3_ext::zext(target_bw - right_bw, right);
    }
  }

  if ((Rel_ & EQUAL) == 0)
    result = result && (left != right);

  if (IsSigned_) {
    if ((Rel_ & LOWER) == 0)
      result = result && !(left < right);

    if ((Rel_ & GREATER) == 0)
      result = result && !(left > right);
  } else {
    if ((Rel_ & LOWER) == 0)
      result = result && !(ult(left, right));

    if ((Rel_ & GREATER) == 0)
      result = result && !(ugt(left, right));
  }

  return result;
}

void NumRels::prettyPrint(PrettyPrinter &out) const {
  if (isTop()) {
    out << pp::top;
    return;
  }

  if (isBottom()) {
    out << pp::bottom;
    return;
  }

  static const std::map<uint8_t, std::string> OP_NAMES = {
      {EQUAL, "="},          {LOWER | GREATER, "!="}, {LOWER, "<"},
      {LOWER | EQUAL, "=<"}, {GREATER, ">"},          {GREATER | EQUAL, ">="}};

  out << Left_ << " " << OP_NAMES.at(Rel_);
  if (IsSigned_)
    out << "S";
  out << " " << Right_;
}

const uint8_t NumRels::BOTTOM;
const uint8_t NumRels::LOWER;
const uint8_t NumRels::GREATER;
const uint8_t NumRels::EQUAL;
const uint8_t NumRels::TOP;

namespace {
DomainConstructor NumRels_Unsigned("NumRels.Unsigned",
                                   "unsigned numeric relational domain",
                                   NumRels::NewUnsigned);
DomainConstructor::Register ru(NumRels_Unsigned);

DomainConstructor NumRels_Signed("NumRels.Signed",
                                 "signed numeric relational domain",
                                 NumRels::NewSigned);
DomainConstructor::Register rs(NumRels_Signed);

DomainConstructor::Register NumRels_Zero(
    "NumRels.Zero",
    "signed numeric relational domain for all available non-pointers with 0",
    NumRels::NewZero);

DomainConstructor::Register
    numrels("NumRels", "unsigned and signed numeric relational domain",
            DomainConstructor::product({NumRels_Unsigned, NumRels_Signed}));

} // namespace
} // namespace domains
} // namespace symabs_ai
