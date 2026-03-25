#pragma once

#include <type_traits>

// Simple SFINAE struct to test for iterator type

namespace util {

template <typename T> class is_iterator {
private:
  template <typename U>
  static std::enable_if_t<!std::is_pointer<U>::value, std::true_type>
  test(typename std::iterator_traits<U>::value_type *);

  template <typename> static std::false_type test(...);

public:
  static constexpr bool value = decltype(test<T>(nullptr))::value;
};

} // namespace util