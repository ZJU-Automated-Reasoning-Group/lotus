//===-- Verification/Sifa/Domain/IntervalDomain.h
//--------------------------===//
//
// Interval domain for Sifa (ported from Ultimate Library-Sifa).
//
// State: map from LLVM Value* to [lo, hi]. Join = interval hull, widen =
// standard. post(Edge): applies instruction-level transfer over the source
// block (sound over-approximation). Implementation in IntervalDomain.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_INTERVALDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_INTERVALDOMAIN_H

#include "llvm/ADT/Optional.h"

#include "Verification/Sifa/BlockTransferPolicy.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"

namespace llvm {
class raw_ostream;
}
namespace lotus {
class AliasAnalysisWrapper;
}
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Value.h"

#include <algorithm>
#include <unordered_map>

namespace lotus {
namespace sifa {

struct SatisfyingInputs;

/// Single interval [lo, hi]; unbounded when optional is nullopt
/// (Ultimate-aligned). Bottom = empty set; top = [-inf, +inf] (no bounds).
struct Interval {
  llvm::Optional<int64_t> lo;
  llvm::Optional<int64_t> hi;
  /// True iff this interval represents bottom (empty set). When false and both
  /// lo/hi nullopt, interval is top.
  bool isBottom_ = false;

  static Interval bottom() {
    Interval i;
    i.isBottom_ = true;
    return i;
  }
  static Interval top() { return {llvm::None, llvm::None, false}; }
  static Interval point(int64_t p) { return {p, p, false}; }
  bool isBottom() const { return isBottom_; }
  bool isTop() const { return !isBottom_ && !lo && !hi; }
  bool isUnbounded() const { return !lo || !hi; }

  /// Ultimate: hasLower/hasUpper — finite bound.
  bool hasLower() const { return lo.hasValue(); }
  bool hasUpper() const { return hi.hasValue(); }
  int64_t getLower() const { return lo.getValueOr(0); }
  int64_t getUpper() const { return hi.getValueOr(0); }
  bool isPoint() const { return lo && hi && *lo == *hi; }
  bool containsZero() const {
    if (isBottom_)
      return false;
    return (!lo || *lo <= 0) && (!hi || *hi >= 0);
  }

