#ifndef CHECKER_PULSE_PULSEABSTRACTVVALUE_H
#define CHECKER_PULSE_PULSEABSTRACTVVALUE_H

#include <functional>

namespace llvm {
class Value;
} // namespace llvm

namespace pulse {

class AbstractValue {
private:
  const llvm::Value *value_;
  unsigned id_;

public:
  AbstractValue() : value_(nullptr), id_(0) {}
  AbstractValue(const llvm::Value *v, unsigned id) : value_(v), id_(id) {}

  const llvm::Value *getValue() const { return value_; }
  unsigned getId() const { return id_; }

  bool operator==(const AbstractValue &other) const { return id_ == other.id_; }
  bool operator<(const AbstractValue &other) const { return id_ < other.id_; }
};

} // namespace pulse

namespace std {

template <> struct hash<pulse::AbstractValue> {
  size_t operator()(const pulse::AbstractValue &v) const noexcept {
    return std::hash<unsigned>{}(v.getId());
  }
};

} // namespace std

#endif // CHECKER_PULSE_PULSEABSTRACTVVALUE_H
