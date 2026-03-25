// TODO: by LLM; to be checked.
#include "Verification/SymAbsAI/Domains/Congruence.h"

#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/ParamStrategy.h"
#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"

#include <z3++.h>

using namespace symabs_ai;
using namespace symabs_ai::domains;

bool Congruence::joinWith(const AbstractValue &av_other) {
  auto &other = dynamic_cast<const Congruence &>(av_other);
  assert(isJoinableWith(other));

  if (isBottom()) {
    Top_ = other.Top_;
    Bottom_ = other.Bottom_;
    Modulus_ = other.Modulus_;
    Remainder_ = other.Remainder_;
    return !other.isBottom();
  }
  if (other.isBottom())
    return false;

  if (isTop() || other.isTop()) {
    bool changed = !isTop();
    havoc();
    return changed;
  }

  // Both are concrete values: need gcd-based merge
  // If Modulus_ == 0, it's a singleton (constant)
  uint64_t m1 = (Modulus_ == 0) ? 0 : Modulus_;
  uint64_t m2 = (other.Modulus_ == 0) ? 0 : other.Modulus_;

  // Handle singleton cases
  if (m1 == 0 && m2 == 0) {
    // Both are constants
    if (Remainder_ == other.Remainder_) {
      return false; // no change
    } else {
      havoc(); // different constants -> TOP
      return true;
    }
  }

  if (m1 == 0) {
    // This is constant, other is congruence
    if ((Remainder_ - other.Remainder_) % m2 == 0) {
      return false; // constant satisfies congruence
    } else {
      havoc();
      return true;
    }
  }

  if (m2 == 0) {
    // Other is constant, this is congruence
    if ((other.Remainder_ - Remainder_) % m1 == 0) {
      return false; // constant satisfies our congruence
    } else {
      havoc();
      return true;
    }
  }

  // Both are true congruences
  uint64_t new_mod =
      gcd_u64(m1, gcd_u64(m2, (Remainder_ > other.Remainder_)
                                  ? (Remainder_ - other.Remainder_)
                                  : (other.Remainder_ - Remainder_)));
  uint64_t new_rem = Remainder_ % new_mod;

  bool changed = (new_mod != Modulus_ || new_rem != Remainder_);
  Modulus_ = new_mod;
  Remainder_ = new_rem;

  if (Modulus_ == 1) {
    // Everything is 0 mod 1, so TOP
    havoc();
    return true;
  }

  return changed;
}

bool Congruence::meetWith(const AbstractValue &av_other) {
  auto &other = dynamic_cast<const Congruence &>(av_other);
  assert(isJoinableWith(other));

  if (isTop()) {
    Top_ = other.Top_;
    Bottom_ = other.Bottom_;
    Modulus_ = other.Modulus_;
    Remainder_ = other.Remainder_;
    return !other.isTop();
  }
  if (other.isTop())
    return false;

  if (other.isBottom()) {
    resetToBottom();
    return true;
  }

  uint64_t m1 = (Modulus_ == 0) ? 0 : Modulus_;
  uint64_t m2 = (other.Modulus_ == 0) ? 0 : other.Modulus_;

  // Handle singleton cases
  if (m1 == 0 && m2 == 0) {
    // Both constants
    if (Remainder_ != other.Remainder_) {
      resetToBottom();
      return true;
    }
    return false;
  }

  if (m1 == 0) {
    // This is constant, check if it satisfies other congruence
    if ((Remainder_ - other.Remainder_) % m2 != 0) {
      resetToBottom();
      return true;
    }
    return false;
  }

  if (m2 == 0) {
    // Other is constant
    if ((other.Remainder_ - Remainder_) % m1 != 0) {
      resetToBottom();
      return true;
    }
    // Narrow to constant
    Modulus_ = other.Modulus_;
    Remainder_ = other.Remainder_;
    Top_ = other.Top_;
    Bottom_ = other.Bottom_;
    return true;
  }

  // Both are congruences: compute LCM-based meet
  uint64_t g = gcd_u64(m1, m2);
  if ((Remainder_ % g) != (other.Remainder_ % g)) {
    resetToBottom();
    return true;
  }

  // Compute LCM
  uint64_t lcm = (m1 / g) * m2;

  // Find value that satisfies both congruences (extended Euclidean)
  // For simplicity, just use first common value
  uint64_t new_rem = Remainder_;
  bool found = false;
  for (uint64_t k = 0; k < m2 && !found; ++k) {
    uint64_t candidate = Remainder_ + k * m1;
    if ((candidate - other.Remainder_) % m2 == 0) {
      new_rem = candidate % lcm;
      found = true;
      break;
    }
  }

  if (!found) {
    resetToBottom();
    return true;
  }

  bool changed = (lcm != Modulus_ || new_rem != Remainder_);
  Modulus_ = lcm;
  Remainder_ = new_rem;
  return changed;
}

