//===- AbstractValue.h ----Abstract Value--------------------------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Checker/AE/AddressValue.h"
#include "Checker/AE/IntervalValue.h"

namespace lotus {
namespace analysis {

/// AbstractValue - combines interval and address values
class AbstractValue {
public:
  IntervalValue interval;
  AddressValue addrs;

  AbstractValue() {
    interval = IntervalValue::bottom();
    addrs = AddressValue();
  }

  AbstractValue(const AbstractValue &other) {
    interval = other.interval;
    addrs = other.addrs;
  }

  bool isInterval() const { return !interval.isBottom(); }
  bool isAddr() const { return !addrs.isBottom(); }

  AbstractValue(AbstractValue &&other) {
    interval = std::move(other.interval);
    addrs = std::move(other.addrs);
  }

  AbstractValue &operator=(const AbstractValue &other) {
    interval = other.interval;
    addrs = other.addrs;
    return *this;
  }

  AbstractValue &operator=(AbstractValue &&other) {
    interval = std::move(other.interval);
    addrs = std::move(other.addrs);
    return *this;
  }

  AbstractValue &operator=(const IntervalValue &ival) {
    interval = ival;
    addrs = AddressValue();
    return *this;
  }

  AbstractValue &operator=(const AddressValue &addr) {
    addrs = addr;
    interval = IntervalValue::bottom();
    return *this;
  }

  AbstractValue(const IntervalValue &ival)
      : interval(ival), addrs(AddressValue()) {}

  AbstractValue(const AddressValue &addr)
      : interval(IntervalValue::bottom()), addrs(addr) {}

  IntervalValue &getInterval() { return interval; }
  const IntervalValue getInterval() const { return interval; }
  AddressValue &getAddrs() { return addrs; }
  const AddressValue getAddrs() const { return addrs; }

  ~AbstractValue() {};

  bool equals(const AbstractValue &rhs) const {
    return interval.equals(rhs.interval) && addrs.equals(rhs.addrs);
  }

  void join_with(const AbstractValue &other) {
    interval.join_with(other.interval);
    addrs.join_with(other.addrs);
  }

  void meet_with(const AbstractValue &other) {
    interval.meet_with(other.interval);
    addrs.meet_with(other.addrs);
  }

  void widen_with(const AbstractValue &other) {
    interval.widen_with(other.interval);
  }

  void narrow_with(const AbstractValue &other) {
    interval.narrow_with(other.interval);
  }

  std::string toString() const {
    return "<" + interval.toString() + ", " + addrs.toString() + ">";
  }
};

} // namespace analysis
} // namespace lotus
