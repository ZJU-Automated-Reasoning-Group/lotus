/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Project includes
#include "output_visitor.h"

#include "decision_tree.h"
#include "error.h"

namespace horn_verification {

bool output_visitor::output(base_node &root, datapoint<bool> &datapoint) {

  _datapoint = &datapoint;
  _output = false;

  root.accept(*this);

  _datapoint = nullptr;

  return _output;
}

void output_visitor::visit(categorical_node &n) {

  if (!n.children()[_datapoint->_categorical_data[n.attribute()]]) {
    throw internal_error("No child for categorical value in the tree");
  }

  n.children()[_datapoint->_categorical_data[n.attribute()]]->accept(*this);
}

void output_visitor::visit(int_node &n) {

  if (_datapoint->_int_data[n.attribute()] <= n.threshold()) {
    n.children()[0]->accept(*this);
  } else {
    n.children()[1]->accept(*this);
  }
}

void output_visitor::visit(leaf_node &n) { _output = n.output(); }

}; // End namespace horn_verification