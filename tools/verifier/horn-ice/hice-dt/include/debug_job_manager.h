/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __SIMPLE_JOB_MANAGER_H__
#define __SIMPLE_JOB_MANAGER_H__

// C++ includes
#include <list>
#include <memory>
#include <unordered_set>
#include <stdexcept>
#include <vector>
#include <map>

// C includes
#include <cassert>
#include <math.h>

// Project includes
#include "datapoint.h"
#include "error.h"
#include "horn_constraint.h"
#include "horn_solver.h"
#include "job.h"
#include "slice.h"


namespace horn_verification
{

	template <class T>
	std::ostream & operator<<(std::ostream & out, const horn_constraint<T> & c)
	{

		out << "{";
	
		// Left-hand side
		unsigned i = 0;
		for (const auto & dp : c._premises)
		{
			
			if (i++ > 0)
			{
				out << ", ";
			}
			
			if (dp == nullptr)
			{
				out << "NULL";
			}
			else
			{
				out << *dp;
			}
			
		}
		
		// Right-hand side
		out << "} => ";
		
		if (c._conclusion == nullptr)
		{
			out << "NULL";
		}
		else
		{
			out << *c._conclusion;
		}
		
		return out;
		
	}


	/**
	 * Implements a simple job manager using entropy as the split heuristic.
	 *
	 * A job manager implements heuristics for decising
	 * <ul>
	 *   <li> when to split and when to turn a \ref slice into a leaf node; and</li>
	 *   <li> if a \ref slice is to be split, how to split it.</li>
	 * </ul>
	 *
	 
	 *
	 * A job manager has to implement the following three methods:
	 * <ul>
	 *  <li>add_slice(const slice &) (preferably also add_slice(slice &&)):
	 *      This method adds a new \ref slice to the job manager.</li>
	 *  <li>next_job(): This method returns the job the manager wants to have
	 *      performed next. A job can be to splt a node (i.e., either a
	 *      \ref categorical_split_job or an \ref int_split_job, depending on
	 *      which type of attribute the learner should split on) or to create a
	 *      leaf node (i.e., a \ref leaf_creation_job). In order to facilitate
	 *      polymorphism and be exception safe, the manager returns a \p unique_ptr.</li>
	 *  <li>has_jobs(): This method returns whether there are jobts that the
	 *      learner needs to process.</li>
	 * </ul>
	 
	 * The learner uses the job manager in the following way:
	 * <ol>
	 *   <li> The learning algorithm adds slices using add_slice(). </li>
	 *   <li> The job manager decides which \ref slice to process next and what
	 *        to do with the \ref slice (create a leaf or split).</li>
	 *   <li> The learning algorithm calls next_job() to retrieve the next job
	 *        and processes it.</li>
	 * </ol>
	 * This process repeats until all jobs have been processed (i.e., has_jobs()
	 * returns \c false).
	 */
	class debug_job_manager
	{

	protected:
		
		/// The slices that need to be processed
		std::list<slice> _slices;
	
		/// A reference to the set of (pointers to) data points
		std::vector<datapoint<bool> *> & _datapoint_ptrs;
	
		/// A reference to the horn constraints 
		const std::vector<horn_constraint<bool>> & _horn_constraints;
	
		/// The solver for Horn clauses
		horn_solver<bool> & _horn_solver;
	
		/// Indicates whether this manager has is about to perform the first split
		bool _is_first_split = true;
		
		
	public:
	
		/**
		 * Creates a new simple job manager.
		 *
		 * @param datapoint_ptrs A reference to the set of (pointers to) data points over which to work
		 * @param horn_constraints A reference to the horn constraints over which to work
		 * @param solver A reference to the Horn solver to use
		 */
		debug_job_manager(std::vector<datapoint<bool> *> & datapoint_ptrs, const std::vector<horn_constraint<bool>> & horn_constraints, horn_solver<bool> & solver)
			: _datapoint_ptrs(datapoint_ptrs), _horn_constraints(horn_constraints), _horn_solver(solver)
		{
			// Nothing
		}
	

