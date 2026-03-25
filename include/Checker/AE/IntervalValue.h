//===- IntervalValue.h ----Interval Value for Abstract Domain-------------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace lotus {
namespace analysis {

/// Bounded integer value for interval bounds
class BoundedInt {
public:
  enum class Kind { Normal, PlusInf, MinusInf };

private:
  Kind kind;
  int64_t value;

public:
  BoundedInt() : kind(Kind::Normal), value(0) {}
  explicit BoundedInt(int64_t v) : kind(Kind::Normal), value(v) {}

  static BoundedInt plus_infinity() {
    BoundedInt bi;
    bi.kind = Kind::PlusInf;
    return bi;
  }

  static BoundedInt minus_infinity() {
    BoundedInt bi;
    bi.kind = Kind::MinusInf;
    return bi;
  }

  bool is_plus_infinity() const { return kind == Kind::PlusInf; }
  bool is_minus_infinity() const { return kind == Kind::MinusInf; }
  bool is_infinity() const {
    return kind == Kind::PlusInf || kind == Kind::MinusInf;
  }
  bool is_zero() const { return kind == Kind::Normal && value == 0; }

  /// Return numeral; for infinity return INT64_MIN/-MAX (aligns with SVF).
  int64_t getNumeral() const {
    if (kind == Kind::MinusInf)
      return std::numeric_limits<int64_t>::min();
    if (kind == Kind::PlusInf)
      return std::numeric_limits<int64_t>::max();
    return value;
  }

  int64_t getIntNumeral() const { return getNumeral(); }

  int64_t getIntNumeralOrZero() const {
    if (kind != Kind::Normal)
      return 0;
    return value;
  }

  double getRealNumeral() const {
    assert(kind == Kind::Normal && "Cannot get real numeral from infinity");
    return static_cast<double>(value);
  }

  bool is_real() const { return false; }

  // Comparison operators
  bool leq(const BoundedInt &other) const {
    if (kind == Kind::MinusInf || other.kind == Kind::PlusInf)
      return true;
    if (kind == Kind::PlusInf || other.kind == Kind::MinusInf)
      return false;
    return value <= other.value;
  }

  bool geq(const BoundedInt &other) const {
    if (kind == Kind::PlusInf || other.kind == Kind::MinusInf)
      return true;
    if (kind == Kind::MinusInf || other.kind == Kind::PlusInf)
      return false;
    return value >= other.value;
  }

  bool equal(const BoundedInt &other) const {
    if (kind != other.kind)
      return false;
    if (kind == Kind::Normal)
      return value == other.value;
    return true; // Both infinity of same kind
  }

  bool eq(const BoundedInt &other) const { return equal(other); }

  // Arithmetic operators
  static BoundedInt safeAdd(const BoundedInt &lhs, const BoundedInt &rhs) {
    // +inf + -inf is undefined in the original SVF model.
    if ((lhs.is_plus_infinity() && rhs.is_minus_infinity()) ||
        (lhs.is_minus_infinity() && rhs.is_plus_infinity())) {
      assert(false && "invalid add");
    }

    if (lhs.is_plus_infinity() || rhs.is_plus_infinity())
      return plus_infinity();
    if (lhs.is_minus_infinity() || rhs.is_minus_infinity())
      return minus_infinity();

    if (lhs.value > 0 && rhs.value > 0 &&
        (std::numeric_limits<int64_t>::max() - lhs.value) < rhs.value)
      return plus_infinity();

    if (lhs.value < 0 && rhs.value < 0 &&
        (std::numeric_limits<int64_t>::lowest() - lhs.value) > rhs.value)
      return minus_infinity();

    return BoundedInt(lhs.value + rhs.value);
  }

