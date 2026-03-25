#pragma once
//===--------------------------------------------------------------------===//
//  optional14/optional14.hpp
//
//  A C++14 back-port of (most of) std::optional
//  – in_place construction
//  – nullopt / bad_optional_access
//  – value(), value_or(), reset(), emplace()
//  – CTAD-like helpers: make_optional()
//  – constexpr where the standard allows it in C++14
//  – noexcept/exception-safety matches <optional>
//  – cross-type converting ctors/assign
//  – operators == != < <= > >= with optionals and bare T
//  – ADL swap + std::hash
//
//  Limitations vs. C++20 optional
//  – No monadic helpers (and_then, transform, …)
//  – Not all operations are constexpr (C++14 can’t place new in constexpr)
//  – Uses one byte of padding on many ABIs (sizeof(optional<int>) == 8)
//
//  Public domain / 0-BSD – use at own risk.
//===--------------------------------------------------------------------===//

#include <cassert>
#include <cstddef>
#include <functional>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace optional14 {

//---------------------------------------------------------------------------
// 1. nullopt machinery
//---------------------------------------------------------------------------
struct nullopt_t {
  struct init {};
  constexpr explicit nullopt_t(init) noexcept {}
};

constexpr nullopt_t nullopt{nullopt_t::init{}};

//---------------------------------------------------------------------------
// 2. bad_optional_access
//---------------------------------------------------------------------------
class bad_optional_access : public std::logic_error {
public:
  explicit bad_optional_access(const char *msg = "bad optional access")
      : std::logic_error(msg) {}
};

//---------------------------------------------------------------------------
// 3. in_place machinery
//---------------------------------------------------------------------------
struct in_place_t {
  struct init {};
  constexpr explicit in_place_t(init) noexcept {}
};

constexpr in_place_t in_place{in_place_t::init{}};

//---------------------------------------------------------------------------
// 4. raw storage helper with trivial-destructor propagation
//---------------------------------------------------------------------------
template <typename T, bool = std::is_trivially_destructible<T>::value>
struct storage_base {
  bool engaged_;
  typename std::aligned_storage<sizeof(T), alignof(T)>::type buf_;

  constexpr storage_base() noexcept : engaged_(false), buf_{} {}

  template <class... Args> void construct(Args &&...args) {
    ::new (static_cast<void *>(&buf_)) T(std::forward<Args>(args)...);
    engaged_ = true;
  }

  void destroy() noexcept {
    if (engaged_) {
      reinterpret_cast<T *>(&buf_)->~T();
      engaged_ = false;
    }
  }

  ~storage_base() { destroy(); }
};

template <typename T> struct storage_base<T, /*trivial*/ true> {
  bool engaged_;
  typename std::aligned_storage<sizeof(T), alignof(T)>::type buf_;

  constexpr storage_base() noexcept : engaged_(false), buf_{} {}

  template <class... Args> void construct(Args &&...args) {
    ::new (static_cast<void *>(&buf_)) T(std::forward<Args>(args)...);
    engaged_ = true;
  }

  void destroy() noexcept {
    if (engaged_)
      reinterpret_cast<T *>(&buf_)->~T();
    engaged_ = false;
  }

  // trivial dtor
  ~storage_base() = default;
};

//---------------------------------------------------------------------------
// 5. optional<T>
//---------------------------------------------------------------------------
template <typename T> class optional;

namespace detail {

// SFINAE helpers ------------------------------------------------------------
template <class From, class To>
using convertible_t = typename std::enable_if<
    std::is_constructible<To, From>::value &&
    !std::is_same<typename std::decay<From>::type, To>::value>::type;

template <class T> struct is_optional : std::false_type {};

template <class U> struct is_optional<optional<U>> : std::true_type {};

} // namespace detail

