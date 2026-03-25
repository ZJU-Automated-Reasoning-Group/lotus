#pragma once

#include "Alias/TPA/Util/Iterator/IteratorAdaptor.h"

namespace util {

template <typename Iterator,
          typename ValueT =
              typename std::iterator_traits<Iterator>::value_type::second_type>
class MapValueIterator
    : public IteratorAdaptor<
          MapValueIterator<Iterator>, Iterator,
          typename std::iterator_traits<Iterator>::iterator_category, ValueT> {
private:
  using BaseT = IteratorAdaptor<
      MapValueIterator<Iterator>, Iterator,
      typename std::iterator_traits<Iterator>::iterator_category, ValueT>;

public:
  MapValueIterator() = default;
  MapValueIterator(const Iterator &i) : BaseT(i) {}
  MapValueIterator(Iterator &&i) : BaseT(std::move(i)) {}

  ValueT &operator*() const { return this->itr->second; }
};

template <typename Iterator,
          typename ValueT = const typename std::iterator_traits<
              Iterator>::value_type::second_type>
class MapValueConstIterator
    : public IteratorAdaptor<
          MapValueConstIterator<Iterator>, Iterator,
          typename std::iterator_traits<Iterator>::iterator_category, ValueT> {
private:
  using BaseT = IteratorAdaptor<
      MapValueConstIterator<Iterator>, Iterator,
      typename std::iterator_traits<Iterator>::iterator_category, ValueT>;

public:
  MapValueConstIterator() = default;
  MapValueConstIterator(const Iterator &i) : BaseT(i) {}
  MapValueConstIterator(Iterator &&i) : BaseT(std::move(i)) {}

  ValueT &operator*() const { return this->itr->second; }
};

template <typename Iterator>
MapValueIterator<Iterator> mapValueIterator(Iterator &&itr) {
  return MapValueIterator<Iterator>(std::forward<Iterator>(itr));
}

template <typename Iterator>
MapValueConstIterator<Iterator> mapValueConstIterator(Iterator &&itr) {
  return MapValueConstIterator<Iterator>(std::forward<Iterator>(itr));
}

} // namespace util