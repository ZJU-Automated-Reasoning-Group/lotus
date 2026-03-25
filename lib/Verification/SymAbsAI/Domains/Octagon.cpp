// TODO: by LLM; to be checked.
#include "Verification/SymAbsAI/Domains/Octagon.h"

#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/ParamStrategy.h"
// #include "Verification/SymAbsAI/Domains/Product.h"
#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"
// #include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <algorithm>

#include <z3++.h>

using namespace symabs_ai;
using namespace symabs_ai::domains;
using std::unique_ptr;

bool Octagon::isInconsistent() const {
  // Check basic contradictions
  // c[0] + c[1] < 0  means x-y <= c0 and -x+y <= c1, so 0 <= c0+c1, violation
  // if negative
  if (C_[0] != INF && C_[1] != INF && C_[0] + C_[1] < 0)
    return true;
  if (C_[2] != INF && C_[3] != INF && C_[2] + C_[3] < 0)
    return true;
  return false;
}

void Octagon::checkConsistency() {
  if (isInconsistent() && !Bottom_) {
    resetToBottom();
  }
}

bool Octagon::isJoinableWith(const AbstractValue &other) const {
  if (auto *o = dynamic_cast<const Octagon *>(&other))
    return o->X_ == X_ && o->Y_ == Y_;
  return false;
}

bool Octagon::joinWith(const AbstractValue &av_other) {
  auto &other = dynamic_cast<const Octagon &>(av_other);
  assert(isJoinableWith(other));

  if (isBottom()) {
    Top_ = other.Top_;
    Bottom_ = other.Bottom_;
    for (int i = 0; i < 4; ++i)
      C_[i] = other.C_[i];
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

  // Join: take max of each bound (weaker constraint)
  bool changed = false;
  for (int i = 0; i < 4; ++i) {
    int64_t new_c = std::max(C_[i], other.C_[i]);
    if (new_c != C_[i]) {
      C_[i] = new_c;
      changed = true;
    }
  }

  return changed;
}

bool Octagon::meetWith(const AbstractValue &av_other) {
  auto &other = dynamic_cast<const Octagon &>(av_other);
  assert(isJoinableWith(other));

  if (isTop()) {
    Top_ = other.Top_;
    Bottom_ = other.Bottom_;
    for (int i = 0; i < 4; ++i)
      C_[i] = other.C_[i];
    return !other.isTop();
  }
  if (other.isTop())
    return false;

  if (other.isBottom()) {
    bool changed = !isBottom();
    resetToBottom();
    return changed;
  }

  // Meet: take min of each bound (stronger constraint)
  bool changed = false;
  for (int i = 0; i < 4; ++i) {
    int64_t new_c = std::min(C_[i], other.C_[i]);
    if (new_c != C_[i]) {
      C_[i] = new_c;
      changed = true;
    }
  }

  checkConsistency();
  return changed;
}

bool Octagon::updateWith(const ConcreteState &cstate) {
  unsigned bw = Fctx_.sortForType(X_->getType()).bv_size();
  assert(bw > 0 && bw <= 64);
  uint64_t mask = (bw == 64) ? ~0ULL : ((1ULL << bw) - 1ULL);

  auto sign_extend = [&](uint64_t v) -> int64_t {
    v &= mask;
    if (bw < 64) {
      uint64_t sign = 1ULL << (bw - 1);
      if (v & sign)
        v |= ~mask;
    }
    return (int64_t)v;
  };

  uint64_t x_u = ((uint64_t)cstate[X_]) & mask;
  uint64_t y_u = ((uint64_t)cstate[Y_]) & mask;

  int64_t vals[4];
  vals[0] = sign_extend((x_u - y_u) & mask);
  vals[1] = sign_extend((y_u - x_u) & mask);
  vals[2] = sign_extend((x_u + y_u) & mask);
  vals[3] = sign_extend((0ULL - x_u - y_u) & mask);

  if (isBottom()) {
    Bottom_ = false;
    Top_ = false;
    for (int i = 0; i < 4; ++i) {
      C_[i] = vals[i];
    }
    return true;
  }

  if (isTop()) {
    Top_ = false;
    for (int i = 0; i < 4; ++i) {
      C_[i] = vals[i];
    }
    return true;
  }

  bool changed = false;
  for (int i = 0; i < 4; ++i) {
    if (vals[i] > C_[i]) {
      C_[i] = vals[i];
      changed = true;
    }
  }

  return changed;
}

z3::expr Octagon::toFormula(const ValueMapping &vmap, z3::context &ctx) const {
  if (isTop())
    return ctx.bool_val(true);
  if (isBottom())
    return ctx.bool_val(false);

  unsigned bw = Fctx_.sortForType(X_->getType()).bv_size();
  z3::expr x = vmap[X_];
  z3::expr y = vmap[Y_];
  z3::expr result = ctx.bool_val(true);

  if (C_[0] != INF) {
    z3::expr c0 = ctx.bv_val((uint64_t)C_[0], bw);
    result = result && z3::sle(x - y, c0);
  }

  if (C_[1] != INF) {
    z3::expr c1 = ctx.bv_val((uint64_t)C_[1], bw);
    result = result && z3::sle(y - x, c1);
  }

  if (C_[2] != INF) {
    z3::expr c2 = ctx.bv_val((uint64_t)C_[2], bw);
    result = result && z3::sle(x + y, c2);
  }

  if (C_[3] != INF) {
    z3::expr c3 = ctx.bv_val((uint64_t)C_[3], bw);
    result = result && z3::sle(ctx.bv_val(0, bw) - x - y, c3);
  }

  return result;
}

void Octagon::prettyPrint(PrettyPrinter &out) const {
  if (isTop()) {
    out << pp::top;
    return;
  }
  if (isBottom()) {
    out << pp::bottom;
    return;
  }

  out << "Oct(" << X_ << ", " << Y_ << "): ";
  bool first = true;

  if (C_[0] != INF) {
    if (!first)
      out << " & ";
    out << X_ << " - " << Y_ << " <= " << C_[0];
    first = false;
  }
  if (C_[1] != INF) {
    if (!first)
      out << " & ";
    out << Y_ << " - " << X_ << " <= " << C_[1];
    first = false;
  }
  if (C_[2] != INF) {
    if (!first)
      out << " & ";
    out << X_ << " + " << Y_ << " <= " << C_[2];
    first = false;
  }
  if (C_[3] != INF) {
    if (!first)
      out << " & ";
    out << "-" << X_ << " - " << Y_ << " <= " << C_[3];
    first = false;
  }

  if (first) {
    out << pp::top;
  }
}

namespace {
// Wrapper to match alt_ffunc_0 signature
std::unique_ptr<AbstractValue>
OctagonFactory(const FunctionContext &fctx, llvm::BasicBlock *bb, bool after) {
  return params::ForNonPointerPairs<Octagon>(fctx, bb, after, false);
}

DomainConstructor::Register _octagon("Octagon",
                                     "octagonal constraint domain (±x ± y ≤ c)",
                                     OctagonFactory);
} // namespace
