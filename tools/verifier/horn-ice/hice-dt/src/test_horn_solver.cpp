/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "horn_solver.h"

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unordered_set>
using namespace std;
using namespace horn_verification;

datapoint<bool> *
get_by_identifier(std::vector<datapoint<bool> *> &vector_objects,
                  unsigned identifier) {
  unsigned i;
  for (i = 0; i < vector_objects.size(); i++) {
    if (vector_objects.at(i)->_identifier == identifier) {
      break;
    }
  }
  return vector_objects.at(i);
}

void insert_if_unique(std::vector<datapoint<bool> *> &vector_objects,
                      datapoint<bool> *object) {
  bool unique_marking = true;
  for (unsigned i = 0; i < vector_objects.size(); i++) {
    if (vector_objects.at(i) == object) {
      unique_marking = false;
      break;
    }
  }
  if (unique_marking) {
    vector_objects.push_back(object);
  }
}

datapoint<bool> *
insert_if_unique(std::vector<datapoint<bool>> &vector_of_temp_datapoints,
                 std::vector<datapoint<bool> *> &vector_objects,
                 unsigned identifier) {
  bool unique_identifier = true;
  for (unsigned i = 0; i < vector_objects.size(); i++) {
    if (vector_objects.at(i)->_identifier == identifier) {
      unique_identifier = false;
      return vector_objects.at(i);
      break;
    }
  }
  if (unique_identifier) {
    datapoint<bool> data_point_object(identifier);
    vector_of_temp_datapoints.push_back(data_point_object);
    vector_objects.push_back(
        &vector_of_temp_datapoints.at(vector_of_temp_datapoints.size() - 1));
  }
  return &vector_of_temp_datapoints.at(vector_of_temp_datapoints.size() - 1);
}