		/**
		 * Adds a new slice to the manager.
		 *
		 * @param sl the slice to add
		 */
		void add_slice(const slice & sl)
		{
			_slices.push_back(sl);
		}

		
		/**
		 * Adds a new slice to the manager using move semantics.
		 *
		 * @param sl the slice to add
		 */
		void add_slice(slice && sl)
		{
			_slices.push_back(std::move(sl));
		}
		
		
		/**
		 * Checks whether the job manager has jobs that need to be processed.
		 *
		 * @returns whether the job manager has jobs that need to be processed
		 */
		inline bool has_jobs() const
		{
			return !_slices.empty();
		}
		
		
		/**
		 * Returns the next job. If no job exists (i.e., calling learner::empty() returns \c true),
		 * the bahavior is undefined.
		 *
		 * This implementation is simplistic in that it retrieves slices in a breadth-first order
		 * and uses a simple entropy measure to split slices.
		 *
		 * @returns a unique pointer to the next job
		 */
		std::unique_ptr<abstract_job> next_job()
		{
		
			//
			// Get next slice
			//
			auto sl = _slices.back();
			_slices.pop_back();
			//std::cout << sl << std::endl;
		
		
			//
			// If this is the first split , split on the unique categorical attribute
			//
			if (_is_first_split)
			{
			
				// Check if data points have exactly one categorical attribute
				if (_datapoint_ptrs[sl._left_index]->_categorical_data.size() != 1)
				{
					throw std::runtime_error("Learner expects exactly one categorical attribute");
				}
				
				_is_first_split = false;
				return std::unique_ptr<abstract_job> { std::make_unique<categorical_split_job>(sl, 0) };
				
			}
		
		
			//
			// Determine what needs to be done (split or create leaf)
			//
			auto label = false; // label is unimportant (if is_leaf() returns false)
			auto positive_ptrs = std::unordered_set<datapoint<bool> *>();
			auto negative_ptrs = std::unordered_set<datapoint<bool> *>();
			auto can_be_turned_into_leaf = is_leaf(sl, label, positive_ptrs, negative_ptrs);

			// Slice can be turned into a leaf node
			if (can_be_turned_into_leaf)
			{
				return std::unique_ptr<abstract_job> {std::make_unique<leaf_creation_job>(sl, label, std::move(positive_ptrs), std::move(negative_ptrs))};
			}
			// Slice needs to be split
			else
			{
				return find_best_split(sl);
			}
		
		}
		
	protected:
	
