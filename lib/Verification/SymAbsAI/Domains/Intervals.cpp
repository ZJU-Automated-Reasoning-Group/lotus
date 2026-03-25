#include "Verification/SymAbsAI/Domains/Intervals.h"

#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/ParamStrategy.h"
// #include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <algorithm>

#include <z3++.h>

using namespace symabs_ai;
using namespace domains;
using std::unique_ptr;

void Interval::havoc() {
  Bottom_ = false;
  Lower_ = Min_;
  Upper_ = Max_;
}

bool Interval::isTop() const { return (Lower_ == Min_) && (Upper_ == Max_); }

bool Interval::joinWith(const AbstractValue &av_other) {
  auto other = dynamic_cast<const Interval &>(av_other);

  if (Bottom_) {
    if (!other.isBottom()) {
      Bottom_ = false;
      Lower_ = other.Lower_;
      Upper_ = other.Upper_;
      assert(checkValid());
      return true;
    }
    return false;
  } else {
    if (other.Lower_ < Lower_ || other.Upper_ > Upper_) {
      Lower_ = std::min(other.Lower_, Lower_);
      Upper_ = std::max(other.Upper_, Upper_);
      assert(checkValid());
      return true;
    }
    return false;
  }
}

bool Interval::meetWith(const AbstractValue &av_other) {
  auto other = dynamic_cast<const Interval &>(av_other);

  if (Bottom_)
    return false;
  if (other.Bottom_) {
    Bottom_ = true;
    Lower_ = 0;
    Upper_ = 0;
    assert(checkValid());
    return true;
  }
  if (other.Lower_ > Lower_ || other.Upper_ < Upper_) {
    Lower_ = std::max(other.Lower_, Lower_);
    Upper_ = std::min(other.Upper_, Upper_);
    if (!checkValid()) {
      Bottom_ = true;
      Lower_ = 0;
      Upper_ = 0;
    }
    assert(checkValid());
    return true;
  }
  return false;
}

bool Interval::updateWith(const ConcreteState &cstate) {
  int64_t val = (int64_t)cstate[Value_];
  if (Bottom_) {
    Bottom_ = false;
    Lower_ = val;
    Upper_ = val;
    // Save current bounds as previous for next widening comparison
    PrevLower_ = Lower_;
    PrevUpper_ = Upper_;
    assert(checkValid());
    return true;
  } else {
    // Save current bounds before potentially changing them
    PrevLower_ = Lower_;
    PrevUpper_ = Upper_;

    if (val < Lower_) {
      Lower_ = val;
      assert(checkValid());
      return true;
    }
    if (val > Upper_) {
      Upper_ = val;
      assert(checkValid());
      return true;
    }
    // no change to bounds
    return false;
  }
}

z3::expr Interval::toFormula(const ValueMapping &vmap,
                             z3::context &zctx) const {
  z3::expr result = zctx.bool_val(true);

  if (Bottom_) {
    result = zctx.bool_val(false);
  } else if (isTop()) {
    // not really necessary but maybe easier to read
    result = zctx.bool_val(true);
  } else {
    unsigned bw = FunctionContext_.sortForType(Value_->getType()).bv_size();
    z3::expr l = zctx.bv_val((int64_t)Lower_, bw);
    z3::expr u = zctx.bv_val((int64_t)Upper_, bw);
    result = ((vmap[Value_] >= l) && (vmap[Value_] <= u));
  }
  return result;
}

int64_t Interval::getLowerBound() const {
  assert(!isBottom());
  return Lower_;
}

int64_t Interval::getUpperBound() const {
  assert(!isBottom());
  return Upper_;
}

void Interval::abstractConsequence(const AbstractValue &av_other) {
  auto other = dynamic_cast<const Interval &>(av_other);
  if (Bottom_)
    return;

  uint64_t delta_l = Lower_ - other.Lower_;
  // positive as Lower_ >= other.Lower_
  uint64_t delta_u = other.Upper_ - Upper_;
  // positive as Upper_ <= other.Upper_

  Lower_ = Lower_ - (delta_l / 2);
  Upper_ = Upper_ + (delta_u / 2);
  assert(checkValid());
}

void Interval::prettyPrint(PrettyPrinter &out) const {
  out << Value_ << pp::rightarrow;

  if (isTop()) {
    out << pp::top;
  } else if (isBottom()) {
    out << pp::bottom;
  } else {
    out << "[" << Lower_ << ", " << Upper_ << "]";
  }
}

void Interval::resetToBottom() {
  Lower_ = 0;
  Upper_ = 0;
  Bottom_ = true;
}

