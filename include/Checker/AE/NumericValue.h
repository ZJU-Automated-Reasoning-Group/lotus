#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace lotus {
namespace analysis {

class BoundedDouble {
private:
  double _fVal;

  BoundedDouble() = default;

public:
  BoundedDouble(double fVal) : _fVal(fVal) {}

  BoundedDouble(const BoundedDouble &rhs) : _fVal(rhs._fVal) {}

  BoundedDouble &operator=(const BoundedDouble &rhs) {
    _fVal = rhs._fVal;
    return *this;
  }

  BoundedDouble(BoundedDouble &&rhs) : _fVal(rhs._fVal) {}

  BoundedDouble &operator=(BoundedDouble &&rhs) {
    _fVal = rhs._fVal;
    return *this;
  }

  virtual ~BoundedDouble() {}

  static bool doubleEqual(double a, double b) {
    if (std::isinf(a) && std::isinf(b))
      return a == b;
    return std::fabs(a - b) < std::numeric_limits<double>::epsilon();
  }

  double getFVal() const { return _fVal; }

  bool is_plus_infinity() const {
    return _fVal == std::numeric_limits<double>::infinity();
  }

  bool is_minus_infinity() const {
    return _fVal == -std::numeric_limits<double>::infinity();
  }

  bool is_infinity() const { return is_plus_infinity() || is_minus_infinity(); }

  void set_plus_infinity() { *this = plus_infinity(); }

  void set_minus_infinity() { *this = minus_infinity(); }

  static BoundedDouble plus_infinity() {
    return std::numeric_limits<double>::infinity();
  }

  static BoundedDouble minus_infinity() {
    return -std::numeric_limits<double>::infinity();
  }

  bool is_zero() const { return doubleEqual(_fVal, 0.0); }

  static bool isZero(const BoundedDouble &expr) {
    return doubleEqual(expr.getFVal(), 0.0);
  }

  bool equal(const BoundedDouble &rhs) const {
    return doubleEqual(_fVal, rhs._fVal);
  }

  bool leq(const BoundedDouble &rhs) const {
    if (is_infinity() ^ rhs.is_infinity()) {
      if (is_infinity())
        return is_minus_infinity();
      else
        return rhs.is_plus_infinity();
    }
    if (is_infinity() && rhs.is_infinity()) {
      if (is_minus_infinity())
        return true;
      else
        return rhs.is_plus_infinity();
    }
    return _fVal <= rhs._fVal;
  }

  bool geq(const BoundedDouble &rhs) const {
    if (is_infinity() ^ rhs.is_infinity()) {
      if (is_infinity())
        return is_plus_infinity();
      else
        return rhs.is_minus_infinity();
    }
    if (is_infinity() && rhs.is_infinity()) {
      if (is_plus_infinity())
        return true;
      else
        return rhs.is_minus_infinity();
    }
    return _fVal >= rhs._fVal;
  }

  friend bool operator==(const BoundedDouble &lhs, const BoundedDouble &rhs) {
    return lhs.equal(rhs);
  }

  friend bool operator!=(const BoundedDouble &lhs, const BoundedDouble &rhs) {
    return !lhs.equal(rhs);
  }

  friend bool operator>(const BoundedDouble &lhs, const BoundedDouble &rhs) {
    return !lhs.leq(rhs);
  }

  friend bool operator<(const BoundedDouble &lhs, const BoundedDouble &rhs) {
    return !lhs.geq(rhs);
  }

  friend bool operator<=(const BoundedDouble &lhs, const BoundedDouble &rhs) {
    return lhs.leq(rhs);
  }

  friend bool operator>=(const BoundedDouble &lhs, const BoundedDouble &rhs) {
    return lhs.geq(rhs);
  }