  static BoundedInt safeMul(const BoundedInt &lhs, const BoundedInt &rhs) {
    if (lhs.is_zero() || rhs.is_zero())
      return BoundedInt(0);

    if (lhs.is_infinity() || rhs.is_infinity()) {
      // Sign of infinity follows operand sign parity.
      int lhsSign =
          lhs.is_minus_infinity()
              ? -1
              : (lhs.is_plus_infinity() ? 1 : (lhs.value < 0 ? -1 : 1));
      int rhsSign =
          rhs.is_minus_infinity()
              ? -1
              : (rhs.is_plus_infinity() ? 1 : (rhs.value < 0 ? -1 : 1));
      return (lhsSign * rhsSign > 0) ? plus_infinity() : minus_infinity();
    }

    if (lhs.value > 0 && rhs.value > 0 &&
        lhs.value > std::numeric_limits<int64_t>::max() / rhs.value)
      return plus_infinity();

    if (lhs.value < 0 && rhs.value < 0 &&
        lhs.value < std::numeric_limits<int64_t>::max() / rhs.value)
      return plus_infinity();

    if ((lhs.value > 0 && rhs.value < 0 &&
         rhs.value < std::numeric_limits<int64_t>::lowest() / lhs.value) ||
        (lhs.value < 0 && rhs.value > 0 &&
         lhs.value < std::numeric_limits<int64_t>::lowest() / rhs.value))
      return minus_infinity();

    return BoundedInt(lhs.value * rhs.value);
  }

  BoundedInt operator+(const BoundedInt &other) const {
    return safeAdd(*this, other);
  }

  BoundedInt operator-(const BoundedInt &other) const {
    return safeAdd(*this, -other);
  }

  BoundedInt operator*(const BoundedInt &other) const {
    return safeMul(*this, other);
  }

  BoundedInt operator/(const BoundedInt &other) const {
    if (other.is_zero()) {
      assert(false && "divide by zero");
      return plus_infinity();
    }
    if (!is_infinity() && !other.is_infinity())
      return BoundedInt(value / other.value);
    if (!is_infinity() && other.is_infinity())
      return BoundedInt(0);
    if (is_infinity() && !other.is_infinity())
      return (other.value >= 0) ? *this : -*this;
    return equal(other) ? plus_infinity() : minus_infinity();
  }

  BoundedInt operator%(const BoundedInt &other) const {
    if (other.is_zero()) {
      assert(false && "divide by zero");
      return BoundedInt(0);
    }
    if (!is_infinity() && !other.is_infinity())
      return BoundedInt(value % other.value);
    if (!is_infinity() && other.is_infinity())
      return BoundedInt(0);
    if (is_infinity() && !other.is_infinity())
      return (other.value > 0) ? *this : -*this;
    return equal(other) ? plus_infinity() : minus_infinity();
  }

  BoundedInt operator&(const BoundedInt &other) const {
    if (is_infinity() || other.is_infinity())
      return BoundedInt(0);
    return BoundedInt(value & other.value);
  }

  BoundedInt operator|(const BoundedInt &other) const {
    if (is_infinity() || other.is_infinity())
      return plus_infinity();
    return BoundedInt(value | other.value);
  }

  BoundedInt operator^(const BoundedInt &other) const {
    if (is_infinity() || other.is_infinity())
      return plus_infinity();
    return BoundedInt(value ^ other.value);
  }

  BoundedInt operator<<(const BoundedInt &other) const {
    assert(other.geq(BoundedInt(0)) && "rhs should be >= 0");
    if (is_zero())
      return *this;
    if (is_infinity())
      return *this;
    if (other.is_infinity())
      return geq(BoundedInt(0)) ? plus_infinity() : minus_infinity();
    return BoundedInt(value << other.value);
  }

  BoundedInt operator>>(const BoundedInt &other) const {
    assert(other.geq(BoundedInt(0)) && "rhs should be >= 0");
    if (is_zero())
      return *this;
    if (is_infinity())
      return *this;
    if (other.is_infinity())
      return geq(BoundedInt(0)) ? BoundedInt(0) : BoundedInt(-1);
    return BoundedInt(value >> other.value);
  }