		/**
		 * Checks whether a slice can be turned into a leaf node. If so, this method also
		 * determines the label of the leaf node and which unlabeled data points need to
		 * be labaled positively and negatively, respectively, in order to satisfy the
		 * horn constraints.
		 *
		 * @param sl The slice to check
		 * @param label If the slice can be turned into a leaf, this parameter is used to return the label
		 *              of the leaf node
		 * @param positive_ptrs If the slice can be turned into a leaf, this paramater is used to return
		 *                      a set of (pointers to) unlabaled data points which have to be labeled positively
		 * @param negative_ptrs If the slice can be turned into a leaf, this paramater is used to return
		 *                      a set of (pointers to) unlabaled data points which have to be labeled negatively
		 *
		 * @return whether this slice can be turned into a leaf node
		 */
		bool is_leaf(const slice & sl, bool & label, std::unordered_set<datapoint<bool> *> & positive_ptrs, std::unordered_set<datapoint<bool> *> & negative_ptrs)
		{
		
			assert (sl._left_index <= sl._right_index && sl._right_index < _datapoint_ptrs.size());
			assert (positive_ptrs.empty());
			assert (negative_ptrs.empty());
		
		
			//
			// Check which classifications occur
			//
			bool found_true = false;
			bool found_false = false;
			bool found_unlabeled = false;
			std::size_t index_of_first_unlabeled = 0;
			std::size_t index_of_last_unlabeled = 0;
		
			for (std::size_t i = sl._left_index; i <= sl._right_index; ++i)
			{
				
				if (_datapoint_ptrs[i]->_is_classified)
				{
					if (_datapoint_ptrs[i]->_classification)
					{
						found_true = true;
					}
					else
					{
						found_false = true;
					}
				}
				else
				{
					if (!found_unlabeled)
					{
						found_unlabeled = true;
						index_of_first_unlabeled = i;
						index_of_last_unlabeled = i;
					}
					else
					{
						index_of_last_unlabeled = i;
					}
				}
					
				
				if (found_true && found_false)
				{
					break;
				}
				
			}
		
			//std::cout << "========== " << found_true << " / " << found_false << " / " << found_unlabeled << " (+/-/?); slice is [" << sl._left_index << " - " << sl._right_index << "]" << std::endl;
		
			//
			// Found positively and negatively classified data points, thus no leaf node
			//
			if (found_true && found_false)
			{
				return false;
			}
		
		
			//
			// Only positively or only negatively classified data points (i.e., no unlabaled)
			//
			else if (!found_unlabeled)
			{
				label = found_true; // Since either positive or negative data points occur, found_true indicates which ones occur
				return true;
			}


			//
			// Unlabeled and positive data points
			//
			else if (found_true)
			{
				
				// Collect unlabeled data points
				for (std::size_t i = index_of_first_unlabeled; i <= index_of_last_unlabeled; ++i)
				{
					if (!_datapoint_ptrs[i]->_is_classified)
					{
						positive_ptrs.insert(_datapoint_ptrs[i]);
					}
				}
				
				// Run Horn solver
				//output_state(positive_ptrs, negative_ptrs, _horn_constraints, "\n---------- + and ? (mark +)", std::cout);
				//horn_solver<bool> solver;
				//auto ok = solver.solve(_datapoint_ptrs, _horn_constraints, positive_ptrs, negative_ptrs);
				auto ok = _horn_solver.solve(_datapoint_ptrs, _horn_constraints, positive_ptrs, negative_ptrs);
				//output_state(positive_ptrs, negative_ptrs, _horn_constraints, "\n---------- Solver result: " + std::to_string(ok), std::cout);
				
				// Labeling satisfies Horn constraints
				if (ok)
				{
					label = true;
					return true;
				}

				// Labeling does not satisfy Horn constraints
				else
				{
					// positive_ptrs.clear();
					return false;
				}
				
			}

			
			//
			// Unlabeled and negative data points
			//
			else if (found_false)
			{
				
				// Collect unlabeled data points
				for (std::size_t i = index_of_first_unlabeled; i <= index_of_last_unlabeled; ++i)
				{
					if (!_datapoint_ptrs[i]->_is_classified)
					{
						negative_ptrs.insert(_datapoint_ptrs[i]);
					}
				}
				
				// Run Horn solver
				//output_state(positive_ptrs, negative_ptrs, _horn_constraints, "\n---------- - and ? (mark -)", std::cout);
				//horn_solver<bool> solver;
				auto ok = _horn_solver.solve(_datapoint_ptrs, _horn_constraints, positive_ptrs, negative_ptrs);
				//output_state(positive_ptrs, negative_ptrs, _horn_constraints, "\n---------- Solver result: " + std::to_string(ok), std::cout);
				
				// Labeling satisfies Horn constraints
				if (ok)
				{
					label = false;
					return true;
				}

				// Labeling does not satisfy Horn constraints
				else
				{
					// negative_ptrs.clear();
					return false;
				}
				
			}

			//
			// Only unlabeled data points
			//
			else
			{
				
				//
				// Try to turn all data points positive
				//
				for (std::size_t i = sl._left_index; i <= sl._right_index; ++i)
				{
					positive_ptrs.insert(_datapoint_ptrs[i]);
				}
				
				// Run Horn solver
				//output_state(positive_ptrs, negative_ptrs, _horn_constraints, "\n---------- All ? (mark +)", std::cout);
				//horn_solver<bool> solver;
				auto ok = _horn_solver.solve(_datapoint_ptrs, _horn_constraints, positive_ptrs, negative_ptrs);
				//output_state(positive_ptrs, negative_ptrs, _horn_constraints, "\n---------- Solver result: " + std::to_string(ok), std::cout);
				
				// If labeling satisfies Horn constraints, report leaf with classification true
				if (ok)
				{
					label = true;
					return true;					
				}
				
				
				//
				// Try to turn all data points negative
				//
				positive_ptrs.clear();
				for (std::size_t i = sl._left_index; i <= sl._right_index; ++i)
				{
					negative_ptrs.insert(_datapoint_ptrs[i]);
				}
				
				// Run Horn solver
				//output_state(positive_ptrs, negative_ptrs, _horn_constraints, "\n---------- All ? (mark -)", std::cout);
				//horn_solver<bool> solver1;
				ok = _horn_solver.solve(_datapoint_ptrs, _horn_constraints, positive_ptrs, negative_ptrs);
				//output_state(positive_ptrs, negative_ptrs, _horn_constraints, "\n---------- Solver result: " + std::to_string(ok), std::cout);
				
				// If labeling satisfies Horn constraints, report leaf with classification false
				if (ok)
				{
					label = false;
					return true;					
				}
				

				//
				// Split is necessary
				//
				
				return false;
				
			}

		}
		
