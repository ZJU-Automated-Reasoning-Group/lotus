/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __API_HELPER_H__
#define __API_HELPER_H__

// C++ includes
#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

// C includes
#include <unistd.h>

// Project includes
#include "boogie_io.h"
#include "bound.h"
#include "debug_job_manager.h"
#include "horn_solver.h"
#include "horndini.h"
#include "learner.h"
#include "pretty_print_visitor.h" // DEBUG
#include "simple_job_manager.h"


namespace horn_verification
{

	using namespace horn_verification;

	class api_helper {


		private:

			attributes_metadata & metadata;
 			
			std::vector<datapoint<bool>> & datapoints;

 			std::vector<std::pair<std::set<unsigned>, std::set<unsigned>>> & horn_indexes;

	 		std::vector<std::pair<unsigned, unsigned>> & intervals;

			horn_verification::boogie_io boogie_io_object;

		public:

			api_helper (attributes_metadata & _metadata, std::vector<datapoint<bool>> & _datapoints, std::vector<std::pair<std::set<unsigned>, std::set<unsigned>>> & _horn_indexes, std::vector<std::pair<unsigned, unsigned>> & _intervals): metadata(_metadata), datapoints(_datapoints), horn_indexes(_horn_indexes), intervals(_intervals)
			{
				//Nothing to do
			}

/**
 * Implements a Houdini pre-phase in the following way:
 *
 * 1) It generates derived predicates for each integer attribute of the form
 *    x <= c, x >= c, and x = c, where c is a constant given by the user
 * 2) It generates a new set of data points based on the derived attributes
 * 3) It generates a new set of Horn constraints based on these data points
 * 4) It runs Houdini on the data points and Horn constraints
 * 5) If a consistent conjunction exists, it constructs an equivalent decision
 *    tree and returns
 *
 * Throws an <code>no_conjunction_exists_exception</code> if no conjunction exists.
 *
 * @param metadata The meta data
 * @param datapoints The data points
 * @param horn_indexes The Horn constraints, modeled as indexes into the <code>datapoints</code> vector
 * @param intervals The intervals describing where individual annotations begin and end in a data point
 * @param c The threshold used to generate derived predicates
 *
 * @return a decision tree equivalent to the conjunctions computed by Houdini (if a conjunction exists)
 */
decision_tree horndini_prephase(const attributes_metadata & metadata, const std::vector<datapoint<bool>> & datapoints, const std::vector<std::pair<std::set<unsigned>, std::set<unsigned>>> & horn_indexes, const std::vector<std::pair<unsigned, unsigned>> & intervals, unsigned c = 1)
{
	//
	// Generate data for Horndini
	//
	auto derived_predicates_and_intervals = horndini::generate_derived_predicates_and_intervals(intervals, c);

	auto derived_datapoints = horndini::generate_derived_datapoints(datapoints, derived_predicates_and_intervals.first);
	auto derived_horn_constraints = boogie_io_object.indexes2horn_constraints(horn_indexes, derived_datapoints);
	assert (metadata.number_of_categories().size() == 1 && metadata.number_of_categories()[0] == derived_predicates_and_intervals.second.size());


	//
	// Generate initial conjunctions
	//
	std::vector<std::list<unsigned>> conjunctions(derived_predicates_and_intervals.second.size());
	for (unsigned i = 0 ; i < derived_predicates_and_intervals.second.size(); ++i)
	{
		for (unsigned j = derived_predicates_and_intervals.second[i].first; j <= derived_predicates_and_intervals.second[i].second; ++j)
		{
			conjunctions[i].push_back(j);
		}
	}


	//
	// Run Horndini
	//

	horndini::learn(derived_datapoints, derived_horn_constraints, conjunctions);
	auto horndini_tree = horndini::conjunctions2tree(metadata, derived_predicates_and_intervals.first, conjunctions);
			
	// Debug checks			
	for (unsigned i = 0; i < conjunctions.size(); ++i)
	{
		for (const auto & c : conjunctions[i])
		{
			assert (c >= derived_predicates_and_intervals.second[i].first && c <= derived_predicates_and_intervals.second[i].second);
			assert (derived_predicates_and_intervals.first[c]._index >= intervals[i].first);
			assert (derived_predicates_and_intervals.first[c]._index <= intervals[i].second);
		}
	}


	return horndini_tree;

}


/**
 * Prints a help message to an output stream.
 *
 * @param out The output stream to write to
 * @param name The name of the program
 */
void print_help(std::ostream & out, const char * name)
{

	out << "Usage: " << name << " [options] file_stem\n";
	out << "Options are:\n";
	out << "  -b\t\tBound the learner\n";
	out << "  -h\t\tRun Horndini pre-phase\n";

}


decision_tree learn_decision_tree (bool do_horndini_prephase, bool use_bounds) {

	//
	// Run the learner and allow a graceful exit if something goes wrong
	//
	try 
	{
		//
		// Check input
		//
		if (metadata.int_names().size() + metadata.categorical_names().size() == 0)
		{
			throw std::runtime_error("No attributes defined");
		}
		if (metadata.number_of_categories().size() < 0)
		{
			throw std::runtime_error("At least one categorical attribute (the ID of the annotation) is required");
		}
		if (metadata.number_of_categories()[0] != intervals.size())
		{
			throw std::runtime_error("Intervals file does not match number of annotations to synthesize");
		}

		//
		// Create bounds
		//
		bound<> cur_bound (1, use_bounds);

		
		/************************************************************************************
		 *
		 * Do Horndini prephase if desired
		 *
		 ************************************************************************************/
		if (do_horndini_prephase)
		{

			//
			// Create copy of data points and pointers thereof
			//
			std::vector<datapoint<bool>> datapoints_copy;
			datapoints_copy.reserve(datapoints.size());
			for (unsigned i = 0; i < datapoints.size(); ++i)
			{
				datapoints_copy.push_back(datapoints[i]);
			}
			
			std::vector<datapoint<bool> *> datapoint_ptrs;
			datapoint_ptrs.reserve(datapoints.size());
			for (unsigned i = 0; i < datapoints_copy.size(); ++i)
			{
				datapoint_ptrs.push_back(&datapoints_copy[i]);
			}

			//
			// Create Horn constraints
			//
			auto horn_constraints = boogie_io::indexes2horn_constraints(horn_indexes, datapoints_copy);


			//
			// Run Horndini
			//
			try // There might not be a conjunctive invariant
			{
				
				auto horndini_tree = horndini_prephase(metadata, datapoints_copy, horn_indexes, intervals);


				//
				// Output and exit if consistent
				//
				auto horndini_consistent = learner<complex_job_manager>::is_consistent(horndini_tree, datapoint_ptrs, horn_constraints);
				if (horndini_consistent)
				{
					return horndini_tree;
				}
			}
			
			// Catch exception that no consistent conjunction exists 
			catch (const no_conjunction_exists_exception & ex)
			{
				// Do nothing, just proceed to decision tree learning phase
			}			
		}

		/************************************************************************************
		 *
		 * Run decision tree learner
		 *
		 ************************************************************************************/

		//
		// Loop over increasing bounds (depending on the implementation of the bounds object, at some point no bound is used)
		//
		bool terminate;
		do
		{
			terminate = !cur_bound.use_bound();


			//
			// Try-catch block is used to handle situations where bounds are too small and need to be increased
			//
			try
			{

				//
				// Create copy of data points and pointers thereof
				//
				std::vector<datapoint<bool>> datapoints_copy;
				datapoints_copy.reserve(datapoints.size());
				for (unsigned i = 0; i < datapoints.size(); ++i)
				{
					datapoints_copy.push_back(datapoints[i]);
				}
				std::vector<datapoint<bool> *> datapoint_ptrs;
				datapoint_ptrs.reserve(datapoints.size());
				for (unsigned i = 0; i < datapoints_copy.size(); ++i)
				{
					datapoint_ptrs.push_back(&datapoints_copy[i]);
				}


				//
				// Generate Horn constraints
				//
				auto horn_constraints = boogie_io::indexes2horn_constraints(horn_indexes, datapoints_copy);

				//
				// Create bound constraints if necessary
				//
				if (cur_bound.use_bound())
				{
					
					std::vector<horn_constraint<bool>> indistinguishability_horn_constraints;
					
					// Infer horn constraints which arise due to indistinguishability of data points
					indistinguishability_horn_constraints = boogie_io::get_indistinguishable_datapoints(datapoints_copy, cur_bound.get_bound());

					// Push these indistinguishability_horn_constraints to horn_constraints
					horn_constraints.insert(horn_constraints.end(), std::make_move_iterator(indistinguishability_horn_constraints.begin()), std::make_move_iterator(indistinguishability_horn_constraints.end()));
					
				}

				//
				// Instantiate Horn solver and perform initial run to label data points if necessary
				//
				horn_solver<bool> solver;
				std::unordered_set<datapoint<bool> *> positive_ptrs;
				std::unordered_set<datapoint<bool> *> negative_ptrs;

				// Initial run
				auto ok = solver.solve(datapoint_ptrs, horn_constraints, positive_ptrs, negative_ptrs);
				
				if (ok)
				{
					
					for (auto & dp : positive_ptrs)
					{
						dp->_is_classified = true;
						dp->_classification = true;	
					}

					for (auto & dp : negative_ptrs)
					{
						dp->_is_classified = true;
						dp->_classification = false;	
					}					
				}
				else
				{
					throw sample_error("No consistent decision tree exists (Horn clauses are contradictory)");
				}

				//
				// Run decision tree learner
				//
				// You can configure different heuristics for the learner by passing, at the end, arguments
				//   NodeSelection enum type with values BFS, DFS, RANDOM, MAX_ENTROPY, MAX_WEIGHTED_ENTROPY, MIN_ENTROPY, MIN_WEIGHTED_ENTROPY
				//   EntropyComputation enum type with values DEFAULT_ENTROPY, HORN_ASSIGNMENTS
				//   ConjunctiveSetting enum type with values NOPREFERENCEFORCONJUNCTS, PREFERENCEFORCONJUNCTS
				auto ns = NodeSelection::BFS;
				auto ec = EntropyComputation::PENALTY;
				auto cs = ConjunctiveSetting::NOPREFERENCEFORCONJUNCTS;
				auto manager = cur_bound.use_bound() ? complex_job_manager(datapoint_ptrs, horn_constraints, solver, cur_bound.get_bound(), ns, ec, cs) : complex_job_manager(datapoint_ptrs, horn_constraints, solver, ns, ec, cs);
				learner<complex_job_manager> l(manager);
				auto decision_tree = l.learn(metadata, datapoint_ptrs, horn_constraints);

				//
				// Debug
				//
				assert (l.is_consistent(decision_tree, datapoint_ptrs, horn_constraints));
				
				terminate = true;

				return decision_tree;
			}

			// Sample is inconsistent
			catch (const sample_error & ex)
			{

				// Increase bounds or declare inconsistent sample if no bounds ought to be used
				if(cur_bound.use_bound())
				{
					cur_bound.increase_bound();
				}
				else
				{
					throw;
				}
				
			}
			
			// No split possible
			// Sample is inconsistent
			catch (const split_not_possible_error & ex)
			{
			
				// Increase bounds or declare inconsistent sample if no bounds ought to be used
				if(cur_bound.use_bound())
				{
					cur_bound.increase_bound();
				}
				else
				{
					throw;
				}	
			}

		} while (!terminate); // Loops over increasing bounds, should terminate at some point
	}

	//
	// Handle exceptions (basically exit gracefully)
	//
	catch (const std::exception & ex)
	{
		std::cerr << ex.what() << '\n';
	}
	catch (...)
	{
		std::cerr << "The learner crashed due to an unknown reason\n";
	}
	
	// Default return value for error cases
	return decision_tree();
}

	};
} // namespace horn_verification

#endif
