// TODO: by LLM; to be checked.
#include "Verification/SymAbsAI/Domains/Zones.h"

#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/ParamStrategy.h"
// #include "Verification/SymAbsAI/Domains/Product.h"
#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"
// #include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <algorithm>
#include <limits>

#include <z3++.h>

using namespace symabs_ai;
using namespace symabs_ai::domains;
using std::unique_ptr;

static const int64_t INF = std::numeric_limits<int64_t>::max();
static const int64_t NINF = std::numeric_limits<int64_t>::min();

bool Zone::isJoinableWith(const AbstractValue &other) const {
  if (auto *o = dynamic_cast<const Zone *>(&other))
    return o->Left_ == Left_ && o->Right_ == Right_;
  return false;
}

bool Zone::tighten(int64_t new_lower, int64_t new_upper) {
  if (isBottom())
    return false;
  if (isTop()) {
    Top_ = false;
    Lower_ = new_lower;
    Upper_ = new_upper;
    checkConsistency();
    return !isBottom();
  }

  int64_t old_lower = Lower_;
  int64_t old_upper = Upper_;

  Lower_ = std::max(Lower_, new_lower);
  Upper_ = std::min(Upper_, new_upper);
  checkConsistency();

  return (Lower_ != old_lower || Upper_ != old_upper) && !isBottom();
}

bool Zone::joinWith(const AbstractValue &av_other) {
  auto &other = dynamic_cast<const Zone &>(av_other);
  assert(isJoinableWith(other));

  if (isBottom()) {
    Top_ = other.Top_;
    Bottom_ = other.Bottom_;
    Upper_ = other.Upper_;
    Lower_ = other.Lower_;
    return !other.isBottom();
  }
  if (other.isBottom())
    return false;

  if (isTop())
    return false;
  if (other.isTop()) {
    bool changed = !isTop();
    havoc();
    return changed;
  }

  // Join: widen bounds to include both
  int64_t new_lower = std::min(Lower_, other.Lower_);
  int64_t new_upper = std::max(Upper_, other.Upper_);

  bool changed = (new_lower != Lower_ || new_upper != Upper_);
  Lower_ = new_lower;
  Upper_ = new_upper;

  return changed;
}

bool Zone::meetWith(const AbstractValue &av_other) {
  auto &other = dynamic_cast<const Zone &>(av_other);
  assert(isJoinableWith(other));

  if (isTop()) {
    Top_ = other.Top_;
    Bottom_ = other.Bottom_;
    Upper_ = other.Upper_;
    Lower_ = other.Lower_;
    return !other.isTop();
  }
  if (other.isTop())
    return false;

  if (other.isBottom()) {
    bool changed = !isBottom();
    resetToBottom();
    return changed;
  }

  // Meet: narrow bounds to intersection
  int64_t new_lower = std::max(Lower_, other.Lower_);
  int64_t new_upper = std::min(Upper_, other.Upper_);

  bool changed = (new_lower != Lower_ || new_upper != Upper_);
  Lower_ = new_lower;
  Upper_ = new_upper;
  checkConsistency();

  return changed;
}

bool Zone::updateWith(const ConcreteState &cstate) {
  int64_t left = (int64_t)cstate[Left_];
  int64_t right = (int64_t)cstate[Right_];
  int64_t diff = left - right;

  if (isBottom()) {
    Bottom_ = false;
    Top_ = false;
    Lower_ = diff;
    Upper_ = diff;
    return true;
  }

  if (isTop()) {
    Top_ = false;
    Lower_ = diff;
    Upper_ = diff;
    return true;
  }

  // Widen bounds if needed to include new concrete value
  int64_t old_lower = Lower_;
  int64_t old_upper = Upper_;

  if (diff < Lower_)
    Lower_ = diff;
  if (diff > Upper_)
    Upper_ = diff;

  return (Lower_ != old_lower || Upper_ != old_upper);
}

z3::expr Zone::toFormula(const ValueMapping &vmap, z3::context &ctx) const {
  if (isTop())
    return ctx.bool_val(true);
  if (isBottom())
    return ctx.bool_val(false);

  unsigned bw = Fctx_.sortForType(Left_->getType()).bv_size();
  z3::expr diff = vmap[Left_] - vmap[Right_];
  z3::expr result = ctx.bool_val(true);

  if (Upper_ != INF) {
    z3::expr ub = ctx.bv_val((uint64_t)Upper_, bw);
    result = result && z3::sle(diff, ub);
  }

  if (Lower_ != NINF) {
    z3::expr lb = ctx.bv_val((uint64_t)Lower_, bw);
    result = result && z3::sge(diff, lb);
  }

  return result;
}

void Zone::prettyPrint(PrettyPrinter &out) const {
  if (isTop()) {
    out << pp::top;
    return;
  }
  if (isBottom()) {
    out << pp::bottom;
    return;
  }

  if (Lower_ == NINF && Upper_ == INF) {
    out << pp::top;
  } else if (Lower_ == Upper_) {
    out << Left_ << " - " << Right_ << " = " << Lower_;
  } else {
    out << Lower_ << " <= " << Left_ << " - " << Right_ << " <= " << Upper_;
  }
}

namespace {
// Wrapper to match alt_ffunc_0 signature
std::unique_ptr<AbstractValue> ZoneFactory(const FunctionContext &fctx,
                                           llvm::BasicBlock *bb, bool after) {
  return params::ForNonPointerPairs<Zone>(fctx, bb, after, false);
}

DomainConstructor::Register _zone("Zones", "difference-bound zone domain (DBM)",
                                  ZoneFactory);
} // namespace