		template <class T>
		void output_state(const std::unordered_set<datapoint<T> *> & positive_ptrs, const std::unordered_set<datapoint<T> *> & negative_ptrs, const std::vector<horn_constraint<T>> & horn_constraints, const std::string & headline, std::ostream & out)
		{

			// Headline
			out << headline << '\n';
		
			// Positive data points
			out << "Positive data points (" << positive_ptrs.size() << "):\n";
			for (const auto & dp : positive_ptrs)
			{
				out << *dp << '\n';
			}
			
			// Negative data points
			out << "Negative data points (" << negative_ptrs.size() << "):\n";
			for (const auto & dp : negative_ptrs)
			{
				out << *dp << '\n';
			}
			
			// Horn constraints
			out << "Horn constraints (" << horn_constraints.size() << "):\n";
			for (const auto & clause : horn_constraints)
			{
				
				for (const auto & dp : clause._premises)
				{
					out << "(" << *dp << ") ";
				}
				
				out << " ==>  ";
				if (clause._conclusion)
				{
					out << "(" << *clause._conclusion << ")";
				}
				else
				{
					out << "(null)";
				}
				out << '\n';
				
			}
			
		}
		
		/**
		 * Computes the best split of a contiguous set of data points and returns the corresponding
		 * split job. If no split (that allows progress) is possible, this function should throw
		 * an exception.
		 *
		 * @param sl The slice of data points to be split
		 *
		 * @returns a unique pointer to the job created
		 */
		std::unique_ptr<abstract_job> find_best_split(const slice & sl)
		{
		
			//std::cout << "---------- " << sl << " ----------" << std::endl;
		
			assert (sl._left_index <= sl._right_index && sl._right_index < _datapoint_ptrs.size());

			// 0) Initialize variables
			bool int_split_possible = false;
			double best_int_entropy = 1000;
			std::size_t best_int_attribute = 0;
			int best_int_threshold = 0;
			
			bool cat_split_possible = false;
			double best_cat_entropy = 1000;
			std::size_t best_cat_attribute = 0;

			
			//
			// Process categorical attributes
			//
			for (std::size_t attribute = 0; attribute < _datapoint_ptrs[sl._left_index]->_categorical_data.size(); ++attribute)
			{
			
				// 1) Sort according to categorical attribute
				auto comparer = [attribute](const datapoint<bool> * const a, const datapoint<bool> * const b) { return a->_categorical_data[attribute] < b->_categorical_data[attribute]; };
				std::sort(_datapoint_ptrs.begin() + sl._left_index, _datapoint_ptrs.begin() + sl._right_index + 1, comparer);
				
				
				// 2) sum all weighted entropies
				double total_weighted_entropy = 0;
				bool split_possible = true;
				auto cur_left = sl._left_index;
				auto cur_right = cur_left;

				while (cur_right <= sl._right_index)
				{
				
					auto cur_category = _datapoint_ptrs[cur_left]->_categorical_data[attribute];

					while (cur_right + 1 <= sl._right_index && cur_category == _datapoint_ptrs[cur_right + 1]->_categorical_data[attribute])
					{
						++cur_right;
					}
			
			
					// If only one category, skip attribute
					if (cur_left == sl._left_index && cur_right == sl._right_index)
					{
						split_possible = false;
						break;
					}
					else
					{
						total_weighted_entropy += weighted_entropy(_datapoint_ptrs, cur_left, cur_right);
						cur_left = cur_right + 1;
						cur_right = cur_left;
					}
				}
				
				if ((!cat_split_possible && split_possible) || (split_possible && total_weighted_entropy < best_cat_entropy))
				{
					
					cat_split_possible = true;
					
					best_cat_attribute = attribute;
					best_cat_entropy = total_weighted_entropy;
				}
				
			}
			
			//std::cout << "Now processing integer splits" << std::endl;
			//
			// Process integer attributes
			//
			for (std::size_t attribute = 0; attribute < _datapoint_ptrs[sl._left_index]->_int_data.size(); ++attribute)
			{
			
				// 1) Sort according to int attribute
				auto comparer = [attribute](const datapoint<bool> * const a, const datapoint<bool> * const b) { return a->_int_data[attribute] < b->_int_data[attribute]; };
				std::sort(_datapoint_ptrs.begin() + sl._left_index, _datapoint_ptrs.begin() + sl._right_index + 1, comparer);
				
				
				// 2) Try all thresholds of current attribute
				auto cur = sl._left_index;
				while (cur < sl._right_index)
				{
				
					// Skip to riight most entry with the same value
					while (cur + 1 <= sl._right_index && _datapoint_ptrs[cur + 1]->_int_data[attribute] == _datapoint_ptrs[cur]->_int_data[attribute])
					{
						++cur;
					}

					// Split is possible
					if (cur < sl._right_index)
					{

						//std::cout << "considering attribute: " << attribute << " sl._left_index: " << cur << " cut: " << _datapoint_ptrs[cur]->_int_data[attribute] << std::endl;
						auto weighted_entropy_left = weighted_entropy(_datapoint_ptrs, sl._left_index, cur);
						auto weighted_entropy_right = weighted_entropy(_datapoint_ptrs, cur + 1, sl._right_index);
						auto total_weighted_entropy = weighted_entropy_left + weighted_entropy_right;
					
						//std::cout << "(" << sl._left_index << ", " << cur << "," << sl._right_index << ") / attr=" << attribute << ": Total entropy is " << total_weighted_entropy;
					
						total_weighted_entropy += penalty(sl, sl._left_index, cur, cur + 1, sl._right_index);
						total_weighted_entropy += penalty(sl, cur + 1, sl._right_index, sl._left_index, cur);
					
						//std::cout << "; with penalty " << total_weighted_entropy << std::endl;
					
						if (!int_split_possible || total_weighted_entropy < best_int_entropy)
						{
							//std::cout << "updated the entropy; split is now definitely possible" << std::endl;	
							int_split_possible = true;
						
							best_int_entropy = total_weighted_entropy;
							best_int_attribute = attribute;
							best_int_threshold = _datapoint_ptrs[cur]->_int_data[attribute];
						
						}
						
						++cur;
						
					}
					
				}
			
				//std::cout << "----- done ----------" << std::endl;
				//exit(1);
			
			}

			
			//
			// Return best split
			//
			
			if (!int_split_possible && !cat_split_possible)
			{
				throw split_not_possible_error("No split possible!");
			}
			
			else if (int_split_possible && !cat_split_possible)
			{
				return std::unique_ptr<abstract_job> { std::make_unique<int_split_job>(sl, best_int_attribute, best_int_threshold) };
			}
			
			else if (!int_split_possible && cat_split_possible)
			{
				return std::unique_ptr<abstract_job> { std::make_unique<categorical_split_job>(sl, best_cat_attribute) };
			}
			
			else
			{
				
				if (best_int_entropy <= best_cat_entropy)
				{
					return std::unique_ptr<abstract_job> { std::make_unique<int_split_job>(sl, best_int_attribute, best_int_threshold) };
				}
				else
				{
					return std::unique_ptr<abstract_job> { std::make_unique<categorical_split_job>(sl, best_cat_attribute) };
				}

			}
			
		}
		
		
		double penalty(const slice & sl, std::size_t left_index1, std::size_t right_index1, std::size_t left_index2, std::size_t right_index2)
		{
		
			/*
			std::cout << "---------- Penalty computation (" << left_index << "-" << right_index << ") ----------\n";
			for_each(_datapoint_ptrs.begin(), _datapoint_ptrs.end(), [](datapoint<bool> * dp){ std::cout << *dp << '\n'; });
			for_each(_horn_constraints.begin(), _horn_constraints.end(), [](const horn_constraint<bool> & c){ std::cout << c << '\n'; });
			*/

		
			double penalty = 0;
			
			// Compute penalty for all horn constraints
			for (const auto & horn_clause : _horn_constraints)
			{
				
				// Check whether right-hand-side is in second slice
				bool rhs_in_slice2 = false;
				if (horn_clause._conclusion != nullptr)
				{
					for (std::size_t i = left_index2; i <= right_index2; ++i)
					{
						if (horn_clause._conclusion == _datapoint_ptrs[i])
						{
							//std::cout << "Conclusion with index " << i << " in slice 2" << std::endl;
							rhs_in_slice2 = true;
							break;
						}
					}

				}
				
				
				
				
				// Skip if rhs is not in slice
				if (!rhs_in_slice2)
				{
					//std::cout << "Conclusion not in slice2" << std::endl;
					continue;
				}
				

				// Count number of lhs in slice1
				unsigned num_lhs_in_slice = 0;
				for (const auto dp : horn_clause._premises)
				{
					
					for (std::size_t i = left_index1; i <= right_index1; ++i)
					{
						if (dp == _datapoint_ptrs[i])
						{
							//std::cout << "LHS with index " << i << " in slice1" << std::endl;
							++num_lhs_in_slice;
							break;
						}
					}
					
					
				}
				
				// Adapt penalty
				penalty += num_lhs_in_slice > 0 ? 1 : 0;
				
			}
			
			
			//std::cout << "penalty=" << penalty << std::endl;
			
			return penalty;
			
		}	
		
		
		/**
		 * Computes the entropy (with respect to the logarithm of 2) of a contiguous set of data
		 * points.
		 *
		 * @param datapoint_ptrs Pointer to the data points
		 * @param left_index The left bound of the set of data points
		 * @param right_index The right bound of the set of data points
		 *
		 * @return the entropy of the given set of data points
		 */
		double entropy(const std::vector<datapoint<bool> *> & datapoint_ptrs, std::size_t left_index, std::size_t right_index)
		{
			
			unsigned int count_f = 0;
			unsigned int count_t = 0;
			
			for (std::size_t i = left_index; i <= right_index; ++i)
			{
				if (datapoint_ptrs[i]->_is_classified)
				{
					if (datapoint_ptrs[i]->_classification)
					{
						++count_t;
					}
					else
					{
						++count_f;
					}
				}
			}
			
			double sum = count_t + count_f;
			//std::cout << "sum=" << sum << std::endl;
			double p_t = ((double)(count_t) / sum);
			//std::cout << "p_t=" << p_t << std::endl;
			double p_f = ((double)(count_f) / sum);
			//std::cout << "p_f=" << p_f << std::endl;
			
			double entropy_t = count_t == 0 ? 0 : p_t * log2(p_t);
			double entropy_f = count_f == 0 ? 0 : p_f * log2(p_f);
			
			return -(entropy_t + entropy_f);
			
		}
	