  BoundedInt operator-() const {
    if (is_minus_infinity())
      return plus_infinity();
    if (is_plus_infinity())
      return minus_infinity();
    return BoundedInt(-value);
  }

  static BoundedInt min(const std::vector<BoundedInt> &vec) {
    BoundedInt result = plus_infinity();
    for (const auto &v : vec) {
      if (v.leq(result))
        result = v;
    }
    return result;
  }

  static BoundedInt max(const std::vector<BoundedInt> &vec) {
    BoundedInt result = minus_infinity();
    for (const auto &v : vec) {
      if (v.geq(result))
        result = v;
    }
    return result;
  }

  static BoundedInt abs(const BoundedInt &v) {
    if (v.is_infinity())
      return plus_infinity();
    return BoundedInt(std::llabs(v.value));
  }

  std::string to_string() const {
    if (is_minus_infinity())
      return "-inf";
    if (is_plus_infinity())
      return "+inf";
    return std::to_string(value);
  }
};

/// IntervalValue abstract value - implemented as a pair of bounds
class IntervalValue {
public:
  BoundedInt _lb;
  BoundedInt _ub;

public:
  friend IntervalValue operator+(const IntervalValue &lhs,
                                 const IntervalValue &rhs);
  friend IntervalValue operator-(const IntervalValue &lhs,
                                 const IntervalValue &rhs);
  friend IntervalValue operator*(const IntervalValue &lhs,
                                 const IntervalValue &rhs);
  friend IntervalValue operator/(const IntervalValue &lhs,
                                 const IntervalValue &rhs);
  friend IntervalValue operator<<(const IntervalValue &lhs,
                                  const IntervalValue &rhs);
  friend IntervalValue operator>>(const IntervalValue &lhs,
                                  const IntervalValue &rhs);
  friend IntervalValue operator&(const IntervalValue &lhs,
                                 const IntervalValue &rhs);
  friend IntervalValue operator|(const IntervalValue &lhs,
                                 const IntervalValue &rhs);
  friend IntervalValue operator^(const IntervalValue &lhs,
                                 const IntervalValue &rhs);

  friend IntervalValue operator==(const IntervalValue &lhs,
                                  const IntervalValue &rhs);
  friend IntervalValue operator!=(const IntervalValue &lhs,
                                  const IntervalValue &rhs);

  bool isTop() const {
    return _lb.is_minus_infinity() && _ub.is_plus_infinity();
  }

  bool isBottom() const {
    return _lb.is_plus_infinity() && _ub.is_minus_infinity();
  }

  static BoundedInt minus_infinity() { return BoundedInt::minus_infinity(); }

  static BoundedInt plus_infinity() { return BoundedInt::plus_infinity(); }

  static bool is_infinite(const BoundedInt &e) { return e.is_infinity(); }

  static IntervalValue top() {
    return IntervalValue(minus_infinity(), plus_infinity());
  }

  static IntervalValue bottom() {
    return IntervalValue(plus_infinity(), minus_infinity());
  }

  IntervalValue() : _lb(minus_infinity()), _ub(plus_infinity()) {}
  explicit IntervalValue(int64_t n) : _lb(n), _ub(n) {}
  explicit IntervalValue(int32_t n) : IntervalValue(static_cast<int64_t>(n)) {}
  explicit IntervalValue(uint32_t n) : IntervalValue(static_cast<int64_t>(n)) {}
  explicit IntervalValue(BoundedInt n) : _lb(n), _ub(n) {}
  explicit IntervalValue(BoundedInt lb, BoundedInt ub) : _lb(lb), _ub(ub) {
    assert((isBottom() || _lb.leq(_ub)) &&
           "lower bound should be <= upper bound");
  }
  explicit IntervalValue(int64_t lb, int64_t ub) : _lb(lb), _ub(ub) {
    assert((isBottom() || _lb.leq(_ub)) &&
           "lower bound should be <= upper bound");
  }

