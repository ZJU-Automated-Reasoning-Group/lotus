/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// C++ includes
#include <fstream>
#include <utility>

// Project includes
#include "boogie_io.h"
#include "error.h"

#include <iostream>

namespace horn_verification {

unsigned int boogie_io::read_status_file(const std::string &filename) {

  // Open file for read operations
  std::ifstream infile(filename);

  // Check opening the file failed
  if (infile.fail()) {
    throw boogie_io_error("Error opening " + filename);
  }

  // Read one line from file
  std::string line;
  if (!std::getline(infile, line)) {
    throw boogie_io_error("Invalid file " + filename);
  }

  // Convert line into int
  unsigned int ret;
  std::stringstream ss(line);
  ss >> ret;

  if (ss.fail() || !ss.eof()) {
    throw boogie_io_error("Unable to parse number invocations in line 1 of " +
                          filename);
  }

  return ret;
}

attributes_metadata
boogie_io::read_attributes_file(const std::string &filename) {

  // Open file for read operations
  std::ifstream infile(filename);

  // Check opening the file failed
  if (infile.fail()) {
    throw boogie_io_error("Error opening " + filename);
  }

  // Variables
  attributes_metadata metadata;
  unsigned int cur_attribute_type = 0; // 0 = categorical; 1 = integer

  //
  // Read file line by line
  //
  std::string line;
  unsigned int line_nr = 0;
  while (std::getline(infile, line)) {

    ++line_nr;

    // Skip empty lines and comments
    if (line.empty() || line.compare(0, 1, "#") == 0) {
      continue;
    }

    // Split line at blanks, remove duplicate blanks
    auto split_line = split(line, ',');

    //
    // Categorical attribute
    //
    if (split_line[0] == "cat") {

      // Check correct format or categorical attribute
      if (split_line.size() != 3) {
        throw boogie_io_error(
            "Invalid definition of categorical attribute in line " +
            std::to_string(line_nr) + " of " + filename);
      }

      // Check correct order of attributes
      if (cur_attribute_type > 0) {
        throw boogie_io_error("Unexpected categorical attribute in line " +
                              std::to_string(line_nr) + " of " + filename);
      }

      // Parse number of categories
      std::stringstream ss(split_line[2]);
      unsigned int number_of_categories;
      ss >> number_of_categories;
      if (ss.fail() || !ss.eof()) {
        throw boogie_io_error("Unable to parse number of categories in line " +
                              std::to_string(line_nr) + " of " + filename);
      } else if (number_of_categories == 0) {
        throw boogie_io_error("Number of categories must not be 0 in line " +
                              std::to_string(line_nr) + " of " + filename);
      }

      // Add categorical attribute
      trim(split_line[1]);
      metadata.add_categorical_attribute(split_line[1], number_of_categories);

    }

    //
    // Integer attribute
    //
    else if (split_line[0] == "int") {

      // Check correct format or integer attribute
      if (split_line.size() != 2) {
        throw boogie_io_error(
            "Invalid definition of integer attribute in line " +
            std::to_string(line_nr) + " of " + filename);
      }

      // Check correct order of attributes
      if (cur_attribute_type < 1) {
        cur_attribute_type = 1;
      }

      // Add integer attribute
      trim(split_line[1]);
      metadata.add_int_attribute(split_line[1]);

    }

    //
    // Invalid attribute type
    //
    else {
      throw boogie_io_error("Unknown attribute type in line " +
                            std::to_string(line_nr) + " of " + filename);
    }
  }

  // Close input file
  infile.close();

  return metadata;
}

std::vector<datapoint<bool>>
boogie_io::read_data_file(const std::string &filename,
                          const attributes_metadata &metadata) {

  // Open file for read operations
  std::ifstream infile(filename);

  // Check opening the file failed
  if (infile.fail()) {
    throw boogie_io_error("Error opening " + filename);
  }

  // Some variables
  std::vector<datapoint<bool>> datapoints;
  unsigned number_of_datapoints = 0;

  //
  // Read file line by line
  //
  std::string line;
  unsigned int line_nr = 0;
  while (std::getline(infile, line)) {

    ++line_nr;

    // Skip empty lines and comments
    if (line.empty() || line.compare(0, 1, "#") == 0) {
      continue;
    }

    // Split line
    auto split_line = split(line, ',');

    // Check format of line
    if (split_line.size() !=
        metadata.categorical_names().size() + metadata.int_names().size() + 1) {
      throw boogie_io_error("Invalid format of datapoint in line " +
                            std::to_string(line_nr) + " of " + filename);
    }

    //
    // Get classification
    //
    bool is_labeled;
    bool label;
    const auto &last = split_line.back();

    if (last == "true") {
      is_labeled = true;
      label = true;
    } else if (last == "false") {
      is_labeled = true;
      label = false;
    } else if (last == "?") {
      is_labeled = false;
      label = false; // Unimportant
    } else {
      throw boogie_io_error("Invalid classification of datapoint in line " +
                            std::to_string(line_nr) + " of " + filename);
    }

    //
    // Parse datapoint
    //
    datapoint<bool> dp(label, is_labeled);
    std::stringstream ss;
    auto it = split_line.begin();

    // Categorical arguments
    dp._categorical_data.reserve(metadata.categorical_names().size());
    for (std::size_t i = 0; i < metadata.categorical_names().size(); ++i) {

      // Parse categorical data
      ss.str(*it);
      ss.clear();
      unsigned int value;
      ss >> value;

      if (ss.fail() || !ss.eof()) {
        throw boogie_io_error("Unable to parse categorical data in line " +
                              std::to_string(line_nr) + " of " + filename);
      } else if (value >= metadata.number_of_categories()[i]) {
        throw boogie_io_error("Invalid category value in line " +
                              std::to_string(line_nr) + " of " + filename);
      }

      // Add data
      dp._categorical_data.push_back(value);

      // Advance iterator
      ++it;
    }

    // Integer attributes
    dp._int_data.reserve(metadata.int_names().size());
    for (std::size_t i = 0; i < metadata.int_names().size(); ++i) {

      // Parse categorical data
      ss.str(*it);
      ss.clear();
      int value;
      ss >> value;

      if (ss.fail() || !ss.eof()) {
        throw boogie_io_error("Unable to parse integer data in line " +
                              std::to_string(line_nr) + " of " + filename);
      }

      // Add data
      dp._int_data.push_back(value);

      // Advance iterator
      ++it;
    }

    // Check data point
    if (dp._categorical_data.size() != metadata.categorical_names().size()) {
      throw boogie_io_error(
          "Data point has incorrect number of categorical attributes in line " +
          std::to_string(line_nr) + " of " + filename);
    }
    if (dp._int_data.size() != metadata.int_names().size()) {
      throw boogie_io_error(
          "Data point has incorrect number of integer attributes in line " +
          std::to_string(line_nr) + " of " + filename);
    }

    // Add data point
    dp._identifier = number_of_datapoints++;
    datapoints.push_back(std::move(dp)); // No use of dp after this point
  }

  // Close input file
  infile.close();

  return datapoints;
}

std::vector<horn_constraint<bool>> boogie_io::indexes2horn_constraints(
    const std::vector<std::pair<std::set<unsigned>, std::set<unsigned>>>
        &horn_constraints_as_indexes,
    std::vector<datapoint<bool>> &datapoints) {

  // Create Horn constraint objects
  std::vector<horn_constraint<bool>> horn_constraints;
  horn_constraints.reserve(horn_constraints_as_indexes.size());

  unsigned i = 0;
  for (const auto &horn_pair : horn_constraints_as_indexes) {

    ++i;

    // Create premises
    std::vector<datapoint<bool> *> premises;
    premises.reserve(horn_pair.first.size());

    for (const auto index : horn_pair.first) {

      if (index >= datapoints.size()) {
        throw boogie_io_error("Horn constraint " + std::to_string(i) +
                              " out of bounds");
      }

      premises.push_back(&datapoints[index]);
    }

    // Create consequence
    datapoint<bool> *consequent = nullptr;
    if (!horn_pair.second.empty()) {

      auto index = *horn_pair.second.begin();

      if (index >= datapoints.size()) {
        throw boogie_io_error("Horn constraint " + std::to_string(i) +
                              " out of bounds");
      }

      consequent = &datapoints[index];
    }

    // Add constraint
    horn_constraints.push_back(horn_constraint<bool>(premises, consequent));
  }

  return horn_constraints;
}

std::vector<horn_constraint<bool>>
boogie_io::read_horn_file(const std::string &filename,
                          std::vector<datapoint<bool>> &datapoints) {
  return indexes2horn_constraints(read_horn_file(filename), datapoints);
}

std::vector<std::pair<std::set<unsigned>, std::set<unsigned>>>
boogie_io::read_horn_file(const std::string &filename) {

  // Define symbol for empty head
  auto empty_head = "_";

  // Open file for read operations
  std::ifstream infile(filename);

  // Check opening the file failed
  if (infile.fail()) {
    throw boogie_io_error("Error opening " + filename);
  }

  // Some variables
  std::vector<std::pair<std::set<unsigned>, std::set<unsigned>>>
      horn_constraints; // Horn constraints as indexes (as occurring in the data
                        // file)

  //
  // Read file line by line
  //
  std::string line;
  unsigned int line_nr = 0;
  while (std::getline(infile, line)) {

    ++line_nr;

    // Skip empty lines and comments
    if (line.empty() || line.compare(0, 1, "#") == 0) {
      continue;
    }

    // Split line
    auto split_line = split(line, ',');

    // Check format of line
    if (split_line.size() < 2) {
      throw boogie_io_error("Invalid format of horn constraint in line " +
                            std::to_string(line_nr) + " of " + filename);
    }

    // Parse premises
    std::set<unsigned> premises;
    std::stringstream ss;
    for (std::size_t i = 0; i < split_line.size() - 1; ++i) {

      ss.str(split_line[i]);
      ss.clear();
      std::size_t index;
      ss >> index;

      if (ss.fail() || !ss.eof()) {
        throw boogie_io_error("Unable to parse horn constraint in line " +
                              std::to_string(line_nr) + " of " + filename);
      }

      // std::cout << index << " ";

      premises.insert(index);
    }

    // Parse consequence
    // std::cout << " => ";
    std::set<unsigned> consequence;
    if (split_line.back() != empty_head) {

      ss.str(split_line.back());
      ss.clear();
      std::size_t index;
      ss >> index;

      if (ss.fail() || !ss.eof()) {
        throw boogie_io_error("Unable to parse horn constraint in line " +
                              std::to_string(line_nr) + " of " + filename);
      }

      // std::cout << index;

      consequence.insert(index);
    }

    horn_constraints.push_back(std::make_pair(premises, consequence));
    // std::cout << std::endl;
  }

  // Close input file
  infile.close();

  return horn_constraints;
}

void boogie_io::write_json_file(const std::string &filename,
                                decision_tree &tree,
                                const attributes_metadata &metadata) {

  // Open file
  std::ofstream outfile;
  outfile.open(filename, std::ofstream::out);

  // Check opening the file failed
  if (outfile.fail()) {
    throw boogie_io_error("Error opening " + filename);
  }

  // Create visitor and write output
  // json_visitor visitor(metadata, outfile);
  // tree.accept(visitor);

  // Close file
  outfile.close();
}

std::vector<horn_constraint<bool>> boogie_io::get_indistinguishable_datapoints(
    std::vector<datapoint<bool>> &datapoints, unsigned int threshold) {

  std::vector<horn_constraint<bool>> horn_constraints;
  auto dpts_size = datapoints.size();

  for (unsigned int i = 0; i < dpts_size; i++) {
    for (unsigned int j = i + 1; j < dpts_size; j++) {
      if (!datapoints[i].is_distinguishable(datapoints[j], threshold)) {
        // std::cout << "Cannot distinguish points: " << i << " and " << j <<
        // std::endl;

        std::vector<datapoint<bool> *> premises1;
        datapoint<bool> *consequence1 = nullptr;

        premises1.push_back(&datapoints[i]);
        consequence1 = &datapoints[j];
        horn_constraints.push_back(
            horn_constraint<bool>(premises1, consequence1));

        std::vector<datapoint<bool> *> premises2;
        datapoint<bool> *consequence2 = nullptr;

        premises2.push_back(&datapoints[j]);
        consequence2 = &datapoints[i];
        horn_constraints.push_back(
            horn_constraint<bool>(premises2, consequence2));
      }
    }
  }
  return horn_constraints;
}

std::vector<std::pair<unsigned, unsigned>>
boogie_io::read_intervals_file(const std::string &filename) {

  std::vector<std::pair<unsigned, unsigned>> intervals;

  // Open file for read operations
  std::ifstream infile(filename);

  // Check opening the file failed
  if (infile.fail()) {
    throw boogie_io_error("Error opening " + filename);
  }

  // Read one line from file
  std::string line;
  unsigned int line_nr = 0;
  while (std::getline(infile, line)) {

    ++line_nr;

    // Skip empty lines and comments
    if (line.empty() || line.compare(0, 1, "#") == 0) {
      continue;
    }

    // Split line at comma
    auto split_line = boogie_io::split(line, ',');

    if (split_line.size() != 2) {
      throw boogie_io_error("Unable to parse interval in line " +
                            std::to_string(line_nr) + " of " + filename);
    }

    // Convert left interval bound
    unsigned left;
    std::stringstream ss1(split_line[0]);
    ss1 >> left;
    if (ss1.fail() || !ss1.eof()) {
      throw boogie_io_error("Unable to parse left interval bound in line " +
                            std::to_string(line_nr) + " of " + filename);
    }

    // Convert right interval bound
    unsigned right;
    std::stringstream ss2(split_line[1]);
    ss2 >> right;
    if (ss2.fail() || !ss2.eof()) {
      throw boogie_io_error("Unable to parse right interval bound in line " +
                            std::to_string(line_nr) + " of " + filename);
    }

    intervals.push_back(std::pair<unsigned, unsigned>(left, right));
  }

  return intervals;
}

}; // End namespace horn_verification