  /// Hull (join): [min(lo,rhs.lo), max(hi,rhs.hi)]. Bottom is identity for
  /// join.
  Interval join(const Interval &rhs) const {
    if (isBottom())
      return rhs;
    if (rhs.isBottom())
      return *this;
    Interval r;
    r.lo = (lo && rhs.lo) ? llvm::Optional<int64_t>(std::min(*lo, *rhs.lo))
                          : llvm::None;
    r.hi = (hi && rhs.hi) ? llvm::Optional<int64_t>(std::max(*hi, *rhs.hi))
                          : llvm::None;
    return r;
  }
  /// Standard widening (Ultimate-aligned). Bottom is identity.
  Interval widen(const Interval &rhs) const {
    if (isBottom())
      return rhs;
    if (rhs.isBottom())
      return *this;
    Interval r;
    r.lo = (lo && rhs.lo && *rhs.lo < *lo) ? llvm::None : rhs.lo;
    r.hi = (hi && rhs.hi && *rhs.hi > *hi) ? llvm::None : rhs.hi;
    return r;
  }
  Interval negate() const {
    if (isBottom())
      return bottom();
    Interval r;
    r.lo = hi ? llvm::Optional<int64_t>(-*hi) : llvm::None;
    r.hi = lo ? llvm::Optional<int64_t>(-*lo) : llvm::None;
    return r;
  }
  Interval add(const Interval &rhs) const {
    if (isBottom() || rhs.isBottom())
      return bottom();
    Interval r;
    if (lo && rhs.lo) {
      int64_t sum;
      r.lo = __builtin_add_overflow(*lo, *rhs.lo, &sum)
                 ? llvm::None
                 : llvm::Optional<int64_t>(sum);
    }
    if (hi && rhs.hi) {
      int64_t sum;
      r.hi = __builtin_add_overflow(*hi, *rhs.hi, &sum)
                 ? llvm::None
                 : llvm::Optional<int64_t>(sum);
    }
    return r;
  }
  Interval subtract(const Interval &rhs) const {
    if (isBottom() || rhs.isBottom())
      return bottom();
    Interval r;
    if (lo && rhs.hi) {
      int64_t diff;
      r.lo = __builtin_sub_overflow(*lo, *rhs.hi, &diff)
                 ? llvm::None
                 : llvm::Optional<int64_t>(diff);
    }
    if (hi && rhs.lo) {
      int64_t diff;
      r.hi = __builtin_sub_overflow(*hi, *rhs.lo, &diff)
                 ? llvm::None
                 : llvm::Optional<int64_t>(diff);
    }
    return r;
  }
  /// Ultimate-aligned: [a,b]*[c,d] = [min(ac,ad,bc,bd), max(ac,ad,bc,bd)].
  /// Uses overflow-safe multiplication; returns top on any overflow.
  Interval multiply(const Interval &rhs) const {
    if (isBottom() || rhs.isBottom())
      return bottom();
    if (!lo || !hi || !rhs.lo || !rhs.hi)
      return top();
    int64_t v00, v01, v10, v11;
    if (__builtin_mul_overflow(*lo, *rhs.lo, &v00) ||
        __builtin_mul_overflow(*lo, *rhs.hi, &v01) ||
        __builtin_mul_overflow(*hi, *rhs.lo, &v10) ||
        __builtin_mul_overflow(*hi, *rhs.hi, &v11))
      return top();
    return {std::min({v00, v01, v10, v11}), std::max({v00, v01, v10, v11}),
            false};
  }
  /// Ultimate: divide; containsZero(rhs) => top.
  Interval divide(const Interval &rhs) const {
    if (isBottom() || rhs.isBottom())
      return bottom();
    if (rhs.containsZero())
      return top();
    if (!lo || !hi || !rhs.lo || !rhs.hi)
      return top();
    int64_t v00 = *lo / *rhs.lo, v01 = *lo / *rhs.hi, v10 = *hi / *rhs.lo,
            v11 = *hi / *rhs.hi;
    return {std::min({v00, v01, v10, v11}), std::max({v00, v01, v10, v11}),
            false};
  }
  /// Ultimate: intersect — [max(lo,rhs.lo), min(hi,rhs.hi)]; unbounded sides
  /// preserved.
  Interval intersect(const Interval &rhs) const {
    if (isBottom() || rhs.isBottom())
      return bottom();
    llvm::Optional<int64_t> l =
        (lo && rhs.lo) ? std::max(*lo, *rhs.lo) : (lo ? lo : rhs.lo);
    llvm::Optional<int64_t> h =
        (hi && rhs.hi) ? std::min(*hi, *rhs.hi) : (hi ? hi : rhs.hi);
    if (l && h && *l > *h)
      return bottom();
    Interval r;
    r.lo = l;
    r.hi = h;
    r.isBottom_ = false;
    return r;
  }
  /// Ultimate: complement — one half when half-unbounded; top when fully
  /// unbounded; bottom when empty.
  Interval complement() const {
    if (isBottom())
      return top();
    if (!hasLower() && !hasUpper())
      return bottom();
    if (hasLower() && !hasUpper())
      return {llvm::None, llvm::Optional<int64_t>(*lo - 1), false};
    if (!hasLower() && hasUpper())
      return {llvm::Optional<int64_t>(*hi + 1), llvm::None, false};
    return top();
  }
  SatisfyingInputs satisfyEqual(const Interval &rhs) const;
  SatisfyingInputs satisfyDistinct(const Interval &rhs) const;
  SatisfyingInputs satisfyLessOrEqual(const Interval &rhs) const;
  SatisfyingInputs satisfyGreaterOrEqual(const Interval &rhs) const;

  bool operator==(const Interval &rhs) const {
    return isBottom_ == rhs.isBottom_ && lo == rhs.lo && hi == rhs.hi;
  }
};

/// Ultimate-aligned: SatisfyingInputs for satisfyEqual/satisfyLessOrEqual etc.
struct SatisfyingInputs {
  Interval lhs;
  Interval rhs;
  SatisfyingInputs() = default;
  explicit SatisfyingInputs(const Interval &lhsAndRhs)
      : lhs(lhsAndRhs), rhs(lhsAndRhs) {}
  SatisfyingInputs(const Interval &l, const Interval &r) : lhs(l), rhs(r) {}
  const Interval &getLhs() const { return lhs; }
  const Interval &getRhs() const { return rhs; }
  SatisfyingInputs swap() const { return SatisfyingInputs(rhs, lhs); }
};

inline SatisfyingInputs Interval::satisfyEqual(const Interval &rhs) const {
  return SatisfyingInputs(intersect(rhs));
}
inline SatisfyingInputs Interval::satisfyDistinct(const Interval &rhs) const {
  if (isPoint() && rhs.isPoint() && *lo == *rhs.lo)
    return SatisfyingInputs(bottom());
  return SatisfyingInputs(*this, rhs);
}
inline SatisfyingInputs
Interval::satisfyLessOrEqual(const Interval &rhs) const {
  return SatisfyingInputs(
      {lo, hi && rhs.hi ? std::min(*hi, *rhs.hi) : hi},
      {rhs.lo && lo ? std::max(*rhs.lo, *lo) : rhs.lo, rhs.hi});
}
inline SatisfyingInputs
Interval::satisfyGreaterOrEqual(const Interval &rhs) const {
  return rhs.satisfyLessOrEqual(*this).swap();
}

/// Interval state: map from Value* to Interval (registers) + optional region
/// memory (region -> Interval for Load/Store when alias analysis is used).
class IntervalState {
public:
  IntervalState() = default;
  explicit IntervalState(bool isBot) : isBottom_(isBot) {}

