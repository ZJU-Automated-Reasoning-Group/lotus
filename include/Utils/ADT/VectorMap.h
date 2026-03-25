#pragma once

#include "Utils/ADT/SortedVector.h"

#include <tuple>

namespace util {

template <typename Key, typename Value, typename KeyCompare = std::less<Key>,
          typename Allocator = std::allocator<std::pair<const Key, Value>>>
class VectorMap {
public:
  // Member types
  using key_type = Key;
  using mapped_type = Value;
  using value_type = std::pair<Key, Value>;
  using key_compare = KeyCompare;
  using allocator_type = Allocator;

  class value_compare {
  private:
    KeyCompare kComp;

  protected:
    value_compare(KeyCompare c) : kComp(std::move(c)) {}

  public:
    bool operator()(const value_type &lhs, const value_type &rhs) const {
      return kComp(lhs.first, rhs.first);
    }

    friend class VectorMap;
  };

private:
  using VectorType = SortedVector<value_type, value_compare, Allocator>;
  VectorType vec;

public:
  // Member type (cont.)
  using size_type = typename VectorType::size_type;
  using difference_type = typename VectorType::difference_type;
  using reference = typename VectorType::reference;
  using const_reference = typename VectorType::const_reference;
  using pointer = typename VectorType::pointer;
  using const_pointer = typename VectorType::const_pointer;
  using iterator = typename VectorType::iterator;
  using const_iterator = typename VectorType::const_iterator;
  using reverse_iterator = typename VectorType::reverse_iterator;
  using const_reverse_iterator = typename VectorType::const_reverse_iterator;

  // Constructor
  VectorMap() : VectorMap(KeyCompare()) {}
  explicit VectorMap(const KeyCompare &c, const Allocator &a = Allocator())
      : vec(value_compare(c), a) {}
  explicit VectorMap(const Allocator &a)
      : vec(value_compare(KeyCompare()), a) {}

  template <typename Iterator>
  VectorMap(Iterator first, Iterator last, const KeyCompare &c = KeyCompare(),
            const Allocator &a = Allocator())
      : vec(std::move(first), std::move(last), value_compare(c), a) {}

  VectorMap(std::initializer_list<value_type> init,
            const KeyCompare &c = KeyCompare(),
            const Allocator &a = Allocator())
      : vec(std::move(init), value_compare(c), a) {}

  // Capacity
  bool empty() const noexcept { return vec.empty(); }
  size_type size() const noexcept { return vec.size(); }

  // Element access
  mapped_type &operator[](const key_type &key) {
    return vec.insert(value_type(key, mapped_type())).first->second;
  }
  mapped_type &operator[](key_type &&key) {
    return vec.insert(value_type(std::move(key), mapped_type())).first->second;
  }
  mapped_type &at(const key_type &key) {
    auto itr = find(key);
    if (itr == end())
      throw std::out_of_range("VectorMap::at()");
    else
      return itr->second;
  }
  const mapped_type &at(const key_type &key) const {
    auto itr = find(key);
    if (itr == end())
      throw std::out_of_range("VectorMap::at()");
    else
      return itr->second;
  }

  // Lookup
  iterator lower_bound(const Key &key) {
    return std::lower_bound(vec.begin(), vec.end(), key,
                            [this](const value_type &v, const key_type &k) {
                              return key_comp()(v.first, k);
                            });
  }
  const_iterator lower_bound(const Key &key) const {
    return std::lower_bound(vec.begin(), vec.end(), key,
                            [this](const value_type &v, const key_type &k) {
                              return key_comp()(v.first, k);
                            });
  }
  iterator upper_bound(const Key &key) {
    return std::upper_bound(vec.begin(), vec.end(), key,
                            [this](const key_type &k, const value_type &v) {
                              return key_comp()(k, v.first);
                            });
  }
  const_iterator upper_bound(const Key &key) const {
    return std::upper_bound(vec.begin(), vec.end(), key,
                            [this](const key_type &k, const value_type &v) {
                              return key_comp()(k, v.first);
                            });
  }
  std::pair<iterator, iterator> equal_range(const key_type &k) {
    return std::make_pair(lower_bound(k), upper_bound(k));
  }
  std::pair<const_iterator, const_iterator>
  equal_range(const key_type &k) const {
    return std::make_pair(lower_bound(k), upper_bound(k));
  }

  iterator find(const key_type &key) {
    auto itr = lower_bound(key);
    if (itr != end() && key_comp()(key, itr->first))
      itr = end();
    return itr;
  }
  const_iterator find(const key_type &key) const {
    auto itr = lower_bound(key);
    if (itr != end() && key_comp()(key, itr->first))
      itr = end();
    return itr;
  }
  size_type count(const key_type &key) const { return find(key) != end(); }

  // Modifiers
  void clear() noexcept { vec.clear(); }

  std::pair<iterator, bool> insert(const value_type &value) {
    return vec.insert(value);
  }
  std::pair<iterator, bool> insert(value_type &&value) {
    return vec.insert(std::move(value));
  }

  template <typename Iterator> void insert(Iterator first, Iterator last) {
    vec.insert(first, last);
  }

