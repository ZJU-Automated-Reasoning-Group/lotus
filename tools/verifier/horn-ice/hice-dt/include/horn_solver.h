/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef HORN_SOLVER_H_
#define HORN_SOLVER_H_

// C++ includes
#include <ctime>
#include <stack>
#include <sys/time.h>
#include <unordered_set>
#include <vector>

// Project includes
#include "horn_constraint.h"

namespace horn_verification {

	/**
 	 * This class represents a horn solver.
 	 *
 	 * @tparam T Type of a horn solver.
 	 *
 	 * @author Ezudheen P
 	 * @version 1.0
 	 */
	template <class T> class horn_solver {
	public:
		/// Elapsed time during the entire itrations.
		clock_t elapsed_time;

		/// Elapsed time during the entire itrations.
		unsigned long invocation_count;

	private:

			/// Indicate whether the set of horn constraints become satisfiable or unsatisfiable, default value is true.
			mutable bool horn_constraints_satisfiability;

			/// Contain all classified datapoints.
			mutable std::stack <datapoint<T> *> worklist;

			/// Store the starting time of execution.
			mutable timeval start_time;

			/// Store the finishing time of execution.
			mutable timeval end_time;

			/// Store the copy of input datapoints.
			mutable std::vector<datapoint<T> > _copy_of_datapoints;

			/// Store the copy of input datapoints.
			mutable std::vector<datapoint<T> > _copy_of_positive;

			/// Store the copy of input datapoints.
			mutable std::vector<datapoint<T> > _copy_of_negative;

			/// Store the unmodified horn constraints.
			mutable std::vector<horn_constraint<T> > _copy_of_horn_constraints;

			/// Create an exclusive false datapoint.
			mutable datapoint<T> _false_datapoint;

			/// Flag for indicating presence of horn constraints without conclusion.
			mutable bool _horn_constraints_without_conclusion = false;

			/// Flag for indicating the requirement for computing current marking of datapoints.
			mutable bool _compute_current_marking = true;

			/**
			* Fill worklist stack with classified datapoints.
			* @param vector of datapoints.
			*/
			void fill_worklist (const std::vector<datapoint<T> *> &datapoints, std::unordered_set <datapoint<T> *> &positive, std::unordered_set <datapoint<T> *> &negative) const;

			/**
			* Restore the old classifications of datapoints.
			* @param vector of datapoints.
			*/
			void roll_back_datapoints(const std::vector<datapoint<T> *> &datapoints) const;

			/**
			* Compute the all possible markings of datapoints until a fix point reaches.
			* @param vector of horn constraints.
			*/
			void compute_current_marking() const;

			/**
			* Compute extra classifications of datapoints derivable from given the set of partial classifications
			* of datapoints and the given set of horn constraints.
			* @param vector of horn constraints.
			* @param vector of datapoints.
			* @returns true if no horn constraints are violated and extra classifications will be persisted in the given arguments.
			* Some new datapoints may have classified, premises of some horn constraints may have reduced by removing the datapoints which are classified as true.
			* @returns false if any horn constraint get violated.
			*/
			bool verify_constraints_satisfiability(std::unordered_set <datapoint<T> *> & positive, std::unordered_set <datapoint<T> *> & negative) const;

			/**
			* Remove a datapoint from a vector of datapoints if the given datapoint is present in the vector.
			* @param vector of datapoints.
			* @param a datapoint element.
			*/
			void remove_from_vector(std::vector <datapoint<T> *> &vector_of_objects, datapoint<T> *object_to_romove) const;

			/**
			* Propagate the truth classification of a datapoint to all horn constraints and modify the horn constraints according to the new classification
			* of premises and conclusions, compute all extra positive classifications until a fix point reaches.
			* If the new valuation makes any horn constraints trivial then mark that horn constraint as satisfied.
			* @param vector of horn constraints.
			* @param a datapoint element.
			*/
			bool propagate_true_classification(horn_constraint<T> *current_horn_clause_addr, datapoint<T> *current_variable_addr, std::unordered_set <datapoint<T> *> & positive) const;

			/**
			* Propogate the false classification of a datapoint to all horn constraints and modify the horn constraints according to the new classification
			* of premises and conclusions, compute all extra positive classifications until a fix point reaches.
			* If the new valuation makes any horn constraints trivial then mark that horn constraint as satisfied.
			* @param vector of horn constraints.
			* @param a datapoint element.
			*/
			bool propagate_false_classification(horn_constraint<T> *current_horn_clause_addr, datapoint<T> *current_variable_addr, std::unordered_set <datapoint<T> *> & negative) const;

			/**
			* Compute the initial marking of datapoints and store in list_of_marking.
			* Compute the set of horn constraints which uses the datapoint as either a premise or conclusion and store in list_of_horn_constraints.
			* @param vector of datapoints.
			* @param vector of horn constraints.
			*/
			void populate_meta_data(const std::vector<datapoint<T> *> &datapoints, std::vector<horn_constraint<T> > &horn_constraints) const;

			/**
			* Keep a copy of unmodified vector of datapoints as old_values_of_datapoints and unmodified vector of horn constraints as old_values_of_horn_constraints.
			* @param vector of datapoints.
			* @param vector of horn constraints.
			*/
			void keep_backup(const std::vector<datapoint<T> *> &datapoints, const std::vector<horn_constraint<T> > &horn_constraints, std::unordered_set <datapoint<T> *> &positive, std::unordered_set <datapoint<T> *> & negative) const;

			/**
			* Keep a copy of unmodified vector of datapoints as old_values_of_datapoints and unmodified vector of horn constraints as old_values_of_horn_constraints.
			* @param vector of datapoints.
			* @param vector of horn constraints.
			*/
			void create_log_file (const std::vector<datapoint<T> *> &datapoints, const std::vector<horn_constraint<T> > &horn_constraints, std::unordered_set <datapoint<T> *> & positive, std::unordered_set <datapoint<T> *> & negative) const;


			/**
			* Compute extra classifications of datapoints derivable from given set of partial classifications of datapoints and a given
			* set of horn constraints. If any horn constraint is not satisfiable then the arguments will be restored with their old values.
			* @param vector of horn constraints.
			* @param vector of datapoints.
			* @returns true if no horn constraints are violated and extra classifications will be persisted in the given arguments.
			* Some new datapoints may have classified, premises of some horn constraints may have reduced by removing the datapoints which are classified as true.
			* @returns false if any horn constraint get violated, then no change will be affected on given arguments.
			*/
			bool solve_horn_constraints(const std::vector<datapoint<T> *> &vector_datapoints, const std::vector<horn_constraint<T> > &vector_horn_constraints) const;

		public:

			/**
			* Implementation of external interface of horn solver.
			* This functor computes extra classifications of datapoints derivable from a given set of partial classifications of datapoints and a given
			* set of horn constraints. If any horn constraint is not satisfiable then the arguments will be restored with their old values.
			* @param datapoints vector
			* @param horn_constraints vector
			* @param positive datapoints vector
			* @param negative datapoints vector
			* @returns true if no horn constraint is violated and extra classifications will be stored in vector of positive and negative datapoints.
			* @returns false if any horn constraint get violated, then vector of positive and negative datapoints will be empty.
			*/
			bool solve(const std::vector<datapoint<T> *> &datapoints, const std::vector<horn_constraint<T> > &horn_constraints, std::unordered_set <datapoint<T> *> &positive, std::unordered_set <datapoint<T> *> &negative);

			virtual ~horn_solver();

			/**
			* Constructor.
			*/
			horn_solver(){
				horn_constraints_satisfiability = true;
				invocation_count = 0;
				elapsed_time = 0;
			};
		};
} // namespace horn_verification
#endif /* HORN_SOLVER_H_ */
