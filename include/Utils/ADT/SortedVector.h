#pragma once

#include <algorithm>
#include <cassert>
#include <vector>

namespace util {

template <typename T, typename Comparator = std::less<T>,
          typename Allocator = std::allocator<T>>
class SortedVector {
private:
  // Use allocator rebind to handle const keys in map allocator
  using AllocValueType = typename std::allocator_traits<Allocator>::value_type;
  using VectorType = std::vector<
      T, typename std::allocator_traits<Allocator>::template rebind_alloc<T>>;
  VectorType vec;
  Comparator comp;

  void sortAndUnique() {
    std::sort(vec.begin(), vec.end(), comp);
    vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
  }

public:
  // Member types
  using value_type = typename VectorType::value_type;
  using allocator_type = typename VectorType::allocator_type;
  using size_type = typename VectorType::size_type;
  using difference_type = typename VectorType::difference_type;
  using value_compare = Comparator;
  using reference = typename VectorType::reference;
  using const_reference = typename VectorType::const_reference;
  using pointer = typename VectorType::pointer;
  using const_pointer = typename VectorType::const_pointer;
  using iterator = typename VectorType::iterator;
  using const_iterator = typename VectorType::const_iterator;
  using reverse_iterator = typename VectorType::reverse_iterator;
  using const_reverse_iterator = typename VectorType::const_reverse_iterator;

  // Construct/Destruct/Copy
  SortedVector() : SortedVector(Comparator()) {}
  explicit SortedVector(const Comparator &c, const Allocator &a = Allocator())
      : vec(a), comp(c) {}
  explicit SortedVector(const Allocator &a) : vec(a), comp(Comparator()) {}

  template <typename Iterator>
  SortedVector(Iterator first, Iterator last,
               const Comparator &c = Comparator(),
               const Allocator &a = Allocator())
      : vec(first, last, a), comp(c) {
    sortAndUnique();
  }

  SortedVector(std::initializer_list<T> init,
               const Comparator &c = Comparator(),
               const Allocator &a = Allocator())
      : vec(std::move(init), a), comp(c) {
    sortAndUnique();
  }

  SortedVector(std::vector<T> &&v, const Comparator &c = Comparator(),
               const Allocator &a = Allocator())
      : vec(std::move(v), a), comp(c) {
    sortAndUnique();
  }

  template <typename Iterator> void assign(Iterator first, Iterator last) {
    vec.assign(first, last);
    sortAndUnique();
  }
  void assign(std::initializer_list<T> init) {
    vec.assign(std::move(init));
    sortAndUnique();
  }

  // Capacity
  bool empty() const noexcept { return vec.empty(); }
  size_type size() const noexcept { return vec.size(); }
  size_type capacity() const noexcept { return vec.capacity(); }
  void reserve(size_type size) { vec.reserve(size); }
  void shrink_to_fit() { vec.shrink_to_fit(); }

  // Element access
  reference operator[](size_type pos) { return vec[pos]; }
  const_reference operator[](size_type pos) const { return vec[pos]; }
  reference at(size_type pos) { return vec.at(pos); }
  const_reference at(size_type pos) const { return vec.at(pos); }
  reference front() { return vec.front(); }
  const_reference front() const { return vec.front(); }
  reference back() { return vec.back(); }
  const_reference back() const { return vec.back(); }

  // Lookup
  iterator lower_bound(const T &elem) {
    return std::lower_bound(begin(), end(), elem, comp);
  }
  const_iterator lower_bound(const T &elem) const {
    return std::lower_bound(begin(), end(), elem, comp);
  }
  iterator upper_bound(const T &elem) {
    return std::upper_bound(begin(), end(), elem, comp);
  }
  const_iterator upper_bound(const T &elem) const {
    return std::upper_bound(begin(), end(), elem, comp);
  }
  std::pair<iterator, iterator> equal_range(const T &elem) {
    return std::equal_range(begin(), end(), elem, comp);
  }
  std::pair<const_iterator, const_iterator> equal_range(const T &elem) const {
    return std::equal_range(begin(), end(), elem, comp);
  }
  iterator find(const T &elem) {
    auto itr = lower_bound(elem);
    if (itr != end() && comp(elem, *itr))
      itr = end();
    return itr;
  }
  const_iterator find(const T &elem) const {
    auto itr = lower_bound(elem);
    if (itr != end() && comp(elem, *itr))
      itr = end();
    return itr;
  }
  size_type count(const T &elem) const { return find(elem) != end(); }

  // Compare operators
  template <typename U, typename C, typename A>
  friend bool operator==(const SortedVector<U, C, A> &lhs,
                         const SortedVector<U, C, A> &rhs);
  template <typename U, typename C, typename A>
  friend bool operator!=(const SortedVector<U, C, A> &lhs,
                         const SortedVector<U, C, A> &rhs);
  template <typename U, typename C, typename A>
  friend bool operator<(const SortedVector<U, C, A> &lhs,
                        const SortedVector<U, C, A> &rhs);
  template <typename U, typename C, typename A>
  friend bool operator<=(const SortedVector<U, C, A> &lhs,
                         const SortedVector<U, C, A> &rhs);
  template <typename U, typename C, typename A>
  friend bool operator>(const SortedVector<U, C, A> &lhs,
                        const SortedVector<U, C, A> &rhs);
  template <typename U, typename C, typename A>
  friend bool operator>=(const SortedVector<U, C, A> &lhs,
                         const SortedVector<U, C, A> &rhs);