bool Interval::isJoinableWith(const AbstractValue &other) const {
  if (auto *other_val = dynamic_cast<const Interval *>(&other)) {
    if (other_val->Value_ == Value_) {
      return true;
    }
  }
  return false;
}

void Interval::widen() {
  if (isBottom() || isTop()) {
    PrevLower_ = Lower_;
    PrevUpper_ = Upper_;
    return;
  }

  // If lower bound is still monotonically increasing (>=), keep it.
  // Otherwise we lost precision on that side -> send it to -inf.
  if (Lower_ < PrevLower_)
    Lower_ = Min_;

  // If upper bound is still monotonically decreasing (<=), keep it.
  // Otherwise send to +inf.
  if (Upper_ > PrevUpper_)
    Upper_ = Max_;

  PrevLower_ = Lower_;
  PrevUpper_ = Upper_;
}

int64_t ThresholdInterval::getUpperThreshold(int64_t i) {
  for (auto it = Thresholds_.begin(); it < Thresholds_.end(); ++it) {
    if (*it >= i)
      return *it;
  }
  return Max_;
}

int64_t ThresholdInterval::getLowerThreshold(int64_t i) {
  for (auto it = Thresholds_.rbegin(); it < Thresholds_.rend(); ++it) {
    if (*it <= i)
      return *it;
  }
  return Min_;
}

bool ThresholdInterval::joinWith(const AbstractValue &av_other) {
  assert(isJoinableWith(av_other));
  bool changed = Interval::joinWith(av_other);
  if (changed && Lower_ != Upper_) {
    Lower_ = getLowerThreshold(Lower_);
    Upper_ = getUpperThreshold(Upper_);
  }
  return changed;
}

bool ThresholdInterval::meetWith(const AbstractValue &av_other) {
  assert(isJoinableWith(av_other));
  // no adjustment necessary because the result is either a singleton or in
  // thresholds anyway
  return Interval::meetWith(av_other);
}

bool ThresholdInterval::updateWith(const ConcreteState &cstate) {
  bool changed = Interval::updateWith(cstate);
  if (changed && Lower_ != Upper_) {
    Lower_ = getLowerThreshold(Lower_);
    Upper_ = getUpperThreshold(Upper_);
  }
  return changed;
}

void ThresholdInterval::abstractConsequence(const AbstractValue &av_other) {
  auto other = dynamic_cast<const ThresholdInterval &>(av_other);
  if (Bottom_)
    return;

  uint64_t delta_l = Lower_ - other.Lower_;
  // positive as Lower_ >= other.Lower_
  uint64_t delta_u = other.Upper_ - Upper_;
  // positive as Upper_ <= other.Upper_

  int64_t candidate_l = getLowerThreshold(Lower_ - (delta_l / 2));
  int64_t candidate_u = getUpperThreshold(Upper_ + (delta_u / 2));

  if (candidate_l > other.Lower_)
    Lower_ = candidate_l;
  if (candidate_u < other.Upper_)
    Upper_ = candidate_u;

  assert(checkValid());
}

bool ThresholdInterval::isJoinableWith(const AbstractValue &other) const {
  if (const auto *other_val = dynamic_cast<const ThresholdInterval *>(&other)) {
    if (other_val->Value_ != Value_)
      return false;
    if (other_val->Thresholds_ != Thresholds_)
      return false;
    return true;
  }
  return false;
}

unique_ptr<AbstractValue>
ThresholdInterval::ForPowersOfTwo(const FunctionContext &fctx,
                                  llvm::BasicBlock *bb, bool after) {
  Product *result = new Product(fctx);

  for (auto value : fctx.valuesAvailableIn(bb, after)) {
    std::vector<int64_t> th;

    z3::sort sort = fctx.sortForType(value->getType());
    if (!sort.is_bv())
      continue;

    for (int i = 0; i < (int)sort.bv_size() - 1; i++) {
      int64_t v = (1L << i);
      th.push_back(v);
      th.push_back(-v);
    }
    th.push_back(0);
    unique_ptr<AbstractValue> ptr(
        new ThresholdInterval(fctx, value, std::move(th)));
    result->add(std::move(ptr));
  }

  result->finalize();
  return std::unique_ptr<AbstractValue>(result);
}

namespace {
DomainConstructor::Register main("Interval",
                                 "interval domain for single values",
                                 params::ForNonPointers<Interval>);

DomainConstructor::Register
    pow2("Interval/Pow2",
         "restricted interval domain for single values with "
         "either identical lower and upper bounds or bounds "
         "that are powers of two",
         ThresholdInterval::ForPowersOfTwo);
} // namespace
