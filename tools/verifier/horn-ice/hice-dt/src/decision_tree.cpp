/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// C++ includes
#include <algorithm>

// Project includes
#include "decision_tree.h"
#include "visitor.h"

namespace horn_verification {

//
// Categorical node
//

categorical_node &categorical_node::operator=(const categorical_node &rhs) {

  categorical_node tmp(rhs);

  std::swap(_attribute, tmp._attribute);
  std::swap(_children, tmp._children);

  return *this;
}

categorical_node &categorical_node::operator=(categorical_node &&rhs) {
  if (this != &rhs) {
    _attribute = rhs._attribute;
    _children = std::move(rhs._children);
  }

  return *this;
}

void categorical_node::accept(base_visitor &visitor) { visitor.visit(*this); }

categorical_node *categorical_node::clone() const {
  return new categorical_node(*this);
}

//
// Integer node
//

int_node &int_node::operator=(const int_node &rhs) {

  int_node tmp(rhs);

  std::swap(_attribute, tmp._attribute);
  std::swap(_threshold, tmp._threshold);
  std::swap(_children, tmp._children);

  return *this;
}

int_node &int_node::operator=(int_node &&rhs) {
  if (this != &rhs) {
    _attribute = rhs._attribute;
    _threshold = rhs._threshold;
    _children = std::move(rhs._children);
  }

  return *this;
}

void int_node::accept(base_visitor &visitor) { visitor.visit(*this); }

int_node *int_node::clone() const { return new int_node(*this); }

//
// Leaf node
//

leaf_node &leaf_node::operator=(const leaf_node &rhs) {

  leaf_node tmp(rhs);

  std::swap(_output, tmp._output);

  return *this;
}

leaf_node &leaf_node::operator=(leaf_node &&rhs) {
  if (this != &rhs) {
    _output = rhs._output;
  }

  return *this;
}

void leaf_node::accept(base_visitor &visitor) { visitor.visit(*this); }

leaf_node *leaf_node::clone() const { return new leaf_node(*this); }

}; // End namespace horn_verification