/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Project includes
#include "pretty_print_visitor.h"

#include "decision_tree.h"

namespace horn_verification {

void pretty_print_visitor::visit(categorical_node &n) {

  for (unsigned int i = 0; i < _indent; ++i) {
    _out << " ";
  }

  _out << "switch x[" << n.attribute() << "]";

  ++_indent;
  for (const auto &child : n.children()) {
    _out << '\n';
    if (child) {
      child->accept(*this);
    } else {

      for (unsigned int i = 0; i < _indent; ++i) {
        _out << " ";
      }
      _out << "NULL";
    }
  }
  --_indent;
}

void pretty_print_visitor::visit(int_node &n) {

  for (unsigned int i = 0; i < _indent; ++i) {
    _out << " ";
  }

  _out << "if x[" << n.attribute() << "] <= " << n.threshold();

  ++_indent;
  for (const auto &child : n.children()) {
    _out << '\n';
    if (child) {
      child->accept(*this);
    } else {

      for (unsigned int i = 0; i < _indent; ++i) {
        _out << " ";
      }
      _out << "NULL";
    }
  }
  --_indent;
}

void pretty_print_visitor::visit(leaf_node &n) {

  for (unsigned int i = 0; i < _indent; ++i) {
    _out << " ";
  }

  _out << (n.output() ? "true" : "false");
}

}; // End namespace horn_verification