  bool isBottom() const { return isBottom_; }
  void setBottom(bool b) { isBottom_ = b; }

  llvm::Optional<Interval> get(const llvm::Value *v) const {
    auto it = intervals_.find(v);
    if (it == intervals_.end())
      return llvm::None;
    return it->second;
  }
  void set(const llvm::Value *v, Interval i) { intervals_[v] = std::move(i); }
  const std::unordered_map<const llvm::Value *, Interval> &intervals() const {
    return intervals_;
  }

  /// Region memory (allocas, globals). Used when alias analysis is set.
  llvm::Optional<Interval> getMemory(const llvm::Value *region) const {
    auto it = memory_.find(region);
    if (it == memory_.end())
      return llvm::None;
    return it->second;
  }
  void setMemory(const llvm::Value *region, Interval i) {
    memory_[region] = std::move(i);
  }
  const std::unordered_map<const llvm::Value *, Interval> &memory() const {
    return memory_;
  }

  bool operator==(const IntervalState &o) const {
    return isBottom_ == o.isBottom_ && intervals_ == o.intervals_ &&
           memory_ == o.memory_;
  }

  /// Print the state to an output stream.
  void print(llvm::raw_ostream &out) const;

private:
  bool isBottom_ = false;
  std::unordered_map<const llvm::Value *, Interval> intervals_;
  std::unordered_map<const llvm::Value *, Interval> memory_;
};

/// Interval domain implementing AbstractDomain<Transition, IntervalState>.
/// post(Edge) applies real block transfer (instruction semantics) for
/// soundness. When BlockTransferPolicy is set and marks a block as block-wise,
/// post(Edge) uses a fast havoc transfer for that block (less precise, faster).
class IntervalDomain final : public AbstractDomain<Transition, IntervalState> {
public:
  using State = IntervalState;

  IntervalDomain() = default;
  explicit IntervalDomain(const BlockTransferPolicy *policy)
      : blockTransferPolicy_(policy) {}
  /// When \p aliasAnalysis is non-null, Load/Store use region-based memory
  /// (IKOS/CLAM style).
  IntervalDomain(const BlockTransferPolicy *policy,
                 class lotus::AliasAnalysisWrapper *aliasAnalysis)
      : blockTransferPolicy_(policy), aliasAnalysis_(aliasAnalysis) {}

  void setBlockTransferPolicy(const BlockTransferPolicy *policy) {
    blockTransferPolicy_ = policy;
  }
  const BlockTransferPolicy *getBlockTransferPolicy() const {
    return blockTransferPolicy_;
  }
  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) {
    aliasAnalysis_ = aa;
  }
  lotus::AliasAnalysisWrapper *getAliasAnalysis() const {
    return aliasAnalysis_;
  }

  /// Apply transfer for all non-terminator instructions in \p bb (sound
  /// over-approximation). Implemented in IntervalDomain.cpp.
  State applyBlockTransfer(llvm::BasicBlock *bb, const State &in) const;
  State applyBlockTransfer(llvm::BasicBlock *bb, const State &in,
                           const llvm::Instruction *segmentStart,
                           const llvm::Instruction *stopBefore) const;
  /// Block-wise fast path: havoc all values defined in \p bb (sound, less
  /// precise).
  State applyBlockWiseHavoc(llvm::BasicBlock *bb, const State &in) const;
  State applyBlockWiseHavoc(llvm::BasicBlock *bb, const State &in,
                            const llvm::Instruction *segmentStart,
                            const llvm::Instruction *stopBefore) const;

  State top() const override { return State(false); }
  State bottom() const override {
    State s(true);
    return s;
  }
  bool isBottom(const State &s) const override { return s.isBottom(); }

