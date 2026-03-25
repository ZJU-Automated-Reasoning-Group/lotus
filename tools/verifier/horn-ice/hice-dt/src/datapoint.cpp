/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// C++ includes
#include <algorithm>
#include <cassert>

// Project includes
#include "datapoint.h"

namespace horn_verification {
template <class T> datapoint<T>::datapoint(unsigned identifier) {
  _classification = false;
  _is_classified = false;
  _identifier = identifier;
}

template <class T>
datapoint<T>::datapoint(T classification, bool is_classified,
                        unsigned identifier) {
  _classification = classification;
  _is_classified = is_classified;
  _identifier = identifier;
}

template <class T>
datapoint<T>::datapoint(
    T classification, bool is_classified,
    std::unordered_set<datapoint<T> *> list_of_marking,
    std::unordered_set<horn_constraint<T> *> list_of_horn_constraints,
    unsigned identifier) {
  _classification = classification;
  _is_classified = is_classified;
  _list_of_marking = list_of_marking;
  _list_of_horn_constraints = list_of_horn_constraints;
  _identifier = identifier;
}

template <class T> bool datapoint<T>::set_classification(T classification) {
  if (_is_classified) {
    if (classification != _classification) {
      return false;
    }
  } else {
    _classification = classification;
    _is_classified = true;
  }
  return true;
}

template <class T> void datapoint<T>::remove_satisfied_horn_clauses() {
  for (auto horn_constraint_itrator = _list_of_horn_constraints.begin();
       horn_constraint_itrator != _list_of_horn_constraints.end();
       horn_constraint_itrator++) {
    if ((*horn_constraint_itrator)->_satisfiable == true) {
      _list_of_horn_constraints.erase(horn_constraint_itrator);
      if (!_list_of_horn_constraints.empty()) {
        horn_constraint_itrator = _list_of_horn_constraints.begin();
      } else {
        break;
      }
    }
  }
}

template <class T>
bool datapoint<T>::is_distinguishable(const datapoint<T> &other,
                                      unsigned int threshold) {
  // Check if any categorical attribute can distinguish the datapoints
  assert(this->_categorical_data.size() == other._categorical_data.size());

  for (auto it1 = this->_categorical_data.cbegin(),
            it2 = other._categorical_data.cbegin();
       it1 != this->_categorical_data.cend() &&
       it2 != other._categorical_data.cend();
       ++it1, ++it2) {
    if (*it1 != *it2) {
      // Data points can be distinguished using the current categorical
      // attribute.
      return true;
    }
  }

  // Check if numerical attributes can distinguish the datapoints, given the
  // threshold
  assert(this->_int_data.size() == other._int_data.size());

  for (auto it1 = this->_int_data.cbegin(), it2 = other._int_data.cbegin();
       it1 != this->_int_data.cend() && it2 != other._int_data.cend();
       ++it1, ++it2) {
    if (*it1 == *it2) {
      // Data points cannot be distinguished using the current attribute.
      continue;
    }

    // val1 stores the smaller of the two values *it1, *it2
    int val1 = (*it1 < *it2) ? *it1 : *it2;
    int val2 = (*it1 < *it2) ? *it2 : *it1;

    if (val1 <= 0 && val2 > 0) {
      // Data points can be distinguished using a numerical split with cut value
      // = 0
      return true;
    } else if (val2 <= 0) {
      // Both data values are negative
      unsigned int min_possible_cut = -1 * (val2 - 1);
      if (min_possible_cut <= threshold) {
        return true;
      }
    } else if (val1 > 0) {
      // Both data values are psotive
      unsigned int min_possible_cut = val1;
      if (min_possible_cut <= threshold) {
        return true;
      }
    }
  }
  // None of the categorical or numerical attributes can distinguish the two
  // data points
  return false;
}

template class datapoint<bool>;

/*====================
 * Datapoint hasher
 =====================*/

std::size_t datapoint_hasher::operator()(const datapoint<bool> &dp) const {

  std::size_t cat_hash = 378551;
  for_each(dp._categorical_data.cbegin(), dp._categorical_data.cend(),
           [&cat_hash](unsigned int i) { cat_hash += (cat_hash >> 2) + i; });

  std::size_t int_hash = 63689;
  for_each(dp._int_data.cbegin(), dp._int_data.cend(),
           [&int_hash](int i) { int_hash += (int_hash >> 2) + i; });

  return cat_hash ^
         (int_hash + 0x9e3779b97f4a7c15 + (cat_hash << 6) + (cat_hash >> 2));
}

std::size_t datapoint_hasher::operator()(const datapoint<bool> *dp) const {

  std::size_t cat_hash = 378551;
  std::size_t int_hash = 63689;

  if (dp != nullptr) {
    for_each(dp->_categorical_data.cbegin(), dp->_categorical_data.cend(),
             [&cat_hash](unsigned int i) { cat_hash += (cat_hash >> 2) + i; });
    for_each(dp->_int_data.cbegin(), dp->_int_data.cend(),
             [&int_hash](int i) { int_hash += (int_hash >> 2) + i; });
  }

  return cat_hash ^
         (int_hash + 0x9e3779b97f4a7c15 + (cat_hash << 6) + (cat_hash >> 2));
}

bool datapoint_hasher::operator()(const datapoint<bool> &x,
                                  const datapoint<bool> &y) const {
  return x._int_data == y._int_data &&
         x._categorical_data == y._categorical_data;
}

bool datapoint_hasher::operator()(const datapoint<bool> *x,
                                  const datapoint<bool> *y) const {

  if (x == y) {
    return true;
  } else if (x != nullptr && y != nullptr) {
    return x->_int_data == y->_int_data &&
           x->_categorical_data == y->_categorical_data;
  } else {
    return false;
  }
}

}; // namespace horn_verification
