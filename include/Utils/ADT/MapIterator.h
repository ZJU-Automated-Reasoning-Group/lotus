/*
 * @author: rainoftime
 * @brief: MapIterator is a iterator for map data structure.
 * 一个用于映射数据结构的迭代器实现。它提供了高效的遍历和访问映射中的键和值。
 */

#pragma once

#include <iterator>
#include <type_traits>
#include <utility>

template <bool Condition, class T> struct add_const_if {
  typedef T type;
};

template <class T> struct add_const_if<true, T> {
  typedef typename std::add_const<T>::type type;
};

template <class IteratorTy> class key_iterator : public IteratorTy {
public:
  typedef typename add_const_if<
      std::is_const<typename std::remove_reference<
          typename std::iterator_traits<IteratorTy>::reference>::type>::value,
      typename std::iterator_traits<IteratorTy>::value_type::first_type>::type
      value_type;

  key_iterator() : IteratorTy() {}

  key_iterator(IteratorTy It) : IteratorTy(It) {}

  value_type &operator*() const {
    const IteratorTy &It = *this;
    return It->first;
  }

  value_type *operator->() const {
    const IteratorTy &It = *this;
    return &It->first;
  }
};

template <class IteratorTy> class value_iterator : public IteratorTy {
public:
  typedef typename add_const_if<
      std::is_const<typename std::remove_reference<
          typename std::iterator_traits<IteratorTy>::reference>::type>::value,
      typename std::iterator_traits<IteratorTy>::value_type::second_type>::type
      value_type;

  value_iterator() : IteratorTy() {}

  value_iterator(IteratorTy It) : IteratorTy(It) {}

  value_type &operator*() const {
    const IteratorTy &It = *this;
    return It->second;
  }

  value_type *operator->() const {
    const IteratorTy &It = *this;
    return &It->second;
  }
};

template <class IteratorTy> class KeyRange {
public:
  typedef key_iterator<IteratorTy> iterator;

private:
  iterator Begin;
  iterator End;

public:
  KeyRange(iterator B, iterator E) : Begin(B), End(E) {}

  iterator begin() const { return Begin; }

  iterator end() const { return End; }
};

template <class IteratorTy> class ValueRange {
public:
  typedef value_iterator<IteratorTy> iterator;

private:
  iterator Begin;
  iterator End;

public:
  ValueRange(iterator B, iterator E) : Begin(B), End(E) {}

  iterator begin() const { return Begin; }

  iterator end() const { return End; }
};

template <class MapTy>
auto keys(MapTy &&Map) -> KeyRange<decltype(Map.begin())> {
  return KeyRange<decltype(Map.begin())>(Map.begin(), Map.end());
}

template <class MapTy>
auto values(MapTy &&Map) -> ValueRange<decltype(Map.begin())> {
  return ValueRange<decltype(Map.begin())>(Map.begin(), Map.end());
}