  // Modifiers
  void clear() noexcept { vec.clear(); }
  void swap(SortedVector &rhs) noexcept(noexcept(vec.swap(rhs.vec)) &&
                                        noexcept(std::swap(comp, rhs.comp))) {
    vec.swap(rhs.vec);
    std::swap(comp, rhs.comp);
  }

  std::pair<iterator, bool> insert(const T &elem) {
    auto changed = false;
    auto itr = lower_bound(elem);
    if (itr == end() || comp(elem, *itr)) {
      itr = vec.insert(itr, elem);
      changed = true;
    }
    return std::make_pair(itr, changed);
  }
  std::pair<iterator, bool> insert(T &&elem) {
    auto changed = false;
    auto itr = lower_bound(elem);
    if (itr == end() || comp(elem, *itr)) {
      itr = vec.insert(itr, std::move(elem));
      changed = true;
    }
    return std::make_pair(itr, changed);
  }
  iterator insert(iterator pos, const T &elem) {
    auto okBefore = pos == begin() || comp(*(pos - 1), elem);
    auto okAfter = pos == end() || comp(elem, *pos);
    if (okBefore && okAfter)
      return vec.insert(pos, elem);
    else
      return insert(elem).first;
  }
  iterator insert(iterator pos, T &&elem) {
    auto okBefore = pos == begin() || comp(*(pos - 1), elem);
    auto okAfter = pos == end() || comp(elem, *pos);
    if (okBefore && okAfter)
      return vec.insert(pos, std::move(elem));
    else
      return insert(std::move(elem)).first;
  }
  template <typename Iterator> void insert(Iterator first, Iterator last) {
    vec.reserve(vec.size() + std::distance(first, last));
    std::copy(first, last, std::back_inserter(vec));
    sortAndUnique();
  }

  value_compare value_comp() const { return comp; }

  void erase(iterator pos) {
    assert(pos != end());
    vec.erase(pos);
  }
  void erase(const_iterator pos) {
    assert(pos != end());
    vec.erase(pos);
  }
  void erase(iterator first, iterator last) { vec.erase(first, last); }
  void erase(const_iterator first, const_iterator last) {
    vec.erase(first, last);
  }
  size_type erase(const T &elem) {
    auto itr = find(elem);
    if (itr != end()) {
      erase(itr);
      return 1;
    } else
      return 0;
  }

  // Additional utilities
  bool merge(const SortedVector &rhs) {
    std::vector<T, Allocator> newVec;
    auto prevSize = size();
    newVec.reserve(prevSize + rhs.size());

    std::merge(vec.begin(), vec.end(), rhs.vec.begin(), rhs.vec.end(),
               std::back_inserter(newVec));
    newVec.erase(std::unique(newVec.begin(), newVec.end()), newVec.end());

    vec.swap(newVec);
    return prevSize != size();
  }

  bool includes(const SortedVector &rhs) const {
    return std::includes(vec.begin(), vec.end(), rhs.vec.begin(), rhs.vec.end(),
                         comp);
  }

  static std::vector<T> intersects(const SortedVector &lhs,
                                   const SortedVector &rhs) {
    std::vector<T> ret;
    ret.reserve(std::min(lhs.size(), rhs.size()));
    std::set_intersection(lhs.vec.begin(), lhs.vec.end(), rhs.begin(),
                          rhs.end(), std::back_inserter(ret), Comparator());
    return ret;
  }

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
bool operator==(const SortedVector<T, C, A> &lhs,
                const SortedVector<T, C, A> &rhs) {
  return lhs.vec == rhs.vec;
}

template <typename T, typename C, typename A>
bool operator!=(const SortedVector<T, C, A> &lhs,
                const SortedVector<T, C, A> &rhs) {
  return !(lhs == rhs);
}

template <typename T, typename C, typename A>
bool operator<(const SortedVector<T, C, A> &lhs,
               const SortedVector<T, C, A> &rhs) {
  return lhs.vec < rhs.vec;
}

template <typename T, typename C, typename A>
bool operator>(const SortedVector<T, C, A> &lhs,
               const SortedVector<T, C, A> &rhs) {
  return rhs < lhs;
}

template <typename T, typename C, typename A>
bool operator<=(const SortedVector<T, C, A> &lhs,
                const SortedVector<T, C, A> &rhs) {
  return !(rhs < lhs);
}

template <typename T, typename C, typename A>
bool operator>=(const SortedVector<T, C, A> &lhs,
                const SortedVector<T, C, A> &rhs) {
  return !(lhs < rhs);
}

} // namespace util