  const BoundedInt &lb() const {
    assert(!isBottom() && "bottom interval has no lower bound");
    return _lb;
  }

  const BoundedInt &ub() const {
    assert(!isBottom() && "bottom interval has no upper bound");
    return _ub;
  }

  bool is_zero() const { return _lb.is_zero() && _ub.is_zero(); }

  bool is_infinite() const { return _lb.is_infinity() || _ub.is_infinity(); }

  bool is_int() const { return true; }
  bool is_real() const { return false; }

  int64_t getNumeral() const {
    assert(is_numeral() && "not a numeral");
    return _lb.getNumeral();
  }

  int64_t getIntNumeral() const {
    assert(is_numeral() && "not a numeral");
    return _lb.getIntNumeral();
  }

  int64_t getIntNumeralOrZero() const {
    if (!is_numeral())
      return 0;
    return _lb.getIntNumeral();
  }

  double getRealNumeral() const {
    assert(is_numeral() && "not a numeral");
    return _lb.getRealNumeral();
  }

  bool is_numeral() const { return _lb.eq(_ub); }

  void set_to_bottom() {
    _lb = plus_infinity();
    _ub = minus_infinity();
  }

  void set_to_top() {
    _lb = minus_infinity();
    _ub = plus_infinity();
  }

  bool containedWithin(const IntervalValue &other) const {
    if (isBottom())
      return true;
    if (other.isBottom())
      return false;
    return other._lb.leq(_lb) && _ub.leq(other._ub);
  }

  bool contain(const IntervalValue &other) const {
    if (isBottom())
      return true;
    if (other.isBottom())
      return false;
    return other._lb.geq(_lb) && _ub.geq(other._ub);
  }

  bool leq(const IntervalValue &other) const {
    if (isBottom())
      return true;
    if (other.isBottom())
      return false;
    return _ub.leq(other._lb);
  }

  bool geq(const IntervalValue &other) const {
    if (isBottom())
      return true;
    if (other.isBottom())
      return false;
    return _lb.geq(other._ub);
  }

  bool equals(const IntervalValue &other) const {
    if (isBottom())
      return other.isBottom();
    if (other.isBottom())
      return false;
    return _lb.equal(other._lb) && _ub.equal(other._ub);
  }

  void join_with(const IntervalValue &other) {
    if (isBottom()) {
      if (!other.isBottom()) {
        _lb = other._lb;
        _ub = other._ub;
      }
    } else if (!other.isBottom()) {
      _lb = BoundedInt::min({_lb, other._lb});
      _ub = BoundedInt::max({_ub, other._ub});
    }
  }

  void widen_with(const IntervalValue &other) {
    if (isBottom()) {
      _lb = other._lb;
      _ub = other._ub;
    } else if (!other.isBottom()) {
      _lb = !_lb.leq(other._lb) ? minus_infinity() : _lb;
      _ub = !_ub.geq(other._ub) ? plus_infinity() : _ub;
    }
  }

  void narrow_with(const IntervalValue &other) {
    if (isBottom() || other.isBottom()) {
      set_to_bottom();
    } else {
      _lb = is_infinite(_lb) ? other._lb : _lb;
      _ub = is_infinite(_ub) ? other._ub : _ub;
    }
  }

  void meet_with(const IntervalValue &other) {
    if (isBottom() || other.isBottom()) {
      set_to_bottom();
    } else {
      BoundedInt new_lb = BoundedInt::max({_lb, other._lb});
      BoundedInt new_ub = BoundedInt::min({_ub, other._ub});
      if (!new_lb.leq(new_ub)) {
        set_to_bottom();
      } else {
        _lb = new_lb;
        _ub = new_ub;
      }
    }
  }

  bool contains(int n) const {
    return _lb.leq(BoundedInt(static_cast<int64_t>(n))) &&
           _ub.geq(BoundedInt(static_cast<int64_t>(n)));
  }