int main(int argc, char *argv[]) {
  std::string line;
  std::string substring;
  std::string comma = ",";
  std::string closing_bracket = ")";
  bool start_of_P_and_N = false;
  const unsigned Max_size_of_datapoints = 2000;
  std::size_t start_postion, second_postion, lenght;

  std::unordered_set<datapoint<bool> *> positive;
  std::unordered_set<datapoint<bool> *> negative;
  std::vector<datapoint<bool> *> vector_of_datapoints;
  std::vector<datapoint<bool>> vector_of_temp_datapoints;
  vector_of_temp_datapoints.reserve(Max_size_of_datapoints);

  std::vector<horn_constraint<bool>> vector_of_horn_constraints;

  horn_solver<bool> horn_solver_object;
  std::ifstream infile;
  if (argc < 2) {
    infile.open("sample_input.txt");
  } else {
    infile.open(argv[1]);
  }
  while (infile.good()) {
    infile >> line;
    lenght = line.size();
    start_postion = line.find(comma);
    if (start_postion != std::string::npos) {
      std::vector<datapoint<bool> *> left;
      unsigned datapoint_identifier =
          atoi(line.substr(1, start_postion - 1).c_str());
      // std::cout << "The identified is:  " << line.substr(1,
      // start_postion-1).c_str() << std::endl;
      left.push_back(insert_if_unique(vector_of_temp_datapoints,
                                      vector_of_datapoints,
                                      datapoint_identifier));
      second_postion = line.find(comma, start_postion + 1);
      while (second_postion < lenght) {
        left.push_back(insert_if_unique(
            vector_of_temp_datapoints, vector_of_datapoints,
            atoi(line.substr(start_postion + 1,
                             second_postion - start_postion - 1)
                     .c_str())));
        // std::cout << "The identified is:  " << line.substr(start_postion + 1,
        // second_postion - start_postion - 1).c_str() << std::endl;
        start_postion = second_postion; // create next data point
        second_postion = line.find(comma, start_postion + 1);
      }
      second_postion = line.find(closing_bracket, start_postion + 1);
      substring =
          line.substr(start_postion + 1, second_postion - start_postion - 1);
      // std::cout << "The identified is:  " << line.substr(start_postion + 1,
      // second_postion - start_postion - 1).c_str() << std::endl;
      if (substring.compare("true") && substring.compare("false") &&
          substring.compare("NULL") && substring.compare("Unsigned")) {
        horn_constraint<bool> horn_constraint_object(
            left,
            insert_if_unique(
                vector_of_temp_datapoints, vector_of_datapoints,
                atoi(line.substr(start_postion + 1,
                                 second_postion - start_postion - 1)
                         .c_str())),
            false);
        vector_of_horn_constraints.push_back(horn_constraint_object);
        start_of_P_and_N = true;
      }
      if (!substring.compare("NULL")) {
        horn_constraint<bool> horn_constraint_object(left, NULL, false);
        vector_of_horn_constraints.push_back(horn_constraint_object);
        start_of_P_and_N = true;
      }
      if (!substring.compare("true") && start_of_P_and_N) {
        positive.insert(
            get_by_identifier(vector_of_datapoints, datapoint_identifier));
      } else if (!substring.compare("true")) {
        get_by_identifier(vector_of_datapoints, datapoint_identifier)
            ->set_classification(true);
      }
      if (!substring.compare("false") && start_of_P_and_N) {
        negative.insert(
            get_by_identifier(vector_of_datapoints, datapoint_identifier));
      } else if (!substring.compare("false")) {
        get_by_identifier(vector_of_datapoints, datapoint_identifier)
            ->set_classification(false);
      }
    }
  }
  infile.close();
  horn_solver_object.solve(vector_of_datapoints, vector_of_horn_constraints,
                           positive, negative);
  for (auto positive_itrator = positive.begin();
       positive_itrator != positive.end(); positive_itrator++) {
    std::unordered_set<datapoint<bool> *> test_positive;
    std::unordered_set<datapoint<bool> *> test_negative;
    test_positive.insert((*positive_itrator));
    assert(horn_solver_object.solve(vector_of_datapoints,
                                    vector_of_horn_constraints, test_positive,
                                    test_negative));
  }

  for (auto negative_itrator = negative.begin();
       negative_itrator != negative.end(); negative_itrator++) {
    std::unordered_set<datapoint<bool> *> test_positive;
    std::unordered_set<datapoint<bool> *> test_negative;
    test_negative.insert((*negative_itrator));
    assert(horn_solver_object.solve(vector_of_datapoints,
                                    vector_of_horn_constraints, test_positive,
                                    test_negative));
  }

  for (auto positive_itrator = positive.begin();
       positive_itrator != positive.end(); positive_itrator++) {
    std::unordered_set<datapoint<bool> *> test_positive;
    std::unordered_set<datapoint<bool> *> test_negative;
    test_negative.insert((*positive_itrator));
    assert(!horn_solver_object.solve(vector_of_datapoints,
                                     vector_of_horn_constraints, test_positive,
                                     test_negative));
  }

  for (auto negative_itrator = negative.begin();
       negative_itrator != negative.end(); negative_itrator++) {
    std::unordered_set<datapoint<bool> *> test_positive;
    std::unordered_set<datapoint<bool> *> test_negative;
    test_positive.insert((*negative_itrator));
    assert(!horn_solver_object.solve(vector_of_datapoints,
                                     vector_of_horn_constraints, test_positive,
                                     test_negative));
  }

  for (auto datapoint_itrator = 0;
       datapoint_itrator < vector_of_datapoints.size(); datapoint_itrator++) {
    bool unsigned_datapoint = true;
    for (auto negative_itrator = negative.begin();
         negative_itrator != negative.end(); negative_itrator++) {
      if (vector_of_datapoints.at(datapoint_itrator) == (*negative_itrator)) {
        unsigned_datapoint = false;
      }
    }
    for (auto positive_itrator = positive.begin();
         positive_itrator != positive.end(); positive_itrator++) {
      if (vector_of_datapoints.at(datapoint_itrator) == (*positive_itrator)) {
        unsigned_datapoint = false;
      }
    }
    if (unsigned_datapoint) {
      std::unordered_set<datapoint<bool> *> test_positive;
      std::unordered_set<datapoint<bool> *> test_negative;
      test_positive.insert(vector_of_datapoints.at(datapoint_itrator));
      assert(horn_solver_object.solve(vector_of_datapoints,
                                      vector_of_horn_constraints, test_positive,
                                      test_negative));
    }
    if (unsigned_datapoint) {
      std::unordered_set<datapoint<bool> *> test_positive;
      std::unordered_set<datapoint<bool> *> test_negative;
      test_negative.insert(vector_of_datapoints.at(datapoint_itrator));
      assert(horn_solver_object.solve(vector_of_datapoints,
                                      vector_of_horn_constraints, test_positive,
                                      test_negative));
    }
  }

  for (auto positive_itrator = positive.begin();
       positive_itrator != positive.end(); positive_itrator++) {
    assert((*positive_itrator)->set_classification(true));
  }

  for (auto negative_itrator = negative.begin();
       negative_itrator != negative.end(); negative_itrator++) {
    assert((*negative_itrator)->set_classification(false));
  }

  std::cout << "Test completed and total elapsed time is:"
            << horn_solver_object.elapsed_time << '\n';

  return 0;
}