		/**
		 * Computes the entropy (with respect to the logarithm of 2) of a contiguous set of data
		 * points, weighted by the number of points classified in the set.
		 *
		 * @param datapoint_ptrs Pointer to the data points
		 * @param left_index The left bound of the set of data points
		 * @param right_index The right bound of the set of data points
		 *
		 * @return the entropy of the given set of data points
		 */
		double weighted_entropy(const std::vector<datapoint<bool> *> & datapoint_ptrs, std::size_t left_index, std::size_t right_index)
		{
			return entropy(datapoint_ptrs, left_index, right_index) * num_classified_points(_datapoint_ptrs, left_index, right_index); 
		}

		/**
		 * Computes the number of classified points in a contiguous set of data points.
		 *
		 * @param datapoint_ptrs Pointer to the data points
		 * @param left_index The left bound of the set of data points
		 * @param right_index The right bound of the set of data points
		 *
		 * @return the number of classified points in a contiguous set of data points.
		 */
		unsigned int num_classified_points(const std::vector<datapoint<bool> *> & datapoint_ptrs, std::size_t left_index, std::size_t right_index)
		{
			
			unsigned int count = 0;
			
			for (std::size_t i = left_index; i <= right_index; ++i)
			{
				if (datapoint_ptrs[i]->_is_classified)
				{
					count++;
				}
			}
			return count;
		}


	};

}; // End namespace horn_verification

#endif