  bool hasIntersect(const IntervalValue &other) const {
    if (isBottom() || other.isBottom())
      return false;
    // Intersection exists iff lower bound of each interval is <= upper bound of
    // the other interval. This works for finite and infinite bounds.
    return _lb.leq(other._ub) && other._lb.leq(_ub);
  }

  void dump(std::ostream &o) const {
    if (isBottom()) {
      o << "⊥";
    } else {
      o << "[" << _lb.to_string() << ", " << _ub.to_string() << "]";
    }
  }

  std::string toString() const {
    std::stringstream ss;
    dump(ss);
    return ss.str();
  }

private:
  static IntervalValue create(const BoundedInt &lb, const BoundedInt &ub) {
    if (!lb.leq(ub))
      return bottom();
    return IntervalValue(lb, ub);
  }
};

// Arithmetic operators
inline IntervalValue operator+(const IntervalValue &lhs,
                               const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.isTop() || rhs.isTop())
    return IntervalValue::top();
  return IntervalValue(lhs._lb + rhs._lb, lhs._ub + rhs._ub);
}

inline IntervalValue operator-(const IntervalValue &lhs,
                               const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.isTop() || rhs.isTop())
    return IntervalValue::top();
  return IntervalValue(lhs._lb - rhs._ub, lhs._ub - rhs._lb);
}

inline IntervalValue operator*(const IntervalValue &lhs,
                               const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  BoundedInt ll = lhs._lb * rhs._lb;
  BoundedInt lu = lhs._lb * rhs._ub;
  BoundedInt ul = lhs._ub * rhs._lb;
  BoundedInt uu = lhs._ub * rhs._ub;
  return IntervalValue(BoundedInt::min({ll, lu, ul, uu}),
                       BoundedInt::max({ll, lu, ul, uu}));
}

inline IntervalValue operator/(const IntervalValue &lhs,
                               const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (rhs.contains(0)) {
    IntervalValue lb = IntervalValue::create(rhs.lb(), BoundedInt(-1));
    IntervalValue ub = IntervalValue::create(BoundedInt(1), rhs.ub());
    IntervalValue lRes = lhs / lb;
    IntervalValue rRes = lhs / ub;
    lRes.join_with(rRes);
    return lRes;
  }
  if (lhs.contains(0)) {
    IntervalValue lb = IntervalValue::create(lhs.lb(), BoundedInt(-1));
    IntervalValue ub = IntervalValue::create(BoundedInt(1), lhs.ub());
    IntervalValue lRes = lb / rhs;
    IntervalValue rRes = ub / rhs;
    lRes.join_with(rRes);
    lRes.join_with(IntervalValue(0));
    return lRes;
  }

  BoundedInt ll = lhs._lb / rhs._lb;
  BoundedInt lu = lhs._lb / rhs._ub;
  BoundedInt ul = lhs._ub / rhs._lb;
  BoundedInt uu = lhs._ub / rhs._ub;
  return IntervalValue(BoundedInt::min({ll, lu, ul, uu}),
                       BoundedInt::max({ll, lu, ul, uu}));
}

inline IntervalValue operator%(const IntervalValue &lhs,
                               const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.is_infinite() || rhs.is_infinite())
    return IntervalValue::top();
  if (rhs.contains(0))
    return lhs.is_zero() ? IntervalValue(0) : IntervalValue::top();
  BoundedInt n_ub =
      BoundedInt::max({BoundedInt::abs(lhs._lb), BoundedInt::abs(lhs._ub)});
  BoundedInt d_ub =
      BoundedInt::max({BoundedInt::abs(rhs._lb), BoundedInt::abs(rhs._ub)}) -
      BoundedInt(1);
  BoundedInt ub = n_ub.leq(d_ub) ? n_ub : d_ub;
  if (lhs._lb.getNumeral() < 0) {
    if (lhs._ub.getNumeral() > 0)
      return IntervalValue(-ub, ub);
    return IntervalValue(-ub, BoundedInt(0));
  }
  return IntervalValue(BoundedInt(0), ub);
}

