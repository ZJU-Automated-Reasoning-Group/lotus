#pragma once

#include <utility>

namespace util {

// A range adaptor for a pair of iterators
template <typename Iterator> class IteratorRange {
private:
  Iterator first, last;

public:
  using value_type = typename std::iterator_traits<Iterator>::value_type;

  IteratorRange(Iterator s, Iterator e)
      : first(std::move(s)), last(std::move(e)) {}

  Iterator begin() const { return first; }
  Iterator end() const { return last; }

  typename std::iterator_traits<Iterator>::difference_type size() const {
    return std::distance(first, last);
  }
  bool empty() const { return first == last; }
};

template <typename T> IteratorRange<T> iteratorRange(T x, T y) {
  return IteratorRange<T>(std::move(x), std::move(y));
}

template <typename T> IteratorRange<T> iteratorRange(std::pair<T, T> pair) {
  return IteratorRange<T>(std::move(pair.first), std::move(pair.second));
}

} // namespace util