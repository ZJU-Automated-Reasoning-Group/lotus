/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// C++ includes
#include <algorithm>

// Project includes
#include "json_visitor.h"

namespace horn_verification {

void json_visitor::visit(categorical_node &node) {

  _out << "{\"attribute\":\"" << _metadata.categorical_names()[node.attribute()]
       << "\",\"cut\":0,\"classification\":true,\"children\":[";

  bool first = true;
  for (auto &child : node.children()) {

    if (first) {
      first = false;
    } else {
      _out << ",";
    }

    if (child) {
      child->accept(*this);
    } else {
      _out << "null";
    }
  }

  _out << "]}";
}

void json_visitor::visit(int_node &node) {

  _out << "{\"attribute\":\"" << _metadata.int_names()[node.attribute()]
       << "\",\"cut\":" << node.threshold()
       << ",\"classification\":true,\"children\":[";

  node.children()[0]->accept(*this);
  _out << ",";
  node.children()[1]->accept(*this);

  _out << "]}";
}

void json_visitor::visit(leaf_node &node) {
  _out << "{\"attribute\":\"\",\"cut\":0,\"classification\":"
       << (node.output() ? "true" : "false") << ",\"children\":null}";
}

}; // End namespace horn_verification