  bool leq(const State &a, const State &b) const override {
    if (a.isBottom())
      return true;
    if (b.isBottom())
      return false;
    for (const auto &kv : a.intervals()) {
      auto ob = b.get(kv.first);
      if (!ob)
        continue;
      const Interval &ia = kv.second;
      const Interval &ib = *ob;
      if (ia.lo.hasValue() && (!ib.lo.hasValue() || *ia.lo < *ib.lo))
        return false;
      if (ia.hi.hasValue() && (!ib.hi.hasValue() || *ia.hi > *ib.hi))
        return false;
    }
    for (const auto &kv : a.memory()) {
      auto ob = b.getMemory(kv.first);
      if (!ob)
        continue;
      const Interval &ia = kv.second;
      const Interval &ib = *ob;
      if (ia.lo.hasValue() && (!ib.lo.hasValue() || *ia.lo < *ib.lo))
        return false;
      if (ia.hi.hasValue() && (!ib.hi.hasValue() || *ia.hi > *ib.hi))
        return false;
    }
    return true;
  }

  State join(const State &a, const State &b) const override {
    if (a.isBottom())
      return b;
    if (b.isBottom())
      return a;
    State r(false);
    for (const auto &kv : a.intervals())
      r.set(kv.first, kv.second);
    for (const auto &kv : b.intervals()) {
      const llvm::Value *v = kv.first;
      const Interval &i = kv.second;
      auto oa = r.get(v);
      if (!oa) {
        r.set(v, i);
        continue;
      }
      Interval hull;
      hull.lo = (oa->lo && i.lo)
                    ? llvm::Optional<int64_t>(std::min(*oa->lo, *i.lo))
                    : llvm::None;
      hull.hi = (oa->hi && i.hi)
                    ? llvm::Optional<int64_t>(std::max(*oa->hi, *i.hi))
                    : llvm::None;
      r.set(v, hull);
    }
    for (const auto &kv : a.memory())
      r.setMemory(kv.first, kv.second);
    for (const auto &kv : b.memory()) {
      const llvm::Value *reg = kv.first;
      const Interval &i = kv.second;
      auto oa = r.getMemory(reg);
      if (!oa) {
        r.setMemory(reg, i);
        continue;
      }
      Interval hull;
      hull.lo = (oa->lo && i.lo)
                    ? llvm::Optional<int64_t>(std::min(*oa->lo, *i.lo))
                    : llvm::None;
      hull.hi = (oa->hi && i.hi)
                    ? llvm::Optional<int64_t>(std::max(*oa->hi, *i.hi))
                    : llvm::None;
      r.setMemory(reg, hull);
    }
    return r;
  }

  State widen(const State &previous, const State &next) const override {
    if (next.isBottom())
      return next;
    if (previous.isBottom())
      return next;
    State r(false);
    for (const auto &kv : next.intervals()) {
      const llvm::Value *v = kv.first;
      const Interval &in = kv.second;
      auto op = previous.get(v);
      if (!op) {
        r.set(v, in);
        continue;
      }
      Interval w;
      if (op->lo.hasValue() && in.lo.hasValue() && *in.lo < *op->lo)
        w.lo = llvm::None;
      else
        w.lo = in.lo;
      if (op->hi.hasValue() && in.hi.hasValue() && *in.hi > *op->hi)
        w.hi = llvm::None;
      else
        w.hi = in.hi;
      r.set(v, w);
    }
    for (const auto &kv : next.memory()) {
      const llvm::Value *reg = kv.first;
      const Interval &in = kv.second;
      auto op = previous.getMemory(reg);
      if (!op) {
        r.setMemory(reg, in);
        continue;
      }
      Interval w;
      if (op->lo.hasValue() && in.lo.hasValue() && *in.lo < *op->lo)
        w.lo = llvm::None;
      else
        w.lo = in.lo;
      if (op->hi.hasValue() && in.hi.hasValue() && *in.hi > *op->hi)
        w.hi = llvm::None;
      else
        w.hi = in.hi;
      r.setMemory(reg, w);
    }
    return r;
  }

  State post(const Transition &t, const State &in) const override;
  State postCall(const Transition &t, const State &callerState) const override;
  State postReturn(const Transition &t, const State &callerState,
                   const State &calleeSummary) const override;

  State postCall(const State &callerState) const override {
    return callerState;
  }
  State postReturn(const State &callerState,
                   const State &calleeSummary) const override {
    return join(callerState, calleeSummary);
  }

private:
  const BlockTransferPolicy *blockTransferPolicy_ = nullptr;
  lotus::AliasAnalysisWrapper *aliasAnalysis_ = nullptr;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_INTERVALDOMAIN_H
