#pragma once

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/ResultStore.h"
#include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <memory>

#include <llvm/IR/Value.h>

namespace symabs_ai {
class FunctionContext;

namespace domains {

class Interval : public AbstractValue {
protected:
  int64_t Max_ = 0;
  int64_t Min_ = 0;

  /*
   * Previous bounds remembered for widening.  They store the interval that
   * was produced in the previous iteration so we can tell which side keeps
   * moving.  Initialised to the extreme values so that the very first
   * widening step behaves as a no-op.
   */
  int64_t PrevLower_ = std::numeric_limits<int64_t>::min();
  int64_t PrevUpper_ = std::numeric_limits<int64_t>::max();

  const FunctionContext &FunctionContext_;
  RepresentedValue Value_;
  int64_t Lower_ = 0;
  int64_t Upper_ = 0;
  // for a BOTTOM Value, Lower == upper == 0 should hold
  bool Bottom_ = true;

  bool checkValid() {
    if (Lower_ < Min_ || Upper_ > Max_) {
      return false;
    }

    if (Bottom_) {
      return !(Lower_ || Upper_);
    } else {
      return Lower_ <= Upper_;
    }
  }

public:
  Interval(const FunctionContext &fctx, RepresentedValue value)
      : FunctionContext_(fctx), Value_(value) {
    int64_t bw = fctx.sortForType(value->getType()).bv_size();
    Max_ = (1L << (bw - 1L)) - 1L;
    Min_ = (-Max_) - 1L;

    PrevLower_ = Min_;
    PrevUpper_ = Max_;

    assert(checkValid());
  }

  virtual ~Interval() {}

  virtual bool joinWith(const AbstractValue &av_other) override;

  virtual bool meetWith(const AbstractValue &av_other) override;

  virtual bool updateWith(const ConcreteState &cstate) override;

  virtual z3::expr toFormula(const ValueMapping &vmap,
                             z3::context &zctx) const override;

  virtual void havoc() override;

  virtual AbstractValue *clone() const override { return new Interval(*this); }

  virtual bool isTop() const override;
  virtual bool isBottom() const override { return Bottom_; }
  bool isConst() const { return !isTop() && !isBottom(); }

  /**
   * Returns the lower bound stored in this AbstractValue (if there is a
   * lower bound (i.e. it is not bottom), otherwise assertions will fail.
   */
  int64_t getLowerBound() const;

  /**
   * Returns the upper bound stored in this AbstractValue (if there is a
   * upper bound (i.e. it is not bottom), otherwise assertions will fail.
   */
  int64_t getUpperBound() const;

  /**
   * Returns the LLVM Value representing the variable whose constness is
   * described by this AbstractValue.
   */
  llvm::Value *getVariable() const { return Value_; }

  virtual void abstractConsequence(const AbstractValue &av_other) override;

  virtual void prettyPrint(PrettyPrinter &out) const override;

  virtual void resetToBottom() override;
  virtual bool isJoinableWith(const AbstractValue &other) const override;

  virtual void widen() override;

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    ResultStore::ValueWrapper wrap_val(Value_);
    archive(wrap_val, Lower_, Upper_, Bottom_);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void load_and_construct(Archive &archive,
                                 cereal::construct<Interval> &construct) {
    ResultStore::ValueWrapper wrap_val;
    archive(wrap_val);
    auto &fctx = cereal::get_user_data<FunctionContext>(archive);
    auto *rval = fctx.findRepresentedValue(wrap_val);
    construct(fctx, *rval);
    archive(construct->Lower_, construct->Upper_, construct->Bottom_);
    assert(construct->checkValid());
  }
#endif
};

class ThresholdInterval : public Interval {
private:
  // sorted by ascending values
  std::vector<int64_t> Thresholds_;

  int64_t getUpperThreshold(int64_t i);
  int64_t getLowerThreshold(int64_t i);

public:
  ThresholdInterval(const FunctionContext &fctx, RepresentedValue value,
                    std::vector<int64_t> &&thresholds)
      : Interval(fctx, value), Thresholds_(std::move(thresholds)) {
    std::sort(Thresholds_.begin(), Thresholds_.end());
  }

  virtual ~ThresholdInterval() {}

  virtual bool joinWith(const AbstractValue &av_other) override;
  virtual bool meetWith(const AbstractValue &av_other) override;
  virtual bool updateWith(const ConcreteState &cstate) override;
  virtual void abstractConsequence(const AbstractValue &av_other) override;

  virtual bool isJoinableWith(const AbstractValue &other) const override;

  virtual AbstractValue *clone() const override {
    return new ThresholdInterval(*this);
  }

  static std::unique_ptr<AbstractValue>
  ForPowersOfTwo(const FunctionContext &fctx, llvm::BasicBlock *bb, bool after);
};

} // namespace domains
} // namespace symabs_ai

#ifdef ENABLE_DYNAMIC
CEREAL_REGISTER_TYPE(symabs_ai::domains::Interval);
#endif