  static double safeAdd(double lhs, double rhs) {
    if ((lhs == std::numeric_limits<double>::infinity() &&
         rhs == -std::numeric_limits<double>::infinity()) ||
        (lhs == -std::numeric_limits<double>::infinity() &&
         rhs == std::numeric_limits<double>::infinity())) {
      assert(false && "invalid add");
    }
    double res = lhs + rhs;
    if (res == std::numeric_limits<double>::infinity() ||
        res == -std::numeric_limits<double>::infinity())
      return res;
    if (lhs > 0 && rhs > 0 && (std::numeric_limits<double>::max() - lhs) < rhs)
      return std::numeric_limits<double>::infinity();
    if (lhs < 0 && rhs < 0 && (-std::numeric_limits<double>::max() - lhs) > rhs)
      return -std::numeric_limits<double>::infinity();
    return res;
  }

  friend BoundedDouble operator+(const BoundedDouble &lhs,
                                 const BoundedDouble &rhs) {
    return safeAdd(lhs._fVal, rhs._fVal);
  }

  friend BoundedDouble operator-(const BoundedDouble &lhs) {
    return -lhs._fVal;
  }

  friend BoundedDouble operator-(const BoundedDouble &lhs,
                                 const BoundedDouble &rhs) {
    return safeAdd(lhs._fVal, -rhs._fVal);
  }

  static double safeMul(double lhs, double rhs) {
    if (doubleEqual(lhs, 0.0) || doubleEqual(rhs, 0.0))
      return 0.0;
    double res = lhs * rhs;
    if (res == std::numeric_limits<double>::infinity() ||
        res == -std::numeric_limits<double>::infinity())
      return res;
    if (lhs > 0 && rhs > 0 && lhs > std::numeric_limits<double>::max() / rhs)
      return std::numeric_limits<double>::infinity();
    if (lhs < 0 && rhs < 0 && lhs < std::numeric_limits<double>::max() / rhs)
      return std::numeric_limits<double>::infinity();
    if (lhs > 0 && rhs < 0 && rhs < std::numeric_limits<double>::lowest() / lhs)
      return -std::numeric_limits<double>::infinity();
    if (lhs < 0 && rhs > 0 && lhs < std::numeric_limits<double>::lowest() / rhs)
      return -std::numeric_limits<double>::infinity();
    return res;
  }

  friend BoundedDouble operator*(const BoundedDouble &lhs,
                                 const BoundedDouble &rhs) {
    return safeMul(lhs._fVal, rhs._fVal);
  }

  static double safeDiv(double lhs, double rhs) {
    if (doubleEqual(rhs, 0.0))
      return (lhs >= 0.0) ? std::numeric_limits<double>::infinity()
                          : -std::numeric_limits<double>::infinity();
    double res = lhs / rhs;
    if (res == std::numeric_limits<double>::infinity() ||
        res == -std::numeric_limits<double>::infinity())
      return res;
    if (rhs > 0 && rhs < std::numeric_limits<double>::min() &&
        lhs > std::numeric_limits<double>::max() * rhs)
      return std::numeric_limits<double>::infinity();
    if (rhs < 0 && rhs > -std::numeric_limits<double>::min() &&
        lhs > std::numeric_limits<double>::max() * rhs)
      return -std::numeric_limits<double>::infinity();
    return res;
  }

  friend BoundedDouble operator/(const BoundedDouble &lhs,
                                 const BoundedDouble &rhs) {
    return safeDiv(lhs._fVal, rhs._fVal);
  }

  friend BoundedDouble operator%(const BoundedDouble &lhs,
                                 const BoundedDouble &rhs) {
    if (rhs.is_zero())
      return BoundedDouble(0);
    if (!lhs.is_infinity() && !rhs.is_infinity())
      return std::fmod(lhs._fVal, rhs._fVal);
    if (!lhs.is_infinity() && rhs.is_infinity())
      return BoundedDouble(0);
    if (lhs.is_infinity() && !rhs.is_infinity())
      return rhs._fVal > 0 ? lhs : -lhs;
    return lhs.equal(rhs) ? plus_infinity() : minus_infinity();
  }

  bool is_int() const { return _fVal == std::round(_fVal); }
  bool is_real() const { return !is_int(); }

  friend BoundedDouble operator^(const BoundedDouble &lhs,
                                 const BoundedDouble &rhs) {
    int lInt = std::round(lhs._fVal), rInt = std::round(rhs._fVal);
    return lInt ^ rInt;
  }