  template <typename M>
  std::pair<iterator, bool> insert_or_assign(const key_type &key, M &&obj) {
    bool newKey = false;
    auto itr = lower_bound(key);
    if (itr == end() || key_comp()(key, itr->first)) {
      itr = vec.insert(itr, value_type(key, std::forward<M>(obj)));
      newKey = true;
    } else
      itr->second = std::forward<M>(obj);
    return std::make_pair(itr, newKey);
  }
  template <typename M>
  std::pair<iterator, bool> insert_or_assign(key_type &&key, M &&obj) {
    bool newKey = false;
    auto itr = lower_bound(key);
    if (itr == end() || key_comp()(key, itr->first)) {
      itr = vec.insert(itr, value_type(key, std::forward<M>(obj)));
      newKey = true;
    } else
      itr->second = std::forward<M>(obj);
    return std::make_pair(itr, newKey);
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(const key_type &key, Args &&...args) {
    bool changed = false;
    auto itr = lower_bound(key);
    if (itr == end() || key_comp()(key, itr->first)) {
      itr = vec.insert(
          itr, value_type(std::piecewise_construct, std::forward_as_tuple(key),
                          std::forward_as_tuple(std::forward<Args>(args)...)));
      changed = true;
    }
    return std::make_pair(itr, changed);
  }
  template <typename... Args>
  std::pair<iterator, bool> try_emplace(key_type &&key, Args &&...args) {
    bool changed = false;
    auto itr = lower_bound(key);
    if (itr == end() || key_comp()(key, itr->first)) {
      itr = vec.insert(
          itr, value_type(std::piecewise_construct,
                          std::forward_as_tuple(std::move(key)),
                          std::forward_as_tuple(std::forward<Args>(args)...)));
      changed = true;
    }
    return std::make_pair(itr, changed);
  }

  void erase(iterator pos) { vec.erase(pos); }
  void erase(const_iterator pos) { vec.erase(pos); }
  void erase(iterator first, iterator last) { vec.erase(first, last); }
  void erase(const_iterator first, const_iterator last) {
    vec.erase(first, last);
  }
  size_type erase(const key_type &key) {
    auto itr = find(key);
    if (itr != end()) {
      erase(itr);
      return 1;
    } else
      return 0;
  }

  void swap(const VectorMap &rhs) noexcept(noexcept(vec.swap(rhs.vec))) {
    vec.swap(rhs.vec);
  }

  // Compare operators
  template <typename U, typename C, typename A>
  friend bool operator==(const VectorMap<U, C, A> &lhs,
                         const VectorMap<U, C, A> &rhs);
  template <typename U, typename C, typename A>
  friend bool operator!=(const VectorMap<U, C, A> &lhs,
                         const VectorMap<U, C, A> &rhs);
  template <typename U, typename C, typename A>
  friend bool operator<(const VectorMap<U, C, A> &lhs,
                        const VectorMap<U, C, A> &rhs);
  template <typename U, typename C, typename A>
  friend bool operator<=(const VectorMap<U, C, A> &lhs,
                         const VectorMap<U, C, A> &rhs);
  template <typename U, typename C, typename A>
  friend bool operator>(const VectorMap<U, C, A> &lhs,
                        const VectorMap<U, C, A> &rhs);
  template <typename U, typename C, typename A>
  friend bool operator>=(const VectorMap<U, C, A> &lhs,
                         const VectorMap<U, C, A> &rhs);

  // Observers
  key_compare key_comp() const { return vec.value_comp().kComp; }
  value_compare value_comp() const { return vec.value_comp(); }

  // Iterators
  iterator begin() { return vec.begin(); }
  iterator end() { return vec.end(); }
  const_iterator begin() const { return vec.begin(); }
  const_iterator end() const { return vec.end(); }
  const_iterator cbegin() const { return vec.cbegin(); }
  const_iterator cend() const { return vec.cend(); }
  reverse_iterator rbegin() { return vec.rbegin(); }
  reverse_iterator rend() { return vec.rend(); }
  const_reverse_iterator rbegin() const { return vec.rbegin(); }
  const_reverse_iterator rend() const { return vec.rend(); }
  const_reverse_iterator crbegin() const { return vec.crbegin(); }
  const_reverse_iterator crend() const { return vec.crend(); }
};

template <typename T, typename C, typename A>
bool operator==(const VectorMap<T, C, A> &lhs, const VectorMap<T, C, A> &rhs) {
  return lhs.vec == rhs.vec;
}

template <typename T, typename C, typename A>
bool operator!=(const VectorMap<T, C, A> &lhs, const VectorMap<T, C, A> &rhs) {
  return !(lhs == rhs);
}

template <typename T, typename C, typename A>
bool operator<(const VectorMap<T, C, A> &lhs, const VectorMap<T, C, A> &rhs) {
  return lhs.vec < rhs.vec;
}

template <typename T, typename C, typename A>
bool operator>(const VectorMap<T, C, A> &lhs, const VectorMap<T, C, A> &rhs) {
  return rhs < lhs;
}

template <typename T, typename C, typename A>
bool operator<=(const VectorMap<T, C, A> &lhs, const VectorMap<T, C, A> &rhs) {
  return !(rhs < lhs);
}

template <typename T, typename C, typename A>
bool operator>=(const VectorMap<T, C, A> &lhs, const VectorMap<T, C, A> &rhs) {
  return !(lhs < rhs);
}

} // namespace util
