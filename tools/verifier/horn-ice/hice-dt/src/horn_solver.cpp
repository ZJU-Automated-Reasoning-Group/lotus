/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "horn_solver.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace std;

namespace horn_verification {
template <class T> horn_solver<T>::~horn_solver() {}

template <class T>
bool horn_solver<T>::propagate_false_classification(
    horn_constraint<T> *current_horn_clause_addr,
    datapoint<T> *current_variable_addr,
    std::unordered_set<datapoint<T> *> &negative) const {
  for (unsigned j = 0; j < current_horn_clause_addr->_premises.size(); j++) {
    if (current_horn_clause_addr->_premises.at(j) == current_variable_addr) {
      current_horn_clause_addr->_satisfiable = true;
      return true;
    }
  }
  if (current_horn_clause_addr->_conclusion == current_variable_addr) {
    if (current_horn_clause_addr->_size_of_premises == 1) {
      horn_constraints_satisfiability =
          current_horn_clause_addr->_premises.at(0)->set_classification(false);
      if (horn_constraints_satisfiability) {
        worklist.push(current_horn_clause_addr->_premises.at(0));
        negative.insert(current_horn_clause_addr->_premises.at(0));
        current_horn_clause_addr->_satisfiable = true;
        return true;
      } else {
        return false;
      }
    }
    if (_compute_current_marking) {
      compute_current_marking();
      _compute_current_marking = false;
    }
    for (auto current_marking_itrator =
             current_variable_addr->_list_of_marking.begin();
         current_marking_itrator !=
         current_variable_addr->_list_of_marking.end();
         current_marking_itrator++) {
      if ((*current_marking_itrator) != current_variable_addr) {
        if ((!(*current_marking_itrator)->_is_classified) ||
            (*current_marking_itrator)->_classification) {
          horn_constraints_satisfiability =
              (*current_marking_itrator)->set_classification(false);
          if (horn_constraints_satisfiability) {
            worklist.push((*current_marking_itrator));
            negative.insert((*current_marking_itrator));
          } else {
            return false;
          }
        }
      }
    }
  }
  return true;
}

template <class T>
void horn_solver<T>::remove_from_vector(
    std::vector<datapoint<T> *> &vector_of_objects,
    datapoint<T> *object_to_romove) const {
  for (unsigned i = 0; i < vector_of_objects.size();) {
    if (vector_of_objects.at(i) == object_to_romove) {
      vector_of_objects.erase(vector_of_objects.begin() + i);
    } else {
      i++;
    }
  }
}

template <class T>
bool horn_solver<T>::propagate_true_classification(
    horn_constraint<T> *current_horn_clause_addr,
    datapoint<T> *current_variable_addr,
    std::unordered_set<datapoint<T> *> &positive) const {
  if (current_horn_clause_addr->_conclusion == current_variable_addr) {
    current_horn_clause_addr->_satisfiable = true; // Horn clause get satisfied
    return true;
  } else {
    remove_from_vector(current_horn_clause_addr->_premises,
                       current_variable_addr);
    current_horn_clause_addr->_size_of_premises =
        current_horn_clause_addr->_premises.size();
    if (current_horn_clause_addr->_size_of_premises == 0) {
      horn_constraints_satisfiability =
          current_horn_clause_addr->_conclusion->set_classification(true);
      if (horn_constraints_satisfiability) {
        worklist.push(current_horn_clause_addr->_conclusion);
        positive.insert(current_horn_clause_addr->_conclusion);
        current_horn_clause_addr->_satisfiable = true;
      } else {
        return false;
      }
    }
  }
  return true;
}

template <class T> void horn_solver<T>::compute_current_marking() const {
  for (unsigned i = 0; i < _copy_of_horn_constraints.size(); i++) {
    if (!_copy_of_horn_constraints.at(i)._satisfiable) {
      _copy_of_horn_constraints.at(i).compute_marking();
    }
  }
}

template <class T>
void horn_solver<T>::fill_worklist(
    const std::vector<datapoint<T> *> &datapoints,
    std::unordered_set<datapoint<T> *> &positive,
    std::unordered_set<datapoint<T> *> &negative) const {
  // Adding false datapoints to worklist
  if (_horn_constraints_without_conclusion) {
    worklist.push(&_false_datapoint);
  }
  // Adding negative datapoints to worklist
  if (!negative.empty()) {
    for (auto negative_itrator = negative.begin();
         negative_itrator != negative.end(); negative_itrator++) {
      (*negative_itrator)->set_classification(false);
      worklist.push(*negative_itrator);
    }
  }
  // Adding positive datapoints to worklist
  if (!positive.empty()) {
    for (auto positive_itrator = positive.begin();
         positive_itrator != positive.end(); positive_itrator++) {
      (*positive_itrator)->set_classification(true);
      worklist.push(*positive_itrator);
    }
  }
}

template <class T>
void horn_solver<T>::roll_back_datapoints(
    const std::vector<datapoint<T> *> &datapoints) const {
  for (unsigned i = 0; i < datapoints.size(); i++) {
    datapoints.at(i)->_classification =
        _copy_of_datapoints.at(i)._classification;
    datapoints.at(i)->_is_classified = _copy_of_datapoints.at(i)._is_classified;
    datapoints.at(i)->_list_of_marking.clear();
    datapoints.at(i)->_list_of_horn_constraints.clear();
  }
}

template <class T>
void horn_solver<T>::populate_meta_data(
    const std::vector<datapoint<T> *> &datapoints,
    std::vector<horn_constraint<T>> &horn_constraints) const {
  for (unsigned i = 0; i < datapoints.size();
       i++) { /* assign defualt marking */
    datapoints.at(i)->_list_of_marking.insert(datapoints.at(i));
  }
  for (unsigned i = 0; i < horn_constraints.size();
       i++) { /* setting list_of_horn_constraints */
    if (horn_constraints.at(i)._satisfiable == false) {
      horn_constraints.at(i)._conclusion->_list_of_horn_constraints.insert(
          &horn_constraints.at(i));
      for (unsigned j = 0; j < horn_constraints.at(i)._premises.size(); j++) {
        horn_constraints.at(i)
            ._premises.at(j)
            ->_list_of_horn_constraints.insert(&horn_constraints.at(i));
      }
    }
  }
}

template <class T>
void horn_solver<T>::keep_backup(
    const std::vector<datapoint<T> *> &datapoints,
    const std::vector<horn_constraint<T>> &horn_constraints,
    std::unordered_set<datapoint<T> *> &positive,
    std::unordered_set<datapoint<T> *> &negative) const {
  _copy_of_positive.clear();
  _copy_of_negative.clear();
  _copy_of_datapoints.clear();
  _copy_of_horn_constraints.clear();
  _false_datapoint._list_of_marking.clear();
  _false_datapoint.set_classification(false);
  _false_datapoint._list_of_horn_constraints.clear();
  assert(_false_datapoint._list_of_marking.size() == 0);
  assert(_false_datapoint._list_of_horn_constraints.size() == 0);
  if (!positive.empty()) {
    _copy_of_positive.reserve(positive.size());
  }
  if (!negative.empty()) {
    _copy_of_negative.reserve(negative.size());
  }
  if (!datapoints.empty()) {
    _copy_of_datapoints.reserve(datapoints.size());
  }
  if (!horn_constraints.empty()) {
    _copy_of_horn_constraints.reserve(horn_constraints.size());
  }
  for (auto positive_itrator = positive.begin();
       positive_itrator != positive.end(); positive_itrator++) {
    datapoint<T> copy_of_positive_datapoint(
        (*positive_itrator)->_classification,
        (*positive_itrator)->_is_classified, (*positive_itrator)->_identifier);
    _copy_of_positive.push_back(copy_of_positive_datapoint);
  }
  for (auto negative_itrator = negative.begin();
       negative_itrator != negative.end(); negative_itrator++) {
    datapoint<T> copy_of_negative_datapoint(
        (*negative_itrator)->_classification,
        (*negative_itrator)->_is_classified, (*negative_itrator)->_identifier);
    _copy_of_negative.push_back(copy_of_negative_datapoint);
  }

  for (unsigned i = 0; i < datapoints.size(); i++) {
    datapoint<T> copy_of_datapoint(datapoints.at(i)->_classification,
                                   datapoints.at(i)->_is_classified,
                                   datapoints.at(i)->_identifier);
    _copy_of_datapoints.push_back(copy_of_datapoint);
  }
  for (unsigned i = 0; i < horn_constraints.size(); i++) {
    datapoint<T> *_temporary_conclusion;
    if (horn_constraints.at(i)._conclusion == NULL) {
      _temporary_conclusion = &_false_datapoint;
      _horn_constraints_without_conclusion = true;
    } else {
      _temporary_conclusion = horn_constraints.at(i)._conclusion;
    }
    horn_constraint<T> copy_of_horn_constraint(
        horn_constraints.at(i)._premises, _temporary_conclusion,
        horn_constraints.at(i)._satisfiable);
    if ((copy_of_horn_constraint._conclusion->_is_classified == true) &&
        (copy_of_horn_constraint._conclusion->_classification == true)) {
      copy_of_horn_constraint._satisfiable = true;
    }

    for (unsigned j = 0; j < copy_of_horn_constraint._premises.size(); j++) {
      if ((copy_of_horn_constraint._premises.at(j)->_is_classified == true) &&
          (copy_of_horn_constraint._premises.at(j)->_classification == false)) {
        copy_of_horn_constraint._satisfiable = true;
      }
    }
    for (unsigned j = 0; j < copy_of_horn_constraint._premises.size();) {
      if ((copy_of_horn_constraint._premises.at(j)->_is_classified == true) &&
          (copy_of_horn_constraint._premises.at(j)->_classification == true)) {
        remove_from_vector(copy_of_horn_constraint._premises,
                           copy_of_horn_constraint._premises.at(j));
        copy_of_horn_constraint._size_of_premises =
            copy_of_horn_constraint._premises.size();
      } else {
        j++;
      }
    }

    if ((copy_of_horn_constraint._size_of_premises == 0) &&
        (copy_of_horn_constraint._satisfiable == false)) {
      for (auto negative_itrator = negative.begin();
           negative_itrator != negative.end(); negative_itrator++) {
        if ((*negative_itrator) == copy_of_horn_constraint._conclusion) {
          copy_of_horn_constraint._conclusion->set_classification(false);
        }
      }
      horn_constraints_satisfiability =
          copy_of_horn_constraint._conclusion->set_classification(true);
      if (horn_constraints_satisfiability == false) {
        return;
      } else {
        positive.insert(copy_of_horn_constraint._conclusion);
        copy_of_horn_constraint._satisfiable = true;
      }
    }

    if ((copy_of_horn_constraint._conclusion->_is_classified == true) &&
        (copy_of_horn_constraint._conclusion->_classification == false) &&
        (copy_of_horn_constraint._satisfiable == false)) {
      worklist.push(copy_of_horn_constraint._conclusion);
    }
    if (copy_of_horn_constraint._satisfiable == false) {
      _copy_of_horn_constraints.push_back(copy_of_horn_constraint);
      if (horn_constraints.at(i)._conclusion == NULL) {
        _false_datapoint._list_of_horn_constraints.insert(
            &_copy_of_horn_constraints.at(_copy_of_horn_constraints.size() -
                                          1));
      }
    }
  }
}

template <class T>
void horn_solver<T>::create_log_file(
    const std::vector<datapoint<T> *> &datapoints,
    const std::vector<horn_constraint<T>> &horn_constraints,
    std::unordered_set<datapoint<T> *> &positive,
    std::unordered_set<datapoint<T> *> &negative) const {
  gettimeofday(&end_time, NULL);
  std::ofstream outfile;
  outfile.open("horn_solver.log", std::ios_base::app);
  outfile << '\n' << "The given horn constraints are:" << '\n' << '\n';
  for (unsigned i = 0; i < horn_constraints.size(); i++) {
    std::stringstream ss_horn_constriant;
    ss_horn_constriant << "(";
    for (unsigned j = 0; j < horn_constraints.at(i)._premises.size(); j++) {
      ss_horn_constriant << horn_constraints.at(i)._premises.at(j)->_identifier;
      ss_horn_constriant << ",";
    }
    if (horn_constraints.at(i)._conclusion != NULL) {
      ss_horn_constriant << horn_constraints.at(i)._conclusion->_identifier;
    } else {
      ss_horn_constriant << "NULL";
    }
    ss_horn_constriant << ")";
    outfile << ss_horn_constriant.str() << '\n';
  }
  outfile << '\n'
          << "The initial classification of data points is:" << '\n'
          << '\n';
  std::string datapoint_classification;
  for (unsigned i = 0; i < _copy_of_datapoints.size(); i++) {
    if (_copy_of_datapoints.at(i)._is_classified) {
      datapoint_classification =
          _copy_of_datapoints.at(i)._classification ? "true)" : "false)";
      outfile << "(" << _copy_of_datapoints.at(i)._identifier << ","
              << datapoint_classification << '\n';
    }
  }
  outfile << '\n'
          << "The initial positive classification of data points is:" << '\n'
          << '\n';
  for (unsigned i = 0; i < _copy_of_positive.size(); i++) {
    outfile << "(" << _copy_of_positive.at(i)._identifier << "," << "true)"
            << '\n';
  }
  outfile << '\n'
          << "The initial negative classification of data points is:" << '\n'
          << '\n';
  for (unsigned i = 0; i < _copy_of_negative.size(); i++) {
    outfile << "(" << _copy_of_negative.at(i)._identifier << "," << "false)"
            << '\n';
  }

  if (horn_constraints_satisfiability) {
    outfile << '\n'
            << "The given horn constraints are SATISFIABLE." << '\n'
            << '\n';
  } else {
    outfile << '\n'
            << "The given horn constraints are UNSATISFIABLE." << '\n'
            << '\n';
  }
  outfile << "The final horn constraints are:" << '\n' << '\n';
  for (unsigned i = 0; i < _copy_of_horn_constraints.size(); i++) {
    std::stringstream ss_horn_constriant;
    ss_horn_constriant << "(";
    for (unsigned j = 0; j < _copy_of_horn_constraints.at(i)._premises.size();
         j++) {
      ss_horn_constriant
          << _copy_of_horn_constraints.at(i)._premises.at(j)->_identifier;
      ss_horn_constriant << ",";
    }
    ss_horn_constriant
        << _copy_of_horn_constraints.at(i)._conclusion->_identifier;
    ss_horn_constriant << ")";
    outfile << ss_horn_constriant.str() << '\n';
  }
  outfile << '\n'
          << "The final positive classification of data points is:" << '\n'
          << '\n';
  for (auto positive_itrator = positive.begin();
       positive_itrator != positive.end(); positive_itrator++) {
    datapoint_classification =
        (*positive_itrator)->_classification ? "true)" : "true)";
    outfile << "(" << (*positive_itrator)->_identifier << ","
            << datapoint_classification << '\n';
  }
  outfile << '\n'
          << "The final negative classification of data points is:" << '\n'
          << '\n';
  for (auto negative_itrator = negative.begin();
       negative_itrator != negative.end(); negative_itrator++) {
    datapoint_classification =
        (*negative_itrator)->_classification ? "true)" : "false)";
    outfile << "(" << (*negative_itrator)->_identifier << ","
            << datapoint_classification << '\n';
  }
  outfile << '\n'
          << "Time taken in microseconds:  "
          << end_time.tv_usec - start_time.tv_usec << '\n';
  outfile.close();
}

template <class T>
bool horn_solver<T>::solve(
    const std::vector<datapoint<T> *> &datapoints,
    const std::vector<horn_constraint<T>> &horn_constraints,
    std::unordered_set<datapoint<T> *> &positive,
    std::unordered_set<datapoint<T> *> &negative) {
  // gettimeofday(&start_time, NULL);
  clock_t begin_clock = clock();
  invocation_count++;
  _compute_current_marking = true;
  horn_constraints_satisfiability = true;
  _horn_constraints_without_conclusion = false;
  std::stack<datapoint<T> *> empty;
  std::swap(worklist, empty);

  keep_backup(datapoints, horn_constraints, positive, negative);

  if (horn_constraints_satisfiability == false) {
#ifdef DEBUG
    create_log_file(datapoints, horn_constraints, positive, negative);
#endif
    roll_back_datapoints(datapoints);
    _compute_current_marking = true;
    _horn_constraints_without_conclusion = false;
    _copy_of_positive.clear();
    _copy_of_negative.clear();
    _copy_of_datapoints.clear();
    _copy_of_horn_constraints.clear();
    _false_datapoint._list_of_marking.clear();
    _false_datapoint._list_of_horn_constraints.clear();
    /*gettimeofday(&end_time, NULL);
    elapsed_time.tv_sec = elapsed_time.tv_sec + end_time.tv_sec -
    start_time.tv_sec; elapsed_time.tv_usec = elapsed_time.tv_usec +
    end_time.tv_usec - start_time.tv_usec;*/
    clock_t end_clock = clock();
    elapsed_time = elapsed_time + end_clock - begin_clock;
    return false;
  }
  populate_meta_data(datapoints, _copy_of_horn_constraints);
  fill_worklist(datapoints, positive, negative);
  horn_constraints_satisfiability =
      verify_constraints_satisfiability(positive, negative);
#ifdef DEBUG
  create_log_file(datapoints, horn_constraints, positive, negative);
#endif
  roll_back_datapoints(datapoints);
  _compute_current_marking = true;
  _horn_constraints_without_conclusion = false;
  _copy_of_positive.clear();
  _copy_of_negative.clear();
  _copy_of_datapoints.clear();
  _copy_of_horn_constraints.clear();
  _false_datapoint._list_of_marking.clear();
  _false_datapoint._list_of_horn_constraints.clear();
  /*gettimeofday(&end_time, NULL);
  elapsed_time.tv_sec = elapsed_time.tv_sec + end_time.tv_sec -
  start_time.tv_sec; elapsed_time.tv_usec = elapsed_time.tv_usec +
  end_time.tv_usec - start_time.tv_usec;*/
  clock_t end_clock = clock();
  elapsed_time = elapsed_time + end_clock - begin_clock;
  if (horn_constraints_satisfiability) {
    return true;
  }
  return false;
}

template <class T>
bool horn_solver<T>::verify_constraints_satisfiability(
    std::unordered_set<datapoint<T> *> &positive,
    std::unordered_set<datapoint<T> *> &negative) const {
  while (!worklist.empty()) {
    datapoint<T> *current_variable_addr = worklist.top();
    worklist.pop();
    if (!current_variable_addr->_list_of_horn_constraints.empty()) {
      current_variable_addr->remove_satisfied_horn_clauses();
      if (current_variable_addr->_classification ==
          true) { /* propagate truth _classification */
        for (auto horn_constraint_itrator =
                 current_variable_addr->_list_of_horn_constraints.begin();
             horn_constraint_itrator !=
             current_variable_addr->_list_of_horn_constraints.end();
             ++horn_constraint_itrator) {
          if ((*horn_constraint_itrator)->_satisfiable == false) {
            if (propagate_true_classification((*horn_constraint_itrator),
                                              current_variable_addr,
                                              positive) == false) {
              return false;
            }
          }
        }
      } else { /* propagation of false _classification */
        for (auto horn_constraint_itrator =
                 current_variable_addr->_list_of_horn_constraints.begin();
             horn_constraint_itrator !=
             current_variable_addr->_list_of_horn_constraints.end();
             ++horn_constraint_itrator) {
          if ((*horn_constraint_itrator)->_satisfiable == false) {
            if (propagate_false_classification((*horn_constraint_itrator),
                                               current_variable_addr,
                                               negative) == false) {
              return false;
            }
          }
        }
      }
    }
  }
  return true;
}
template class horn_solver<bool>;
}; // namespace horn_verification
