/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Project includes
#include "horn_constraint.h"

namespace horn_verification {

template <class T> horn_constraint<T>::horn_constraint() {
  _size_of_premises = 0;
  _satisfiable = false;
  _conclusion = 0;
}

template <class T> horn_constraint<T>::~horn_constraint() {}

template <class T>
horn_constraint<T>::horn_constraint(const std::vector<datapoint<T> *> &premises,
                                    datapoint<T> *conclusion) {
  _size_of_premises = premises.size();
  _satisfiable = false;
  _conclusion = conclusion;
  _premises = premises;
}

template <class T>
horn_constraint<T>::horn_constraint(const std::vector<datapoint<T> *> &premises,
                                    datapoint<T> *conclusion,
                                    bool satisfiable) {
  _size_of_premises = premises.size();
  _satisfiable = satisfiable;
  _conclusion = conclusion;
  _premises = premises;
}

template <class T> void horn_constraint<T>::compute_marking() const {
  if (_premises.size()) {
    datapoint<T> *master_datapoint = _premises.at(0);
    bool propagate_marking = false;
    for (auto master_marking_itrator =
             master_datapoint->_list_of_marking.begin();
         master_marking_itrator != master_datapoint->_list_of_marking.end();
         master_marking_itrator++) {
      datapoint<T> *master_marking = (*master_marking_itrator);
      bool master_common_marking_satus = true;
      for (unsigned j = 1; (_premises.size() > 1) && (j < _premises.size());
           j++) {
        datapoint<T> *current_datapoint = _premises.at(j);
        bool local_common_marking_satus = false;
        for (auto local_marking_itrator =
                 current_datapoint->_list_of_marking.begin();
             local_marking_itrator != current_datapoint->_list_of_marking.end();
             local_marking_itrator++) {
          datapoint<T> *local_marking = (*local_marking_itrator);
          if (local_marking == master_marking) {
            local_common_marking_satus = true;
            break;
          }
        }
        if (local_common_marking_satus == false) {
          master_common_marking_satus = false;
          break;
        }
      }
      if (master_common_marking_satus == true) {
        if (_conclusion->_list_of_marking.count(master_marking) == 0) {
          _conclusion->_list_of_marking.insert(master_marking);
          propagate_marking = true;
        }
      }
    }
    if (propagate_marking == true) {
      for (auto horn_constraint_itrator =
               _conclusion->_list_of_horn_constraints.begin();
           horn_constraint_itrator !=
           _conclusion->_list_of_horn_constraints.end();
           horn_constraint_itrator++) {
        if ((*horn_constraint_itrator)->_satisfiable == false &&
            (*horn_constraint_itrator)->_conclusion != _conclusion) {
          (*horn_constraint_itrator)->compute_marking();
        }
      }
    }
  }
}
template class horn_constraint<bool>;
}; // namespace horn_verification
