/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// C++ includes
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

// C includes
#include <getopt.h>

// Project includes
#include "boogie_io.h"
#include "learner.h"

using namespace horn_verification;

int read_bounds_file(const std::string &filename) {

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
  int ret;
  std::stringstream ss(line);
  ss >> ret;

  if (ss.fail() || !ss.eof()) {
    throw boogie_io_error("Unable to parse number bounds in line 1 of " +
                          filename);
  }

  return ret;
}

/**
 * Writes bounds to a file.
 *
 * @param filename The file to write to
 * @param bounds The bounds to write
 */
void write_bounds_file(const std::string &filename, int bounds) {

  // Open file
  std::ofstream outfile;
  outfile.open(filename, std::ofstream::out);

  // Check opening the file failed
  if (outfile.fail()) {
    throw boogie_io_error("Error opening " + filename);
  }

  // Write output
  outfile << bounds;

  // Close file
  outfile.close();
}

/**
 * This class represents bounds on the learner and a strategy how to increase
 * the bounds if no consistent decision tree with respect to the current bounds
 * exists.
 *
 * @author Daniel Neider
 *
 * @version 1.0
 */
class bound {

  /// The current bound
  int _bound;

  /// Indicates whether bounds should be used at all
  bool _use_bound;

public:
  /**
   * Creates a new object with bound 1.
   */
  bound() : _bound(1), _use_bound(true) {
    // Nothing
  }

  /**
   * Creates a new object with the given initial bound.
   *
   * @param bound The initial bound
   */
  bound(int bound) : _bound(bound), _use_bound(true) {
    // Nothing
  }

  /**
   * Returns whether bounds should be used at all.
   *
   * @return whether bounds should be used at all
   */
  bool use_bound() const { return _use_bound; }

  /**
   * Returns the bound.
   *
   * @return the bound
   */
  int get_bound() const { return _bound; }

  /**
   * Increases the bounds. Re-implement this method to make it fit your needs!
   */
  void increase() {

    // Double bound
    _bound *= 2;

    // If bound is larger than some arbitrary value, do no longer use bounds
    if (_bound > 1000) {
      _use_bound = false;
    }
  }

  /**
   * Write a textual representation of this object to the given output stream.
   *
   * @param out The output stream to write to
   * @param b The bound
   *
   * @return the given output stream
   */
  friend std::ostream &operator<<(std::ostream &out, const bound &b) {
    out << "[bound=" << b._bound << "; use bound=" << b._use_bound << "]";
    return out;
  }
};

void display_usage(const std::string &program_name) {
  std::cout << "Usage: " << program_name << " [options] file_stem\n";
  std::cout << "Runs the decision tree learner on the files specidifed by the "
               "file stem. If no"
            << '\n';
  std::cout << "options are given, uses bfs as node selection method and "
               "default as the"
            << '\n';
  std::cout << "entropy method.\n";

  std::cout << "Options are:\n";

  std::cout << "  -c\t\t\t\tBiases the learner to prefer splits that"
            << '\n';
  std::cout << "\t\t\t\tproduce conjunctions.\n";

  std::cout << "  -n <node selection method> \tSelects the method used to "
               "determine in which"
            << '\n';
  std::cout << "\t\t\t\torder the tree is constructed. Valid options are"
            << '\n';
  std::cout << "\t\t\t\tbfs, dfs, random, max_entropy,\n";
  std::cout << "\t\t\t\tmax_weighted_entropy, min_entropy,\n";
  std::cout << "\t\t\t\tand min_weighted_entropy.\n";

  std::cout << "  -e <entropy method> \t\tSelects the method used to compute "
               "the score"
            << '\n';
  std::cout << "\t\t\t\t(or entropy) of a node when splitting. Valid"
            << '\n';
  std::cout << "\t\t\t\toptions are default, penalty, and horn.\n";

  std::cout << "  -h -? \t\t\tDisplays this help message.\n";
}