  friend BoundedDouble operator&(const BoundedDouble &lhs,
                                 const BoundedDouble &rhs) {
    int lInt = std::round(lhs._fVal), rInt = std::round(rhs._fVal);
    return lInt & rInt;
  }

  friend BoundedDouble operator|(const BoundedDouble &lhs,
                                 const BoundedDouble &rhs) {
    int lInt = std::round(lhs._fVal), rInt = std::round(rhs._fVal);
    return lInt | rInt;
  }

  friend BoundedDouble operator&&(const BoundedDouble &lhs,
                                  const BoundedDouble &rhs) {
    return lhs._fVal && rhs._fVal;
  }

  friend BoundedDouble operator||(const BoundedDouble &lhs,
                                  const BoundedDouble &rhs) {
    return lhs._fVal || rhs._fVal;
  }

  friend BoundedDouble operator!(const BoundedDouble &lhs) {
    return !lhs._fVal;
  }

  friend BoundedDouble operator>>(const BoundedDouble &lhs,
                                  const BoundedDouble &rhs) {
    assert(rhs.geq(0) && "rhs should be >= 0");
    if (lhs.is_zero())
      return lhs;
    else if (lhs.is_infinity())
      return lhs;
    else if (rhs.is_infinity())
      return lhs.geq(0) ? 0 : -1;
    else
      return (int64_t)lhs.getNumeral() >> (int64_t)rhs.getNumeral();
  }

  friend BoundedDouble operator<<(const BoundedDouble &lhs,
                                  const BoundedDouble &rhs) {
    assert(rhs.geq(0) && "rhs should be >= 0");
    if (lhs.is_zero())
      return lhs;
    else if (lhs.is_infinity())
      return lhs;
    else if (rhs.is_infinity())
      return lhs.geq(0) ? plus_infinity() : minus_infinity();
    else
      return (int64_t)lhs.getNumeral() << (int64_t)rhs.getNumeral();
  }

  friend BoundedDouble ite(const BoundedDouble &cond, const BoundedDouble &lhs,
                           const BoundedDouble &rhs) {
    return cond._fVal != 0.0 ? lhs._fVal : rhs._fVal;
  }

  friend std::ostream &operator<<(std::ostream &out,
                                  const BoundedDouble &expr) {
    out << expr._fVal;
    return out;
  }

  friend bool eq(const BoundedDouble &lhs, const BoundedDouble &rhs) {
    return doubleEqual(lhs._fVal, rhs._fVal);
  }

  friend BoundedDouble min(const BoundedDouble &lhs, const BoundedDouble &rhs) {
    return std::min(lhs._fVal, rhs._fVal);
  }

  friend BoundedDouble max(const BoundedDouble &lhs, const BoundedDouble &rhs) {
    return std::max(lhs._fVal, rhs._fVal);
  }

  static BoundedDouble min(std::vector<BoundedDouble> &_l) {
    BoundedDouble ret(plus_infinity());
    for (const auto &it : _l) {
      if (it.is_minus_infinity())
        return minus_infinity();
      else if (!it.geq(ret)) {
        ret = it;
      }
    }
    return ret;
  }

  static BoundedDouble max(std::vector<BoundedDouble> &_l) {
    BoundedDouble ret(minus_infinity());
    for (const auto &it : _l) {
      if (it.is_plus_infinity())
        return plus_infinity();
      else if (!it.leq(ret)) {
        ret = it;
      }
    }
    return ret;
  }

  friend BoundedDouble abs(const BoundedDouble &lhs) {
    return lhs.leq(0) ? -lhs : lhs;
  }

  bool is_true() const { return _fVal != 0.0; }

  int64_t getNumeral() const {
    if (is_minus_infinity())
      return std::numeric_limits<int64_t>::min();
    else if (is_plus_infinity())
      return std::numeric_limits<int64_t>::max();
    else
      return std::round(_fVal);
  }

  int64_t getIntNumeral() const { return getNumeral(); }

  double getRealNumeral() const { return _fVal; }

  virtual const std::string to_string() const { return std::to_string(_fVal); }
};

} // namespace analysis
} // namespace lotus