inline IntervalValue operator&(const IntervalValue &lhs,
                               const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.is_numeral() && rhs.is_numeral())
    return IntervalValue(lhs._lb & rhs._lb);
  // Cannot call getNumeral() on infinity - return top for unbounded intervals
  if (lhs.is_infinite() || rhs.is_infinite())
    return IntervalValue::top();
  if (lhs._lb.getNumeral() >= 0 && rhs._lb.getNumeral() >= 0)
    return IntervalValue(0, BoundedInt::min({lhs._ub, rhs._ub}).getNumeral());
  if (lhs._lb.getNumeral() >= 0)
    return IntervalValue(0, lhs._ub.getNumeral());
  if (rhs._lb.getNumeral() >= 0)
    return IntervalValue(0, rhs._ub.getNumeral());
  return IntervalValue::top();
}

inline IntervalValue operator|(const IntervalValue &lhs,
                               const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.is_numeral() && rhs.is_numeral())
    return IntervalValue(lhs._lb | rhs._lb);
  auto next_power_of_2 = [](int64_t num) {
    int i = 1;
    while ((num >> i) != 0)
      ++i;
    return int64_t(1) << i;
  };
  if (!lhs.is_infinite() && !rhs.is_infinite() && lhs._lb.getNumeral() >= 0 &&
      rhs._lb.getNumeral() >= 0) {
    int64_t m = std::max(lhs._ub.getNumeral(), rhs._ub.getNumeral());
    int64_t ub = next_power_of_2(m) - 1;
    return IntervalValue(int64_t(0), ub);
  }
  return IntervalValue::top();
}

inline IntervalValue operator^(const IntervalValue &lhs,
                               const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.is_numeral() && rhs.is_numeral())
    return IntervalValue(lhs._lb ^ rhs._lb);
  return IntervalValue::top();
}

inline IntervalValue operator<<(const IntervalValue &lhs,
                                const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.isTop() && rhs.isTop())
    return IntervalValue::top();

  IntervalValue shift = rhs;
  shift.meet_with(IntervalValue(BoundedInt(0), IntervalValue::plus_infinity()));
  if (shift.isBottom())
    return IntervalValue::bottom();

  BoundedInt lb(0);
  if (shift.lb().getNumeral() >= 32 || shift.lb().is_infinity()) {
    lb = IntervalValue::minus_infinity();
  } else {
    lb = BoundedInt(int64_t(1) << static_cast<int>(shift.lb().getNumeral()));
  }

  BoundedInt ub(0);
  if (shift.ub().is_infinity()) {
    ub = IntervalValue::plus_infinity();
  } else {
    ub = BoundedInt(int64_t(1) << static_cast<int>(shift.ub().getNumeral()));
  }

  IntervalValue coeff(lb, ub);
  return lhs * coeff;
}

inline IntervalValue operator>>(const IntervalValue &lhs,
                                const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.isTop() && rhs.isTop())
    return IntervalValue::top();

  IntervalValue shift = rhs;
  shift.meet_with(IntervalValue(BoundedInt(0), IntervalValue::plus_infinity()));
  if (shift.isBottom())
    return IntervalValue::bottom();

  if (lhs.contains(0)) {
    IntervalValue l = IntervalValue::create(lhs.lb(), BoundedInt(-1));
    IntervalValue u = IntervalValue::create(BoundedInt(1), lhs.ub());
    IntervalValue tmp = l >> rhs;
    tmp.join_with(u >> rhs);
    tmp.join_with(IntervalValue(0));
    return tmp;
  }

  BoundedInt ll = lhs._lb >> shift._lb;
  BoundedInt lu = lhs._lb >> shift._ub;
  BoundedInt ul = lhs._ub >> shift._lb;
  BoundedInt uu = lhs._ub >> shift._ub;
  return IntervalValue(BoundedInt::min({ll, lu, ul, uu}),
                       BoundedInt::max({ll, lu, ul, uu}));
}

