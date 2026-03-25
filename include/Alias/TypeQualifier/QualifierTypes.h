#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <llvm/Support/raw_ostream.h>

enum class QualifierState : int8_t {
  Unknown = -1,
  Uninitialized = 0,
  Initialized = 1,
};

enum class RequirednessState : int8_t {
  NotRequired = 0,
  Required = 1,
};

class QualifierDomain {
public:
  static constexpr QualifierState unknown() { return QualifierState::Unknown; }
  static constexpr QualifierState uninitialized() {
    return QualifierState::Uninitialized;
  }
  static constexpr QualifierState initialized() {
    return QualifierState::Initialized;
  }

  static QualifierState join(QualifierState lhs, QualifierState rhs) {
    if (lhs == QualifierState::Uninitialized ||
        rhs == QualifierState::Uninitialized) {
      return QualifierState::Uninitialized;
    }
    if (lhs == QualifierState::Unknown || rhs == QualifierState::Unknown) {
      return QualifierState::Unknown;
    }
    return legacyMin(lhs, rhs);
  }

  static QualifierState legacyMin(QualifierState lhs, QualifierState rhs) {
    return ordinal(lhs) <= ordinal(rhs) ? lhs : rhs;
  }

  static bool requiresInput(QualifierState state) {
    return state == QualifierState::Unknown;
  }

  static int ordinal(QualifierState state) {
    return static_cast<int>(state);
  }

  static const char *toString(QualifierState state) {
    switch (state) {
    case QualifierState::Initialized:
      return "Initialized";
    case QualifierState::Uninitialized:
      return "Uninitialized";
    case QualifierState::Unknown:
      return "Unknown";
    }
    return "Invalid";
  }
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                                     QualifierState state) {
  os << QualifierDomain::toString(state);
  return os;
}

class QualifierArray {
public:
  using Storage = std::vector<QualifierState>;
  using iterator = Storage::iterator;
  using const_iterator = Storage::const_iterator;

  QualifierArray() = default;
  QualifierArray(size_t count, QualifierState value = QualifierState::Unknown)
      : values_(count, value) {}

  void resize(size_t count) { values_.resize(count); }
  void clear() { values_.clear(); }
  bool empty() const { return values_.empty(); }
  size_t size() const { return values_.size(); }

  void assign(size_t count, QualifierState value) {
    values_.assign(count, value);
  }

  template <typename It> void assign(It first, It last) {
    values_.assign(first, last);
  }

  QualifierState &at(size_t index) { return values_.at(index); }
  const QualifierState &at(size_t index) const { return values_.at(index); }

  QualifierState &operator[](size_t index) { return values_[index]; }
  const QualifierState &operator[](size_t index) const { return values_[index]; }

  iterator begin() { return values_.begin(); }
  iterator end() { return values_.end(); }
  const_iterator begin() const { return values_.begin(); }
  const_iterator end() const { return values_.end(); }
  const_iterator cbegin() const { return values_.cbegin(); }
  const_iterator cend() const { return values_.cend(); }

  const Storage &raw() const { return values_; }
  Storage &raw() { return values_; }

  bool operator==(const QualifierArray &other) const {
    return values_ == other.values_;
  }

  bool operator!=(const QualifierArray &other) const {
    return !(*this == other);
  }

private:
  Storage values_;
};

class RequirednessArray {
public:
  using Storage = std::vector<RequirednessState>;
  using iterator = Storage::iterator;
  using const_iterator = Storage::const_iterator;

  RequirednessArray() = default;
  RequirednessArray(size_t count,
                    RequirednessState value = RequirednessState::NotRequired)
      : values_(count, value) {}

  void resize(size_t count) { values_.resize(count); }
  void clear() { values_.clear(); }
  bool empty() const { return values_.empty(); }
  size_t size() const { return values_.size(); }

  void assign(size_t count, RequirednessState value) {
    values_.assign(count, value);
  }

  template <typename It> void assign(It first, It last) {
    values_.assign(first, last);
  }

  RequirednessState &at(size_t index) { return values_.at(index); }
  const RequirednessState &at(size_t index) const {
    return values_.at(index);
  }

  RequirednessState &operator[](size_t index) { return values_[index]; }
  const RequirednessState &operator[](size_t index) const {
    return values_[index];
  }

  iterator begin() { return values_.begin(); }
  iterator end() { return values_.end(); }
  const_iterator begin() const { return values_.begin(); }
  const_iterator end() const { return values_.end(); }
  const_iterator cbegin() const { return values_.cbegin(); }
  const_iterator cend() const { return values_.cend(); }

  bool operator==(const RequirednessArray &other) const {
    return values_ == other.values_;
  }

  bool operator!=(const RequirednessArray &other) const {
    return !(*this == other);
  }

private:
  Storage values_;
};

class RequirednessDomain {
public:
  static constexpr RequirednessState required() {
    return RequirednessState::Required;
  }
  static constexpr RequirednessState notRequired() {
    return RequirednessState::NotRequired;
  }

  static RequirednessState join(RequirednessState lhs,
                                RequirednessState rhs) {
    return (lhs == RequirednessState::Required ||
            rhs == RequirednessState::Required)
               ? RequirednessState::Required
               : RequirednessState::NotRequired;
  }

  static bool isRequired(RequirednessState state) {
    return state == RequirednessState::Required;
  }
};
