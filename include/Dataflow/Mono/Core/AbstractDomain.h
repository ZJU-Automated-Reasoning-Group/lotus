#pragma once

#include <type_traits>
#include <utility>

namespace mono {

template <typename ValueT> struct LegacyProblemDomain {
  using value_type = ValueT;
  static constexpr bool is_legacy = true;

  value_type bottom() const { return {}; }
  value_type join(const value_type &Lhs, const value_type &) const {
    return Lhs;
  }
  bool equal(const value_type &Lhs, const value_type &Rhs) const {
    return Lhs == Rhs;
  }
  value_type widen(const value_type &, const value_type &NewValue) const {
    return NewValue;
  }
};

template <typename ContainerT> struct UnionDomain {
  using value_type = ContainerT;
  static constexpr bool is_legacy = false;

  value_type bottom() const { return {}; }

  value_type join(const value_type &Lhs, const value_type &Rhs) const {
    value_type Out = Lhs;
    for (const auto &Value : Rhs)
      Out.insert(Value);
    return Out;
  }

  bool equal(const value_type &Lhs, const value_type &Rhs) const {
    return Lhs == Rhs;
  }

  value_type widen(const value_type &, const value_type &NewValue) const {
    return NewValue;
  }
};

template <typename ContainerT> class IntersectionDomain {
public:
  using value_type = ContainerT;
  static constexpr bool is_legacy = false;

  IntersectionDomain() = default;
  explicit IntersectionDomain(value_type Universe)
      : Universe(std::move(Universe)) {}

  void setUniverse(value_type NewUniverse) {
    Universe = std::move(NewUniverse);
  }

  const value_type &universe() const { return Universe; }
  value_type bottom() const { return Universe; }

  value_type join(const value_type &Lhs, const value_type &Rhs) const {
    value_type Out;
    for (const auto &Value : Lhs) {
      if (Rhs.count(Value) != 0)
        Out.insert(Value);
    }
    return Out;
  }

  bool equal(const value_type &Lhs, const value_type &Rhs) const {
    return Lhs == Rhs;
  }

  value_type widen(const value_type &, const value_type &NewValue) const {
    return NewValue;
  }

private:
  value_type Universe;
};

template <typename DomainT, typename = void>
struct IsMonoAbstractDomain : std::false_type {};

template <typename DomainT>
struct IsMonoAbstractDomain<
    DomainT,
    std::void_t<typename DomainT::value_type,
                decltype(std::declval<const DomainT &>().bottom()),
                decltype(std::declval<const DomainT &>().join(
                    std::declval<const typename DomainT::value_type &>(),
                    std::declval<const typename DomainT::value_type &>())),
                decltype(std::declval<const DomainT &>().equal(
                    std::declval<const typename DomainT::value_type &>(),
                    std::declval<const typename DomainT::value_type &>()))>>
    : std::true_type {};

} // namespace mono
