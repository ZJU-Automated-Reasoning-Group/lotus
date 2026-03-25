/*
 * This file implements two containers, PushPopVector
 * and PushPopCache, which support push/pop operations.
 *
 * Whenever a pop operation is called, the elements
 * added to the structure after last push operation.
 *
 * PushPopCache guarantees the uniqueness of the elements
 * in the container, while PushPopVector does not have the
 * guarantee.
 *
 * Usage senecrios: ?
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

template <typename T> class PushPopVector {
protected:
  std::vector<size_t> CacheStack;
  std::vector<T> CacheVector;

  size_t LastSz = 0;

public:
  virtual ~PushPopVector() {}

  virtual void add(T N) { CacheVector.push_back(N); }

  void push_back(T N) { add(N); }

  template <typename VecTy> void addAll(VecTy &Vec) {
    for (unsigned I = 0; I < Vec.size(); I++) {
      push_back(Vec[I]);
    }
  }

  T &operator[](size_t Index) {
    assert(Index < size());
    return CacheVector[Index];
  }

  virtual void push() { CacheStack.push_back(CacheVector.size()); }

  virtual void pop(unsigned N = 1) {
    size_t OrigSz = size();
    size_t TargetSz = OrigSz;
    while (N-- > 0) {
      assert(!CacheStack.empty());
      TargetSz = CacheStack.back();
      CacheStack.pop_back();
    }

    if (TargetSz != OrigSz) {
      assert(TargetSz < OrigSz);
      CacheVector.erase(CacheVector.begin() + TargetSz, CacheVector.end());
    }

    if (LastSz > CacheVector.size()) {
      LastSz = CacheVector.size();
    }
  }

  virtual void reset() {
    CacheStack.clear();
    CacheVector.clear();
    LastSz = 0;
  }

  size_t size() const { return CacheVector.size(); }

  bool empty() const { return !size(); }

  const std::vector<T> &getCacheVector() const { return CacheVector; }

  /// This function only gets the elements you have
  /// not got using this function.
  ///
  /// The first call of this function returns the all
  /// the elements. A second call of the function
  /// with an argument \c false will return elements
  /// newly added. A second call of the function with
  /// an argument \c true will return all the elements.
  std::pair<typename std::vector<T>::iterator,
            typename std::vector<T>::iterator>
  getCacheVector(bool Restart) {
    if (Restart) {
      LastSz = 0;
    }

    size_t Start = LastSz;
    size_t End = CacheVector.size();

    LastSz = End;
    return std::make_pair(CacheVector.begin() + Start, CacheVector.end());
  }
};

template <typename T> class PushPopCache : public PushPopVector<T> {
protected:
  std::unordered_set<T> CacheSet;

public:
  virtual ~PushPopCache() {}

  bool contains(T N) const { return CacheSet.count(N); }

  virtual void add(T N) {
    if (!contains(N)) {
      CacheSet.insert(N);
      PushPopVector<T>::add(N);
    }

    // since we always test inCache(N) before calling this method
    // it guarantees CacheVector.size() == CacheSet.size(),
    // which means that element in the vector is unique.
    assert(PushPopVector<T>::CacheVector.size() == CacheSet.size());
  }

  virtual void pop(unsigned N = 1) {
    unsigned M = 0;
    while (M < N) {
      size_t P = PushPopVector<T>::CacheStack.back();
      PushPopVector<T>::CacheStack.pop_back();
      while (PushPopVector<T>::CacheVector.size() > P) {
        auto Nd = PushPopVector<T>::CacheVector.back();
        PushPopVector<T>::CacheVector.pop_back();
        CacheSet.erase(Nd);
      }

      M++;
    }

    if (PushPopVector<T>::LastSz > CacheSet.size()) {
      PushPopVector<T>::LastSz = CacheSet.size();
    }
  }

  virtual void reset() {
    PushPopVector<T>::reset();
    CacheSet.clear();
  }

  const std::unordered_set<T> &getCacheSet() { return CacheSet; }
};
