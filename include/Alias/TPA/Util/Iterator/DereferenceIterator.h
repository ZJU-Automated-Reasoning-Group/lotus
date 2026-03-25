#pragma once

#include "Alias/TPA/Util/Iterator/IteratorAdaptor.h"

namespace util {

// An iterator adapter that takes an iterator over a container of pointers and
// returns the corresponding iterator that automatically dereference those
// pointers
template <typename Iterator, typename ValueT = typename std::remove_reference_t<
                                 decltype(**std::declval<Iterator>())>>
class DereferenceIterator
    : public IteratorAdaptor<
          DereferenceIterator<Iterator>, Iterator,
          typename std::iterator_traits<Iterator>::iterator_category, ValueT> {
private:
  using BaseT = IteratorAdaptor<
      DereferenceIterator<Iterator>, Iterator,
      typename std::iterator_traits<Iterator>::iterator_category, ValueT>;

public:
  DereferenceIterator() = default;
  DereferenceIterator(const Iterator &i) : BaseT(i) {}
  DereferenceIterator(Iterator &&i) : BaseT(std::move(i)) {}

  ValueT &operator*() const { return **this->itr; }
};

template <typename Iterator, typename ValueT = typename std::remove_reference_t<
                                 decltype(**std::declval<Iterator>())>>
class DereferenceConstIterator
    : public IteratorAdaptor<
          DereferenceConstIterator<Iterator>, Iterator,
          typename std::iterator_traits<Iterator>::iterator_category, ValueT> {
private:
  using BaseT = IteratorAdaptor<
      DereferenceConstIterator<Iterator>, Iterator,
      typename std::iterator_traits<Iterator>::iterator_category, ValueT>;

public:
  DereferenceConstIterator() = default;
  DereferenceConstIterator(const Iterator &i) : BaseT(i) {}
  DereferenceConstIterator(Iterator &&i) : BaseT(std::move(i)) {}

  const ValueT &operator*() const { return **this->itr; }
};

template <typename Iterator> auto deref_iterator(Iterator &&itr) {
  using IteratorType = std::remove_reference_t<std::remove_cv_t<Iterator>>;
  return DereferenceIterator<IteratorType>(std::forward<Iterator>(itr));
}

template <typename Iterator> auto deref_const_iterator(Iterator &&itr) {
  using IteratorType = std::remove_reference_t<std::remove_cv_t<Iterator>>;
  return DereferenceConstIterator<IteratorType>(std::forward<Iterator>(itr));
}

} // namespace util