bool Congruence::updateWith(const ConcreteState &cstate) {
  uint64_t val = cstate[Value_];

  if (isBottom()) {
    Bottom_ = false;
    Top_ = false;
    Modulus_ = 0; // singleton
    Remainder_ = val;
    return true;
  }

  if (isTop()) {
    Top_ = false;
    Modulus_ = 0; // singleton
    Remainder_ = val;
    return true;
  }

  // Have existing value(s)
  if (Modulus_ == 0) {
    // Currently a singleton
    if (Remainder_ == val) {
      return false; // no change
    } else {
      // Two different constants -> compute gcd
      uint64_t diff =
          (Remainder_ > val) ? (Remainder_ - val) : (val - Remainder_);
      Modulus_ = diff;
      Remainder_ = val % Modulus_;
      if (Modulus_ == 0)
        Modulus_ = 1; // fallback
      return true;
    }
  }

  // Have congruence, add new value
  if ((val - Remainder_) % Modulus_ == 0) {
    return false; // already satisfied
  }

  uint64_t diff = (val > Remainder_) ? (val - Remainder_) : (Remainder_ - val);
  uint64_t new_mod = gcd_u64(Modulus_, diff);
  uint64_t new_rem = val % new_mod;

  bool changed = (new_mod != Modulus_ || new_rem != Remainder_);
  Modulus_ = new_mod;
  Remainder_ = new_rem;

  return changed;
}

z3::expr Congruence::toFormula(const ValueMapping &vmap,
                               z3::context &ctx) const {
  if (isTop())
    return ctx.bool_val(true);
  if (isBottom())
    return ctx.bool_val(false);

  auto val = vmap[Value_];
  unsigned bw = Fctx_.sortForType(Value_->getType()).bv_size();
  z3::expr rem = ctx.bv_val(Remainder_, bw);

  if (Modulus_ == 0 || Modulus_ == 1) {
    // Singleton or trivial
    return val == rem;
  }

  z3::expr mod = ctx.bv_val(Modulus_, bw);
  // (val - rem) % mod == 0
  z3::expr diff = val - rem;
  return z3::urem(diff, mod) == ctx.bv_val(0, bw);
}

bool Congruence::isJoinableWith(const AbstractValue &other) const {
  if (auto *o = dynamic_cast<const Congruence *>(&other))
    return o->Value_ == Value_;
  return false;
}

void Congruence::prettyPrint(PrettyPrinter &out) const {
  out << Value_ << pp::rightarrow;
  if (isTop())
    out << pp::top;
  else if (isBottom())
    out << pp::bottom;
  else if (Modulus_ == 0)
    out << Remainder_;
  else
    out << Remainder_ << " (mod " << Modulus_ << ")";
}

namespace {
DomainConstructor::Register _("Congruence",
                              "value congruence domain (x ≡ r mod m)",
                              params::ForNonPointers<Congruence>);
} // namespace