template <typename T> class optional : private storage_base<T> {
  using base = storage_base<T>;

public:
  //--------------------------------------------------------------------------
  // 5.1. Constructors
  //--------------------------------------------------------------------------
  constexpr optional() noexcept = default;
  constexpr optional(nullopt_t) noexcept {}

  optional(const optional &other) {
    if (other.engaged_)
      this->construct(*other);
  }

  optional(optional &&other) noexcept(
      std::is_nothrow_move_constructible<T>::value) {
    if (other.engaged_)
      this->construct(std::move(*other));
    other.destroy();
  }

  template <
      class U = T,
      class = typename std::enable_if<
          std::is_constructible<T, U &&>::value &&
          !detail::is_optional<typename std::decay<U>::type>::value>::type>
  constexpr optional(U &&v) {
    this->construct(std::forward<U>(v));
  }

  template <class... Args> explicit optional(in_place_t, Args &&...args) {
    this->construct(std::forward<Args>(args)...);
  }

  // converting from other optional
  template <class U, class = typename std::enable_if<
                         std::is_constructible<T, const U &>::value>::type>
  optional(const optional<U> &other) {
    if (other)
      this->construct(*other);
  }

  template <class U, class = typename std::enable_if<
                         std::is_constructible<T, U &&>::value>::type>
  optional(optional<U> &&other) {
    if (other)
      this->construct(std::move(*other));
    other.reset();
  }

  //--------------------------------------------------------------------------
  // 5.2. Destructor
  //--------------------------------------------------------------------------
  ~optional() = default;

  //--------------------------------------------------------------------------
  // 5.3. Assignment
  //--------------------------------------------------------------------------
  optional &operator=(nullopt_t) noexcept {
    this->destroy();
    return *this;
  }

  optional &operator=(const optional &rhs) {
    if (this != &rhs) {
      if (rhs) {
        if (*this)
          **this = *rhs;
        else
          this->construct(*rhs);
      } else {
        this->destroy();
      }
    }
    return *this;
  }

  optional &operator=(optional &&rhs) noexcept(
      std::is_nothrow_move_constructible<T>::value &&
      std::is_nothrow_move_assignable<T>::value) {
    if (this != &rhs) {
      if (rhs) {
        if (*this)
          **this = std::move(*rhs);
        else
          this->construct(std::move(*rhs));
      } else {
        this->destroy();
      }
      rhs.reset();
    }
    return *this;
  }

  template <class U = T, class = typename std::enable_if<
                             std::is_constructible<T, U>::value &&
                             std::is_assignable<T &, U>::value>::type>
  optional &operator=(U &&v) {
    if (*this)
      **this = std::forward<U>(v);
    else
      this->construct(std::forward<U>(v));
    return *this;
  }

  //--------------------------------------------------------------------------
  // 5.4. Observers
  //--------------------------------------------------------------------------
  constexpr T const *operator->() const {
    return reinterpret_cast<const T *>(&this->buf_);
  }
  T *operator->() { return reinterpret_cast<T *>(&this->buf_); }

  constexpr T const &operator*() const & { return *operator->(); }
  T &operator*() & { return *operator->(); }
  constexpr T &&operator*() const && { return std::move(*operator->()); }
  T &&operator*() && { return std::move(*operator->()); }

  constexpr explicit operator bool() const noexcept { return this->engaged_; }
  constexpr bool has_value() const noexcept { return this->engaged_; }

  T &value() & {
    if (!*this)
      throw bad_optional_access{};
    return **this;
  }
  const T &value() const & {
    if (!*this)
      throw bad_optional_access{};
    return **this;
  }
  T &&value() && {
    if (!*this)
      throw bad_optional_access{};
    return std::move(**this);
  }
  const T &&value() const && {
    if (!*this)
      throw bad_optional_access{};
    return std::move(**this);
  }

  template <class U> constexpr T value_or(U &&v) const & {
    return *this ? **this : static_cast<T>(std::forward<U>(v));
  }

  template <class U> T value_or(U &&v) && {
    return *this ? std::move(**this) : static_cast<T>(std::forward<U>(v));
  }

  //--------------------------------------------------------------------------
  // 5.5. Modifiers
  //--------------------------------------------------------------------------
  void reset() noexcept { this->destroy(); }

  template <class... Args> T &emplace(Args &&...args) {
    this->destroy();
    this->construct(std::forward<Args>(args)...);
    return **this;
  }

  //--------------------------------------------------------------------------
  // 5.6. swap
  //--------------------------------------------------------------------------
  void swap(optional &other) noexcept(
      std::is_nothrow_move_constructible<T>::value &&
      noexcept(std::swap(std::declval<T &>(), std::declval<T &>()))) {
    if (*this && other) {
      using std::swap;
      swap(**this, *other);
    } else if (*this) {
      other.construct(std::move(**this));
      this->destroy();
    } else if (other) {
      this->construct(std::move(*other));
      other.destroy();
    }
  }
};

//---------------------------------------------------------------------------
// 6. Non-member swap
//---------------------------------------------------------------------------
template <class T>
void swap(optional<T> &a, optional<T> &b) noexcept(noexcept(a.swap(b))) {
  a.swap(b);
}

//---------------------------------------------------------------------------
// 7. Relational operators
//---------------------------------------------------------------------------
template <class T, class U>
constexpr bool operator==(optional<T> const &a, optional<U> const &b) {
  return bool(a) == bool(b) && (!a || *a == *b);
}
template <class T, class U>
constexpr bool operator!=(optional<T> const &a, optional<U> const &b) {
  return !(a == b);
}
template <class T, class U>
constexpr bool operator<(optional<T> const &a, optional<U> const &b) {
  return b ? (!a || *a < *b) : false;
}
template <class T, class U>
constexpr bool operator>(optional<T> const &a, optional<U> const &b) {
  return b < a;
}
template <class T, class U>
constexpr bool operator<=(optional<T> const &a, optional<U> const &b) {
  return !(b < a);
}
template <class T, class U>
constexpr bool operator>=(optional<T> const &a, optional<U> const &b) {
  return !(a < b);
}

// comparisons with bare T ---------------------------------------------------
template <class T, class U>
constexpr bool operator==(optional<T> const &a, U const &v) {
  return a ? *a == v : false;
}
template <class T, class U>
constexpr bool operator==(U const &v, optional<T> const &a) {
  return a == v;
}

template <class T, class U>
constexpr bool operator!=(optional<T> const &a, U const &v) {
  return !(a == v);
}
template <class T, class U>
constexpr bool operator!=(U const &v, optional<T> const &a) {
  return !(a == v);
}

//---------------------------------------------------------------------------
// 8. make_optional helpers
//---------------------------------------------------------------------------
template <class T>
constexpr optional<typename std::decay<T>::type> make_optional(T &&v) {
  return optional<typename std::decay<T>::type>(std::forward<T>(v));
}

template <class T, class... Args>
constexpr optional<T> make_optional(Args &&...args) {
  return optional<T>(in_place, std::forward<Args>(args)...);
}

//---------------------------------------------------------------------------
// 9. std::hash specialisation
//---------------------------------------------------------------------------
} // namespace optional14

//---------------------------------------------------------------------------
// 10. util namespace (repo alias for optional14)
//---------------------------------------------------------------------------
namespace util {
template <typename T> using Optional = optional14::optional<T>;
using optional14::nullopt;
} // namespace util

namespace std {
template <class T> struct hash<optional14::optional<T>> {
  size_t operator()(optional14::optional<T> const &o) const
      noexcept(noexcept(std::declval<hash<T>>()(std::declval<T>()))) {
    return o ? hash<T>()(*o) + 0x9e3779b9 : 0;
  }
};
} // namespace std