/**
 * Main program.
 *
 * @param argc The number of command line arguments
 * @param argv The ocmmand line arguments
 *
 * @return The return code of the program
 */
int main(int argc, char *argv[]) {

  //
  // Parse command line options
  //

  // Prepare variables
  NodeSelection node_selection = NodeSelection::BFS; // Standard
  EntropyComputation entropy_computation =
      EntropyComputation::DEFAULT_ENTROPY; // Standard
  ConjunctiveSetting prefer_conjunctions =
      ConjunctiveSetting::NOPREFERENCEFORCONJUNCTS;

  // Parse command line options
  int opt = 0;
  while ((opt = getopt(argc, argv, "ce:hn:?")) != -1) {

    switch (opt) {

    // Split preference
    case 'c': {

      prefer_conjunctions = ConjunctiveSetting::PREFERENCEFORCONJUNCTS;
      break;
    }

    // Node selection method
    case 'n': {

      auto arg = std::string(optarg);

      if (arg == "bfs") {
        node_selection = NodeSelection::BFS;
      } else if (arg == "dfs") {
        node_selection = NodeSelection::DFS;
      } else if (arg == "random") {
        node_selection = NodeSelection::RANDOM;
      } else if (arg == "max_entropy") {
        node_selection = NodeSelection::MAX_ENTROPY;
      } else if (arg == "max_weighted_entropy") {
        node_selection = NodeSelection::MAX_WEIGHTED_ENTROPY;
      } else if (arg == "min_entropy") {
        node_selection = NodeSelection::MIN_ENTROPY;
      } else if (arg == "min_weighted_entropy") {
        node_selection = NodeSelection::MIN_WEIGHTED_ENTROPY;
      } else {
        std::cout << "Invalid option " << arg << '\n';
        return EXIT_FAILURE;
      }

      break;
    }

    // Entropy method
    case 'e': {

      auto arg = std::string(optarg);

      if (arg == "default") {
        entropy_computation = EntropyComputation::DEFAULT_ENTROPY;
      } else if (arg == "penalty") {
        entropy_computation = EntropyComputation::PENALTY;
      } else if (arg == "horn") {
        entropy_computation = EntropyComputation::HORN_ASSIGNMENTS;
      } else {
        std::cout << "Invalid option " << arg << '\n';
        return EXIT_FAILURE;
      }

      break;
    }

    case 'h':
    case '?': {

      display_usage(std::string(argv[0]));
      return EXIT_SUCCESS;
    }
    }
  }

  // Get file stem
  if (optind != argc - 1) {
    std::cout << "Invalid file stem given\n";
    return EXIT_FAILURE;
  }
  auto file_stem = std::string(argv[optind]);

  //
  // DEBUG (write options to file)
  //
  if (true) {

    std::ofstream opt_file(file_stem + ".options");
    opt_file << "node=" << node_selection << '\n';
    opt_file << "entropy=" << entropy_computation << '\n';
    opt_file << "conjunctive" << prefer_conjunctions << '\n';
    opt_file.close();
  }

  //
  // Run the learner and catch all exceptions except for those those related to
  // bounded constants
  //
  try {

    //
    // Read initial status (i.e., number of learner invocations) from file
    //
    int round = boogie_io::read_status_file(file_stem + ".status");

    //
    // Read current bound from file if initial round
    //
    bound current_bound;

    if (round > 1) {

      auto b = read_bounds_file(file_stem + ".bound");
      current_bound = bound(b);
    }

    //
    // Try to learn trees with increasing bounds
    //
    unsigned iteration = 0; // Current iteration
    bool finished = false;
    do {

      ++iteration;
      std::cerr << current_bound << '\n';

      //
      // Read input from files
      //

      // Read attribute meta data
      auto metadata =
          boogie_io::read_attributes_file(file_stem + ".attributes");

      // Read data points
      auto datapoints =
          boogie_io::read_data_file(file_stem + ".data", metadata);

      // Read horn constraints
      auto horn_constraints =
          boogie_io::read_horn_file(file_stem + ".horn", datapoints);

      //
      // Check input
      //
      if (metadata.int_names().size() + metadata.categorical_names().size() ==
          0) {
        throw boogie_io_error("No attributes defined");
      }

      //
      // If attributes are subject to bounding, create and add constraints
      //
      if (current_bound.use_bound()) {
        // Infer horn constraints which arise due to indistinguishability of
        // data points
        auto indistinguishability_horn_constraints =
            boogie_io::get_indistinguishable_datapoints(
                datapoints, current_bound.get_bound());

        // Push these indistinguishability_horn_constraints to horn_constraints
        horn_constraints.insert(
            horn_constraints.end(),
            std::make_move_iterator(
                indistinguishability_horn_constraints.begin()),
            std::make_move_iterator(
                indistinguishability_horn_constraints.end()));

        // std::cout << "number of horn constraints after equality constraints
        // are added are " << horn_constraints.size() << std::endl;
      }

      //
      // Create pointer to datapoints
      //
      std::vector<datapoint<bool> *> datapoint_ptrs;
      datapoint_ptrs.reserve(datapoints.size());
      for (auto &dp : datapoints) {
        datapoint_ptrs.push_back(&dp);
      }

      //
      // Try learning with current bound
      //
      try {

        //
        // Instantiate Horn solver and perform initial run to label data points
        // if necessary
        //
        horn_solver<bool> solver;
        std::unordered_set<datapoint<bool> *> positive_ptrs;
        std::unordered_set<datapoint<bool> *> negative_ptrs;

        // Initial run
        auto ok = solver.solve(datapoint_ptrs, horn_constraints, positive_ptrs,
                               negative_ptrs);

        if (ok) {

          for (auto &dp : positive_ptrs) {
            dp->_is_classified = true;
            dp->_classification = true;
          }

          for (auto &dp : negative_ptrs) {
            dp->_is_classified = true;
            dp->_classification = false;
          }

        }

        else {
          throw sample_error("No consistent decision tree exists (Horn clauses "
                             "are contradictory)");
        }

        //
        // Run decisionn tree learner
        //
        learner<complex_job_manager> l(
            current_bound.use_bound()
                ? complex_job_manager(datapoint_ptrs, horn_constraints, solver,
                                      current_bound.get_bound(), node_selection,
                                      entropy_computation, prefer_conjunctions)
                : complex_job_manager(datapoint_ptrs, horn_constraints, solver,
                                      node_selection, entropy_computation,
                                      prefer_conjunctions));
        auto decision_tree =
            l.learn(metadata, datapoint_ptrs, horn_constraints);

        //
        // Debug
        //
        auto consistent =
            l.is_consistent(decision_tree, datapoint_ptrs, horn_constraints);
        std::cout << "Is consistent? " << consistent << '\n';
        assert(consistent);

        //
        // Output
        //
        boogie_io::write_json_file(file_stem + ".json", decision_tree,
                                   metadata);
        finished = true;

      }

      //
      // Sample becomes inconsistent with the viven bounds
      //
      catch (const sample_error &err) {

        std::cerr << err.what() << '\n';

        // Increase bounds
        current_bound.increase();

      }

      //
      // No split possible
      //
      catch (const split_not_possible_error &err) {

        std::cerr << err.what() << '\n';

        // Increase bounds
        current_bound.increase();
      }

    } while (!finished);

    //
    // Write new bounds to file
    //
    write_bounds_file(file_stem + ".bound", current_bound.get_bound());

    return EXIT_SUCCESS;

  }

  //
  // Handle exceptions (basically exit gracefully)
  //
  catch (const std::exception &ex) {
    std::cerr << ex.what() << '\n';
    return EXIT_FAILURE;
  } catch (...) {
    std::cerr << "The learner crahsed due to an unknown reason\n";
    return EXIT_FAILURE;
  }
}
