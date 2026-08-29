#pragma once

#include <cstdint>
#include <unordered_map>

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

class SignValue final {
public:
  enum Bits : std::uint8_t { None = 0, Negative = 1, Zero = 2, Positive = 4 };

  SignValue() = default;
  explicit SignValue(std::uint8_t Mask) : Mask(Mask) {}

  static SignValue bottom() { return SignValue(None); }
  static SignValue negative() { return SignValue(Negative); }
  static SignValue zero() { return SignValue(Zero); }
  static SignValue positive() { return SignValue(Positive); }
  static SignValue nonNegative() { return SignValue(Zero | Positive); }
  static SignValue nonZero() { return SignValue(Negative | Positive); }
  static SignValue top() { return SignValue(Negative | Zero | Positive); }

  bool isBottom() const { return Mask == None; }
  bool mayBeNegative() const { return (Mask & Negative) != 0; }
  bool mayBeZero() const { return (Mask & Zero) != 0; }
  bool mayBePositive() const { return (Mask & Positive) != 0; }
  std::uint8_t bits() const { return Mask; }

  void mergeIn(SignValue Other) { Mask |= Other.Mask; }

  friend bool operator==(SignValue Lhs, SignValue Rhs) {
    return Lhs.Mask == Rhs.Mask;
  }
  friend bool operator!=(SignValue Lhs, SignValue Rhs) { return !(Lhs == Rhs); }

private:
  std::uint8_t Mask = None;
};

using SignMap = std::unordered_map<const llvm::Value *, SignValue>;

struct SignDomain {
  using value_type = SignMap;

  value_type bottom() const { return {}; }

  value_type join(const value_type &Lhs, const value_type &Rhs) const {
    value_type Out = Lhs;
    for (const auto &Entry : Rhs)
      Out[Entry.first].mergeIn(Entry.second);
    return Out;
  }

  bool equal(const value_type &Lhs, const value_type &Rhs) const {
    for (const auto &Entry : Lhs) {
      auto It = Rhs.find(Entry.first);
      if (It == Rhs.end()) {
        if (!Entry.second.isBottom())
          return false;
        continue;
      }
      if (Entry.second != It->second)
        return false;
    }
    for (const auto &Entry : Rhs) {
      auto It = Lhs.find(Entry.first);
      if (It == Lhs.end() && !Entry.second.isBottom())
        return false;
    }
    return true;
  }
};

} // namespace elimination
