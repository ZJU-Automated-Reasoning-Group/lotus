/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// C++ includes
#include <algorithm>
#include <iostream>

// C includes
#include <cassert>

// Project includes
#include "job.h"

namespace horn_verification {

std::vector<slice>
categorical_split_job::run(std::vector<datapoint<bool> *> &datapoint_ptrs,
                           const attributes_metadata &metadata) {

  // 0) Check parameters
  // Because the learner in connection with the Boogie teacher needs to split on
  // the unique categoricla attribute, in the first round this slice can be of a
  // single data point
  // assert (_slice._left_index < _slice._right_index);

  // 1) Sort datapoints
  auto comparer = [this](const datapoint<bool> *const a,
                         const datapoint<bool> *const b) {
    return a->_categorical_data[this->_attribute] <
           b->_categorical_data[this->_attribute];
  };
  std::sort(datapoint_ptrs.begin() + _slice._left_index,
            datapoint_ptrs.begin() + _slice._right_index + 1, comparer);
  // for_each(datapoint_ptrs.begin() + _slice._left_index,
  // datapoint_ptrs.begin() + _slice._right_index + 1, [](const datapoint<bool>
  // * dp) { std::cout << *dp << std::endl; });

  // 2) Create new categorical node
  auto new_node = new categorical_node(
      _attribute, metadata.number_of_categories()[_attribute]);
  *(_slice._node_ptr) = new_node;

  // 3) Create new tasks
  auto new_slices = std::vector<slice>();

  auto cur_left = _slice._left_index;
  auto cur_right = cur_left;

  while (cur_right <= _slice._right_index) {

    auto cur_category = datapoint_ptrs[cur_left]->_categorical_data[_attribute];

    while (cur_right + 1 <= _slice._right_index &&
           cur_category ==
               datapoint_ptrs[cur_right + 1]->_categorical_data[_attribute]) {
      ++cur_right;
    }

    new_slices.push_back(
        slice(cur_left, cur_right, new_node->children().data() + cur_category));

    cur_left = cur_right + 1;
    cur_right = cur_left;
  }

  return new_slices;
}

std::vector<slice>
int_split_job::run(std::vector<datapoint<bool> *> &datapoint_ptrs,
                   const attributes_metadata &metadata) {

  // 0) Check parameters
  assert(_slice._left_index < _slice._right_index);

  // 1) Sort datapoints
  auto comparer = [this](const datapoint<bool> *const a,
                         const datapoint<bool> *const b) {
    return a->_int_data[this->_attribute] < b->_int_data[this->_attribute];
  };
  std::sort(datapoint_ptrs.begin() + _slice._left_index,
            datapoint_ptrs.begin() + _slice._right_index + 1, comparer);
  // auto pos = _slice._left_index;
  // for_each(datapoint_ptrs.begin() + _slice._left_index,
  // datapoint_ptrs.begin() + _slice._right_index + 1, [&pos](const
  // datapoint<bool> * dp) { std::cout << (pos++) << ": " << *dp << std::endl;
  // });

  // 2) Find index to split at
  assert(
      datapoint_ptrs[_slice._left_index]->_int_data[_attribute] <= _threshold &&
      datapoint_ptrs[_slice._right_index]->_int_data[_attribute] > _threshold);
  auto split_at_index = _slice._right_index;
  while (datapoint_ptrs[split_at_index]->_int_data[_attribute] > _threshold) {
    --split_at_index;
  }

  // 3) Create new int node
  auto new_node = new int_node(_attribute, _threshold);
  *(_slice._node_ptr) = new_node;

  // 4) Return new slices
  auto new_slices = std::vector<slice>();
  // new_slices.reserve(2);
  new_slices.push_back(
      slice(_slice._left_index, split_at_index, new_node->children().data()));
  new_slices.push_back(slice(split_at_index + 1, _slice._right_index,
                             new_node->children().data() + 1));

  return new_slices;
}

std::vector<slice>
leaf_creation_job::run(std::vector<datapoint<bool> *> &datapoint_ptrs,
                       const attributes_metadata &metadata) {

  // 0) Check parameters
  assert(_slice._left_index <= _slice._right_index);

  // 1) Create new leaf node
  *(_slice._node_ptr) = new leaf_node(_label);

  // 2) Apply positive and negative labels of data points
  for (auto &dp : _positive_ptrs) {
    dp->_is_classified = true;
    dp->_classification = true;
  }
  for (auto &dp : _negative_ptrs) {
    dp->_is_classified = true;
    dp->_classification = false;
  }

  return std::vector<slice>();
}

}; // End namespace horn_verification