inline std::ostream &operator<<(std::ostream &o, const IntervalValue &iv) {
  iv.dump(o);
  return o;
}

inline IntervalValue operator==(const IntervalValue &lhs,
                                const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.isTop() || rhs.isTop())
    return IntervalValue::top();
  if (lhs.is_numeral() && rhs.is_numeral()) {
    return lhs.equals(rhs) ? IntervalValue(static_cast<int64_t>(1))
                           : IntervalValue(static_cast<int64_t>(0));
  }
  IntervalValue meet = lhs;
  meet.meet_with(rhs);
  if (meet.isBottom())
    return IntervalValue(static_cast<int64_t>(0));
  return IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(1));
}

inline IntervalValue operator!=(const IntervalValue &lhs,
                                const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.isTop() || rhs.isTop())
    return IntervalValue::top();
  if (lhs.is_numeral() && rhs.is_numeral()) {
    return lhs.equals(rhs) ? IntervalValue(static_cast<int64_t>(0))
                           : IntervalValue(static_cast<int64_t>(1));
  }
  IntervalValue meet = lhs;
  meet.meet_with(rhs);
  if (!meet.isBottom())
    return IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(1));
  return IntervalValue(static_cast<int64_t>(1));
}

// Comparison operators for IntervalValue
inline IntervalValue operator>(const IntervalValue &lhs,
                               const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.isTop() || rhs.isTop())
    return IntervalValue::top();
  if (lhs.is_numeral() && rhs.is_numeral())
    return lhs._lb.leq(rhs._lb) ? IntervalValue(static_cast<int64_t>(0))
                                : IntervalValue(static_cast<int64_t>(1));
  if (!lhs._lb.leq(rhs._ub))
    return IntervalValue(static_cast<int64_t>(1));
  if (!lhs._ub.geq(rhs._lb))
    return IntervalValue(static_cast<int64_t>(0));
  return IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(1));
}

inline IntervalValue operator<(const IntervalValue &lhs,
                               const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.isTop() || rhs.isTop())
    return IntervalValue::top();
  if (lhs.is_numeral() && rhs.is_numeral())
    return lhs._lb.geq(rhs._lb) ? IntervalValue(static_cast<int64_t>(0))
                                : IntervalValue(static_cast<int64_t>(1));
  if (!lhs._ub.geq(rhs._lb))
    return IntervalValue(static_cast<int64_t>(1));
  if (!lhs._lb.leq(rhs._ub))
    return IntervalValue(static_cast<int64_t>(0));
  return IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(1));
}

inline IntervalValue operator>=(const IntervalValue &lhs,
                                const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.isTop() || rhs.isTop())
    return IntervalValue::top();
  if (lhs.is_numeral() && rhs.is_numeral())
    return lhs._lb.geq(rhs._lb) ? IntervalValue(static_cast<int64_t>(1))
                                : IntervalValue(static_cast<int64_t>(0));
  if (lhs._lb.geq(rhs._ub))
    return IntervalValue(static_cast<int64_t>(1));
  if (!lhs._ub.geq(rhs._lb))
    return IntervalValue(static_cast<int64_t>(0));
  return IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(1));
}

inline IntervalValue operator<=(const IntervalValue &lhs,
                                const IntervalValue &rhs) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (lhs.isTop() || rhs.isTop())
    return IntervalValue::top();
  if (lhs.is_numeral() && rhs.is_numeral())
    return lhs._lb.leq(rhs._lb) ? IntervalValue(static_cast<int64_t>(1))
                                : IntervalValue(static_cast<int64_t>(0));
  if (lhs._ub.leq(rhs._lb))
    return IntervalValue(static_cast<int64_t>(1));
  if (!lhs._lb.leq(rhs._ub))
    return IntervalValue(static_cast<int64_t>(0));
  return IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(1));
}

} // namespace analysis
} // namespace lotus
