/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __SIMPLE_JOB_MANAGER_H__
#define __SIMPLE_JOB_MANAGER_H__

// C++ includes
#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <vector>

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

	// Enum to decide the heuristic for selecting the next node while constructing the tree
	enum NodeSelection
	{
		BFS = 0,  	// Selects node in a Breadth-first order
		DFS,		// Selects node in a Depth-first order
		RANDOM,		// Selects a random node
		MAX_ENTROPY,	// Selects the node which has the maximum entropy
		MAX_WEIGHTED_ENTROPY,	// Selects the node which has the maximum entropy weighted by the number of classified points in the node
		MIN_ENTROPY,	// Selects the node which has the minimum entropy
		MIN_WEIGHTED_ENTROPY	// Selects the node which has the minimum entropy wieghted by the number of classified points in the node
	};


	// Enum to select if one prefers conjunctive splits (split carves out a sub-node which includes only negative points or unclassified points) 
	// over non-conjunctive splits
	enum ConjunctiveSetting
	{
		NOPREFERENCEFORCONJUNCTS = 0,
		PREFERENCEFORCONJUNCTS	
	};


	// Enum to decide the heuristic for computing the goodness/badness score (a la entropy) for a set of data points. 
	enum EntropyComputation
	{
		DEFAULT_ENTROPY = 0,		// Ignores horn constraints and computes the entropy using only the positively and negatively classified data points
		PENALTY,		// Adds a linear penalty term based on the number of horn constraints involving data points outside the current set
		HORN_ASSIGNMENTS,	// Estimates the positive/negative distribution of the unclassified points and considers that when computing the entropy
	};


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
	class simple_job_manager
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
	

		/// Threshold which bounds the numerical cuts considered while constructing the tree 
		int _threshold;
		bool _are_numerical_cuts_thresholded;

		bool _is_first_split = true;
		
	public:
	
		/**
		 * Creates a new simple job manager.
		 *
		 * @param datapoint_ptrs A reference to the set of (pointers to) data points over which to work
		 * @param horn_constraints A reference to the horn constraints over which to work
		 * @param solver A reference to the Horn solver to use
		 */
		simple_job_manager(std::vector<datapoint<bool> *> & datapoint_ptrs, const std::vector<horn_constraint<bool>> & horn_constraints, horn_solver<bool> & solver)
			: _datapoint_ptrs(datapoint_ptrs), _horn_constraints(horn_constraints), _horn_solver(solver)
		{
			_are_numerical_cuts_thresholded = false;
		}
	
		/**
		 * Creates a new simple job manager when a threshold is also passed.
		 *
		 * @param datapoint_ptrs A reference to the set of (pointers to) data points over which to work
		 * @param horn_constraints A reference to the horn constraints over which to work
		 * @param solver A reference to the Horn solver to use
		 * @param threshold An unsigned int which serves as the threshold to cuts considered while splitting nodes wrt numerical attributes 
		 */
		simple_job_manager(std::vector<datapoint<bool> *> & datapoint_ptrs, const std::vector<horn_constraint<bool>> & horn_constraints, horn_solver<bool> & solver, \
																			unsigned int threshold)
			: _datapoint_ptrs(datapoint_ptrs), _horn_constraints(horn_constraints), _horn_solver(solver), _threshold(threshold)
		{
			_are_numerical_cuts_thresholded = true;
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
			auto sl = _slices.front();
			_slices.pop_front();
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
			// Unlabaled and positive data points
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
			// Unlabaled and negative data points
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
			// Only unclassified data points
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
			
			assert (sl._left_index <= sl._right_index && sl._right_index < _datapoint_ptrs.size());

			// 0) Initialize variables
			bool int_split_possible = false;
			double best_int_gain_ratio = 0;
			std::size_t best_int_attribute = 0;
			int best_int_threshold = 0;

			bool cat_split_possible = false;
			double best_cat_gain_ratio = 0;
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
				double total_intrinsic_value = 0;
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

						double n1 = 1.0 * num_classified_points(_datapoint_ptrs, cur_left, cur_right);
						double n = 1.0 * num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index);
						total_intrinsic_value += (n1 == 0) ? 0.0 : -1.0 * (n1/n) * log2(n1/n);

						cur_left = cur_right + 1;
						cur_right = cur_left;
					}
				}
				if (split_possible)
				{
					double info_gain;
                                        if (num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index) == 0)
                                        {
                                                info_gain = entropy(_datapoint_ptrs, sl._left_index, sl._right_index);
                                        }
                                        else
                                        {
                                                info_gain = entropy(_datapoint_ptrs, sl._left_index, sl._right_index) - \
                                                                total_weighted_entropy / num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index);

                                        }

					assert (total_intrinsic_value > 0.0);
					double gain_ratio = info_gain / total_intrinsic_value;
					// split is possible on the current "attribute"
					if (! cat_split_possible || gain_ratio > best_cat_gain_ratio)
					{
						cat_split_possible = true;
						best_cat_gain_ratio = gain_ratio;
						best_cat_attribute = attribute;
					}
				}
				
			}
			
			//std::cout << "Now processing integer splits" << std::endl;
			//
			// Process integer attributes
			//
			for (std::size_t attribute = 0; attribute < _datapoint_ptrs[sl._left_index]->_int_data.size(); ++attribute)
			{
				int tries = 0;
				double best_int_entropy_for_given_attribute = 1000000;
				bool int_split_possible_for_given_attribute = false;
				int best_int_split_index_for_given_attribute = 0;
				double best_intrinsic_value_for_given_attribute = 0;


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
						tries++;


						// if cuts have been thresholded, check that a split at the current value of the numerical attribute is allowed
						if (! _are_numerical_cuts_thresholded || \
							       	((-1 * _threshold <= _datapoint_ptrs[cur]->_int_data[attribute]) && \
										       (_datapoint_ptrs[cur]->_int_data[attribute] <= _threshold)))
						{

							//std::cout << "considering attribute: " << attribute << " sl._left_index: " << cur << " cut: " << _datapoint_ptrs[cur]->_int_data[attribute] << std::endl;
							// weighted_entropy_left = H(left_node) * num_classified_points(left_node)
							auto weighted_entropy_left = weighted_entropy(_datapoint_ptrs, sl._left_index, cur);
							// weighted_entropy_right = H(right_node) * num_classified_points(right_node)
							auto weighted_entropy_right = weighted_entropy(_datapoint_ptrs, cur + 1, sl._right_index);
							auto total_weighted_entropy = weighted_entropy_left + weighted_entropy_right;
						
							if (!int_split_possible_for_given_attribute || total_weighted_entropy < best_int_entropy_for_given_attribute)
							{
								//std::cout << "updated the entropy; split is now definitely possible" << std::endl;	
								int_split_possible_for_given_attribute = true;
							
								best_int_entropy_for_given_attribute = total_weighted_entropy;
								best_int_split_index_for_given_attribute = cur;

								// computation of the intrinsic value of the attribute
								double n1 = 1.0 * num_classified_points(_datapoint_ptrs, sl._left_index, cur);
								double n2 = 1.0 * num_classified_points(_datapoint_ptrs, cur + 1, sl._right_index);
								double n = n1 + n2;
								best_intrinsic_value_for_given_attribute = (n1 == 0.0 ? 0.0 : -1.0 * (n1/n) * log2(n1/n)) + \
                                                               		                                          (n2 == 0.0 ? 0.0 : - 1.0 * (n2/n) * log2(n2/n));
									
							}
						
						}
							
						++cur;			
					}
					
				}
				if (int_split_possible_for_given_attribute)
				{
					// We have found the best split threshold for the given attribute
					// Now compute the information gain to optimize across different attributes
					double best_info_gain_for_attribute;					                                        
					if (num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index) == 0.0)
                                        {      
                                                best_info_gain_for_attribute = entropy(_datapoint_ptrs, sl._left_index, sl._right_index) ;
                                        }
                                        else
                                        {
                                                best_info_gain_for_attribute = entropy(_datapoint_ptrs, sl._left_index, sl._right_index) - \
                                                        best_int_entropy_for_given_attribute / num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index);               
                                        }

					double interval = (_datapoint_ptrs[sl._right_index]->_int_data[attribute] - _datapoint_ptrs[sl._left_index]->_int_data[attribute]) / \
							       (_datapoint_ptrs[best_int_split_index_for_given_attribute+1]->_int_data[attribute] - \
							                  _datapoint_ptrs[best_int_split_index_for_given_attribute]->_int_data[attribute]);

					assert (num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index) > 0);
					double threshCost = ( interval < (double)tries ? log2(interval) : log2(tries) ) / \
						                 num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index);

					best_info_gain_for_attribute -= threshCost;
					assert (best_intrinsic_value_for_given_attribute > 0.0);
					double best_gain_ratio_for_given_attribute = best_info_gain_for_attribute / best_intrinsic_value_for_given_attribute;

					if (! int_split_possible || (best_gain_ratio_for_given_attribute > best_int_gain_ratio) || \
					    (best_gain_ratio_for_given_attribute == best_int_gain_ratio && \
					      std::abs(_datapoint_ptrs[best_int_split_index_for_given_attribute]->_int_data[attribute]) < std::abs(best_int_threshold)))	
					{
						// if this is the first attribute for which a split is possible then
						// initialize all variables: best_int_gain_ratio, best_int_attribute, best_int_threshold
						int_split_possible = true;
						best_int_gain_ratio = best_gain_ratio_for_given_attribute;
						best_int_attribute = attribute;
						best_int_threshold = _datapoint_ptrs[best_int_split_index_for_given_attribute]->_int_data[attribute];
					}

				}
			
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
				if (best_int_gain_ratio <= best_cat_gain_ratio)
				{
					return std::unique_ptr<abstract_job> { std::make_unique<int_split_job>(sl, best_int_attribute, best_int_threshold) };
				}
				else
				{
					return std::unique_ptr<abstract_job> { std::make_unique<categorical_split_job>(sl, best_cat_attribute) };
				}

			}
			
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

			
		/**
		 * Returns if an unclassified point is present in a contiguous set of data points.
		 *
		 * @param datapoint_ptrs Pointer to the data points
		 * @param left_index The left bound of the set of data points
		 * @param right_index The right bound of the set of data points
		 *
		 * @return If an unclassified point is present in the contiguous set of data points.
		 */				                                                
		bool unclassified_points_present(const std::vector<datapoint<bool> *> & datapoint_ptrs, std::size_t left_index, std::size_t right_index)
		{
			for (std::size_t i = left_index; i <= right_index; ++i)
			{
				if (! datapoint_ptrs[i]->_is_classified)
				{
					return true;
				}
			}
			return false;
		}

		/**
		 * Returns if a positively labeled point is present in a contiguous set of data points.
		 *
		 * @param datapoint_ptrs Pointer to the data points
		 * @param left_index The left bound of the set of data points
		 * @param right_index The right bound of the set of data points
		 *
		 * @return If a positively labeled point is present in the contiguous set of data points.
		 */				                                                
		bool positive_points_present(const std::vector<datapoint<bool> *> & datapoint_ptrs, std::size_t left_index, std::size_t right_index)
		{
			for (std::size_t i = left_index; i <= right_index; ++i)
			{
				if (datapoint_ptrs[i]->_is_classified && datapoint_ptrs[i]->_classification)
				{
					return true;
				}
			}
			return false;
		}

		/**
		 * Returns number of points with the given classification in a contiguous set of data points.
		 *
		 * @param datapoint_ptrs Pointer to the data points
		 * @param left_index The left bound of the set of data points
		 * @param right_index The right bound of the set of data points
		 * @param classification The classification to count the number of points for
		 *
		 * @return If a positively labeled point is present in the contiguous set of data points.
		 */				                                                
		int num_points_with_classification(const std::vector<datapoint<bool> *> & datapoint_ptrs, std::size_t left_index, std::size_t right_index, bool classification)
		{
			int count = 0;
			for (std::size_t i = left_index; i <= right_index; ++i)
			{
				if (datapoint_ptrs[i]->_is_classified && datapoint_ptrs[i]->_classification == classification)
				{
					count += 1;
				}
			}
			return count;
		}


	};



        /**
         * Implements a complex job manager by deriving from the simple_job_manager.
         *
         * A job manager implements complex heuristics for
         * <ul>
         *   <li> deciding which node to process next; and</li>
	 *   <li> scoring a node with respect to the classification of datapoints in that node (aka entropy computation that takes into account the horn constraints) </li>
         * </ul>
         *
         
         *
         * A job manager specializes the following methods:
         * <ul>
         *  <li>next_job(): This method returns the job the manager wants to
         *      process next. Different heurisitcs are invoked depending on the member enum variable heuristic_for_node_selection. </li>
	 *  <li>entropy(): This method returns an entropy score for a given set of datapoints. Different heuristics compute the entropy differently. Most heuristics take
		into account the horn constraints while computing this score.</li>
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
	class complex_job_manager : public simple_job_manager 
	{

	private:
		NodeSelection _node_selection_criterion;
		EntropyComputation _entropy_computation_criterion;
		ConjunctiveSetting _conjunctive_setting;

		// A map from the set of (pointers to) data points to a fractional value between 0 (negative) and 1 (positive).
		// Fractional value for a datapoint is the likelihood of the given point to be assigned positive in some randomly chosen completion
		// of the Horn assignment. Only used when _entropy_computation_criterion == HORN_ASSIGNMENTS.
		std::map<datapoint<bool> *,double> _datapoint_ptrs_to_frac;


	public:
	        /**
                 * Creates a new complex job manager.
                 *
                 * @param datapoint_ptrs A reference to the set of (pointers to) data points over which to work
                 * @param horn_constraints A reference to the horn constraints over which to work
                 * @param solver A reference to the Horn solver to use
		 * @param node_selection_criterion Node selection heuristic to be used while building the tree
		 * @param entropy_computation_criterion Criterion for scoring a node/slice a la entropy
                 */
                complex_job_manager(std::vector<datapoint<bool> *> & datapoint_ptrs, const std::vector<horn_constraint<bool>> & horn_constraints, horn_solver<bool> & solver, \
						NodeSelection node_selection_criterion, EntropyComputation entropy_computation_criterion, ConjunctiveSetting conjunctive_setting)
                        : simple_job_manager(datapoint_ptrs, horn_constraints, solver)
                {
			_node_selection_criterion = node_selection_criterion;
			_entropy_computation_criterion = entropy_computation_criterion;
			_conjunctive_setting = conjunctive_setting;
                }


                /**
                 * Creates a new complex job manager when a threshold is also passed.
                 *
                 * @param datapoint_ptrs A reference to the set of (pointers to) data points over which to work
                 * @param horn_constraints A reference to the horn constraints over which to work
                 * @param solver A reference to the Horn solver to use
                 * @param threshold An unsigned int which serves as the threshold to cuts considered while splitting nodes wrt numerical attributes 
                 * @param node_selection_criterion Node selection heuristic to be used while building the tree
                 * @param entropy_computation_criterion Criterion for scoring a node/slice a la entropy
                 */
                complex_job_manager(std::vector<datapoint<bool> *> & datapoint_ptrs, const std::vector<horn_constraint<bool>> & horn_constraints, horn_solver<bool> & solver, \
                           unsigned int threshold, NodeSelection node_selection_criterion, EntropyComputation entropy_computation_criterion, ConjunctiveSetting conjunctive_setting)
                        : simple_job_manager(datapoint_ptrs, horn_constraints, solver, threshold)
                {
                        _node_selection_criterion = node_selection_criterion;
                        _entropy_computation_criterion = entropy_computation_criterion;
			_conjunctive_setting = conjunctive_setting;
                }
	                       

                /**
                 * Initializes _datapoint_ptrs_to_frac using purely the classified points in _datapoint_ptrs.
                 */
 		void initialize_datapoint_ptrs_to_frac()
		{
			                                        
			for (auto it = _datapoint_ptrs.begin(); it != _datapoint_ptrs.end(); it++)
			{
				if ((*it)->_is_classified)
				{
					if ((*it)->_classification)
					{
						_datapoint_ptrs_to_frac[*it] = 1.0;
					}
					else
					{
						_datapoint_ptrs_to_frac[*it] = 0.0;
					}	
				}
			}
			return;
		}

		/**
		 * Update _datapoint_ptrs_to_frac with randomly selected complete horn assignments
		 */							
		void update_datapoint_ptrs_to_frac_with_complete_horn_assignments()
		{
			std::map<datapoint<bool> *,double> _sum_of_datapoint_ptrs_to_frac;

			// Create a list of _datapoint_ptrs which are unclassified	
			std::vector<datapoint<bool> *> _unclassified_datapoint_ptrs_stable;
			for (auto it = _datapoint_ptrs.begin(); it != _datapoint_ptrs.end(); it++)
			{
				if (! (*it)->_is_classified)
				{
					_unclassified_datapoint_ptrs_stable.push_back(*it);
				}
			}

			//for(auto it = _unclassified_datapoint_ptrs_stable.begin(); it != _unclassified_datapoint_ptrs_stable.end(); it++)
			//{
			//	std::cout << *it << ": " << **it << std::endl;
			//}



			// Now generate complete horn assignments by randomly picking a datapoint from _unclassified_datapoint_ptrs and 
			// assigning it True/False followed by label propagation.
			int numberOfCompleteHornAssignments = 3;
			for (int i = 0; i < numberOfCompleteHornAssignments; i++)
			{
				std::vector<datapoint<bool> *> _unclassified_datapoint_ptrs_temp(_unclassified_datapoint_ptrs_stable);
                                auto positive_ptrs = std::unordered_set<datapoint<bool> *>();
				auto negative_ptrs = std::unordered_set<datapoint<bool> *>();
				while(_unclassified_datapoint_ptrs_temp.size() > 0)
				{
					unsigned int itemToAssignClassification = rand() % _unclassified_datapoint_ptrs_temp.size();
					if (rand() % 2 == 0)
					{
						positive_ptrs.insert(_unclassified_datapoint_ptrs_temp[itemToAssignClassification]);
						//std::cout << "chosen positive_ptr: " << _unclassified_datapoint_ptrs_temp[itemToAssignClassification] << std::endl;
					}
					else
					{
						negative_ptrs.insert(_unclassified_datapoint_ptrs_temp[itemToAssignClassification]);
						//std::cout << "chosen negative_ptr: " << _unclassified_datapoint_ptrs_temp[itemToAssignClassification] << std::endl;
					}
			
					//std::cout << "_unclassified_datapoint_ptrs_temp: ";
					//for (auto it = _unclassified_datapoint_ptrs_temp.begin(); it != _unclassified_datapoint_ptrs_temp.end(); ++it)
					//{
					//	std::cout << *it << " ";
					//}
					//std::cout << std::endl;

					//std::cout << "positive_ptrs: ";
				       	//for (auto it = positive_ptrs.begin(); it != positive_ptrs.end(); it++)
					//{
					//	std::cout << *it << " ";
					//}
					//std::cout << std::endl;
					//std::cout << "negative_ptrs: ";
				       	//for (auto it = negative_ptrs.begin(); it != negative_ptrs.end(); it++)
				       	//{
					//       	std::cout << *it << " ";
				       	//}
				       	//std::cout << std::endl;

					//horn_solver<bool> solver;

                                	auto ok = _horn_solver.solve(_datapoint_ptrs, _horn_constraints, positive_ptrs, negative_ptrs);

					//std::cout << "positive_ptrs after horn solve: ";
				       	//for (auto it = positive_ptrs.begin(); it != positive_ptrs.end(); it++)
					//{
					//	std::cout << *it << " ";
					//}
					//std::cout << std::endl;
					//std::cout << "negative_ptrs after the horn solve: ";
				       	//for (auto it = negative_ptrs.begin(); it != negative_ptrs.end(); it++)
				       	//{
					//       	std::cout << *it << " ";
				       	//}
				       	//std::cout << std::endl;

					// Horn constraints should be satisfiable after turning the single itemToAssignClassification to True/False.
					assert (ok);

					// Remove the items present in positive_ptrs and negative_ptrs from _unclassified_datapoint_ptrs_temp
					for (auto it = positive_ptrs.begin(); it != positive_ptrs.end(); it++)
					{
						// If *it is present in _unclassified_datapoint_ptrs_temp, then remove it.
						auto result_it = std::find(_unclassified_datapoint_ptrs_temp.begin(), _unclassified_datapoint_ptrs_temp.end(), *it);
					       	if (result_it != _unclassified_datapoint_ptrs_temp.end())
						{
							_unclassified_datapoint_ptrs_temp.erase(result_it);
						}						
					}
					for (auto it = negative_ptrs.begin(); it != negative_ptrs.end(); it++)
					{
						// If *it is present in _unclassified_datapoint_ptrs_temp, then remove it.
						auto result_it = std::find(_unclassified_datapoint_ptrs_temp.begin(), _unclassified_datapoint_ptrs_temp.end(), *it);
					       	if (result_it != _unclassified_datapoint_ptrs_temp.end())
						{
							_unclassified_datapoint_ptrs_temp.erase(result_it);
						}						
					}
				}
				std::map<datapoint<bool> *,double> _datapoint_ptrs_to_frac_temp(_datapoint_ptrs_to_frac);
				for(auto it = positive_ptrs.begin(); it != positive_ptrs.end(); it++)
				{
					_datapoint_ptrs_to_frac_temp[*it] = 1.0;
				}
				for(auto it = negative_ptrs.begin(); it != negative_ptrs.end(); it++)
				{
					_datapoint_ptrs_to_frac_temp[*it] = 0.0;
				}
				//std::cout << "size of the complete horn assignment is: " << _datapoint_ptrs_to_frac_temp.size() << std::endl;
				// Add _datapoint_ptrs_to_frac_temp to _sum_of_datapoint_ptrs_to_frac				
				for (auto it = _datapoint_ptrs_to_frac_temp.begin(); it != _datapoint_ptrs_to_frac_temp.end(); ++it)
				{
					_sum_of_datapoint_ptrs_to_frac[it->first] += it->second;
				}
			}
			// Divide _sum_of_datapoint_ptrs_to_frac by 5 and store it to _datapoint_ptrs_to_frac
			for (auto it = _sum_of_datapoint_ptrs_to_frac.begin(); it != _sum_of_datapoint_ptrs_to_frac.end(); ++it)
			{
				_sum_of_datapoint_ptrs_to_frac[it->first] = it->second / numberOfCompleteHornAssignments;
			}

			// Update _datapoint_ptrs_to_frac to _sum_of_datapoint_ptrs_to_frac
			_datapoint_ptrs_to_frac = _sum_of_datapoint_ptrs_to_frac;

			return;
		}


		/**
		 * Returns the next job. 
		 * If _node_selection_criterion is DEFAULT, calls the next_job() function of the super class
		 *
		 * @returns a unique pointer to the next job
		 */
		std::unique_ptr<abstract_job> next_job()
		{

			if (_is_first_split)
			{
				srand(time(NULL));
                                auto sl = _slices.front();
				_slices.pop_front();

				// Check if data points have exactly one categorical attribute
				if (_datapoint_ptrs[sl._left_index]->_categorical_data.size() != 1)
				{
					throw std::runtime_error("Learner expects exactly one categorical attribute");
				}
				_is_first_split = false;

				return std::unique_ptr<abstract_job> { std::make_unique<categorical_split_job>(sl, 0) };
			}	
			else
			{
				if (_node_selection_criterion == BFS || _node_selection_criterion == RANDOM || _node_selection_criterion == DFS)
				{
					auto slice_index = _node_selection_criterion == BFS ? 0 : _node_selection_criterion == DFS ? _slices.size() - 1 : rand() % _slices.size();
					auto it = _slices.begin();
					advance(it, slice_index);
		                	auto sl = *it;
					//std::cout << "Processing node " << slice_index << " out of total " << _slices.size() << " nodes" << std::endl;
					_slices.erase (it);
	                        	//std::cout << sl << std::endl;

					//
					// Determine what needs to be done (split or create leaf)
					//
					auto label = false; // label is unimportant (if is_leaf() returns false)
					auto positive_ptrs = std::unordered_set<datapoint<bool> *>();
					auto negative_ptrs = std::unordered_set<datapoint<bool> *>();

					//for(auto it = _datapoint_ptrs.begin(); it != _datapoint_ptrs.end(); it++)
					//{
					//	std::cout << "pos 2: " << *it << ": " << **it << std::endl;
					//}

					auto can_be_turned_into_leaf = is_leaf(sl, label, positive_ptrs, negative_ptrs);

					//for(auto it = _datapoint_ptrs.begin(); it != _datapoint_ptrs.end(); it++)
					//{
					//	std::cout << "pos 3: " << *it << ": " << **it << std::endl;
					//}

					// Slice can be turned into a leaf node
					if (can_be_turned_into_leaf)
					{
						return std::unique_ptr<abstract_job> {std::make_unique<leaf_creation_job>(sl, label, \
												std::move(positive_ptrs), std::move(negative_ptrs))};
					}
					// Slice needs to be split
					else
					{
						if (_entropy_computation_criterion == DEFAULT_ENTROPY || _entropy_computation_criterion == PENALTY)
						{
							return find_best_split(sl);
						}
						else if (_entropy_computation_criterion == HORN_ASSIGNMENTS)
						{

							//for(auto it = _datapoint_ptrs.begin(); it != _datapoint_ptrs.end(); it++)
							//{
							//	std::cout << "pos 4: " << *it << ": " << **it << std::endl;
							//}

							// Clear _datapoint_ptrs_to_frac from a previous iteration
							_datapoint_ptrs_to_frac.clear();
							
							//for(auto it = _datapoint_ptrs.begin(); it != _datapoint_ptrs.end(); it++)
							//{
							//	std::cout << "pos 5: " << *it << ": " << **it << std::endl;
							//}

							// Initialize the _datapoint_ptrs_to_frac map using the classified points in _datapoint_ptrs
							initialize_datapoint_ptrs_to_frac();

							//for(auto it = _datapoint_ptrs.begin(); it != _datapoint_ptrs.end(); it++)
							//{
							//	std::cout << "pos 6: " << *it << ": " << **it << std::endl;
							//}

							if (! unclassified_points_present(_datapoint_ptrs, sl._left_index, sl._right_index))
							{
								return find_best_split(sl);
							}	
							else
							{
								// Update _datapoint_ptrs_to_frac with randomly selected complete horn assignments
								update_datapoint_ptrs_to_frac_with_complete_horn_assignments();
								//for(auto it = _datapoint_ptrs.begin(); it != _datapoint_ptrs.end(); it++)
								//{
								//	std::cout << "pos 7: " << *it << ": " << **it << std::endl;
								//}

								return find_best_split(sl);
							}
						}
						else
						{
							// Control never reaches here!
							assert (false);
						}
					}					
				}
				else if (_node_selection_criterion == MAX_ENTROPY || _node_selection_criterion == MAX_WEIGHTED_ENTROPY)
				{

					if (_entropy_computation_criterion == HORN_ASSIGNMENTS)
					{
						_datapoint_ptrs_to_frac.clear();
						initialize_datapoint_ptrs_to_frac();
						update_datapoint_ptrs_to_frac_with_complete_horn_assignments();
					}

					
					float max_entropy = 0.0;
					unsigned int max_entropy_slice_index = 0;
					unsigned int cur_index = 0;
					for (auto it = _slices.begin(); it != _slices.end(); it++)
					{
						auto entropy_val = _node_selection_criterion == MAX_ENTROPY ? entropy(_datapoint_ptrs, it->_left_index, it->_right_index) :\
									    weighted_entropy(_datapoint_ptrs, it->_left_index, it->_right_index);
						if (entropy_val > max_entropy)
						{
							max_entropy = entropy_val;
							max_entropy_slice_index = cur_index;
						}
						cur_index++;
					}
					//std::cout << "entropy value of the node selected is: " << max_entropy << std::endl;	
					assert (max_entropy_slice_index >= 0 && max_entropy_slice_index < _slices.size());
	                                auto it = _slices.begin();
			                advance(it, max_entropy_slice_index);
					//std::cout << "Processing node " << max_entropy_slice_index << " out of total " << _slices.size() << " nodes" << std::endl;
					auto sl = *it;
					_slices.erase (it);
					//std::cout << sl << std::endl;

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
						return std::unique_ptr<abstract_job> {std::make_unique<leaf_creation_job>(sl, label, \
														       std::move(positive_ptrs), std::move(negative_ptrs))};
					}
					// Slice needs to be split
					else
					{
						return find_best_split(sl);
					}					


				}
				else if (_node_selection_criterion == MIN_ENTROPY || _node_selection_criterion == MIN_WEIGHTED_ENTROPY)
				{

					if (_entropy_computation_criterion == HORN_ASSIGNMENTS)
					{
						_datapoint_ptrs_to_frac.clear();
						initialize_datapoint_ptrs_to_frac();
						update_datapoint_ptrs_to_frac_with_complete_horn_assignments();
					}

					float min_entropy = _node_selection_criterion == MIN_ENTROPY ? 1.0 : 100000.0 ;
					unsigned int min_entropy_slice_index = 0;
					unsigned int cur_index = 0;
					for (auto it = _slices.begin(); it != _slices.end(); it++)
					{
                                               	auto entropy_val = _node_selection_criterion == MIN_ENTROPY ? entropy(_datapoint_ptrs, it->_left_index, it->_right_index) :\
									            weighted_entropy(_datapoint_ptrs, it->_left_index, it->_right_index);
						if (entropy_val < min_entropy)
						{
							min_entropy = entropy_val;
							min_entropy_slice_index = cur_index;
						}
						cur_index++;
					}	
					//std::cout << "entropy value of the node selected is: " << min_entropy << std::endl;
					assert (min_entropy_slice_index >= 0 && min_entropy_slice_index < _slices.size());
	                                auto it = _slices.begin();
			                advance(it, min_entropy_slice_index);
					auto sl = *it;
					//std::cout << "Processing node " << min_entropy_slice_index << " out of total " << _slices.size() << " nodes" << std::endl;
					_slices.erase (it);
					//std::cout << sl << std::endl;

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
						return std::unique_ptr<abstract_job> {std::make_unique<leaf_creation_job>(sl, label, \
														       std::move(positive_ptrs), std::move(negative_ptrs))};
					}
					// Slice needs to be split
					else
					{
						return find_best_split(sl);
					}					

				}
				else
				{
					// Control never reaches here!
					assert (false);
				}
			}
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
			if (_entropy_computation_criterion == HORN_ASSIGNMENTS)
			{
				double count_f = 0.0;
				double count_t = 0.0;
			
				for (std::size_t i = left_index; i <= right_index; ++i)
				{
					// assert that datapoint_ptrs[i] is present in the map _datapoint_ptrs_to_frac
					auto it = _datapoint_ptrs_to_frac.find(datapoint_ptrs[i]);
					assert (it != _datapoint_ptrs_to_frac.end());
					count_t += it->second;
				}
			
				double sum = right_index - left_index + 1;
				count_f = sum - count_t;

				//std::cout << "sum=" << sum << std::endl;
				double p_t = ((double)(count_t) / sum);
				//std::cout << "p_t=" << p_t << std::endl;
				double p_f = ((double)(count_f) / sum);
				//std::cout << "p_f=" << p_f << std::endl;
			
				double entropy_t = count_t == 0.0 ? 0 : p_t * log2(p_t);
				double entropy_f = count_f == 0.0 ? 0 : p_f * log2(p_f);
			
				return -(entropy_t + entropy_f);
	
			}
			else if (_entropy_computation_criterion == DEFAULT_ENTROPY || _entropy_computation_criterion == PENALTY)
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
			// _entropy_computation_criterion should be one of the two implemented criterions
			// Control should never reach here!
			assert (false);
			return 0.0;
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
			if (_entropy_computation_criterion == DEFAULT_ENTROPY || _entropy_computation_criterion == PENALTY)
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
			else if (_entropy_computation_criterion == HORN_ASSIGNMENTS)
			{
				return right_index - left_index + 1;
			}
			assert (false);
			return 0;
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
			if (_entropy_computation_criterion == HORN_ASSIGNMENTS)
			{
				return entropy(datapoint_ptrs, left_index, right_index) * (right_index - left_index + 1);
			}
			else if (_entropy_computation_criterion == DEFAULT_ENTROPY || _entropy_computation_criterion == PENALTY)
			{
				return entropy(datapoint_ptrs, left_index, right_index) * num_classified_points(_datapoint_ptrs, left_index, right_index); 
			}
			// _entropy_computation_criterion should be one of the two implemented criterions
			// Control should never reach here!
			assert (false);
			return 0.0;
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
			
			assert (sl._left_index <= sl._right_index && sl._right_index < _datapoint_ptrs.size());

                        // 0) Initialize variables
                        bool int_split_possible = false;
			bool non_zero_intrinsic_value_possible = false;
                        double best_int_gain_ratio = -1000000;
                        std::size_t best_int_attribute = 0;
                        int best_int_threshold = 0;
                        double best_int_gain_ratio_4_zero_iv = -1000000;
                        std::size_t best_int_attribute_4_zero_iv = 0;
                        int best_int_threshold_4_zero_iv = 0;

			// Variables to track the split if conjunctive splits are preferred	
			bool conj_int_split_possible = false;
			bool conj_non_zero_intrinsic_value_possible = false;
                        double best_conj_int_gain_ratio = -1000000;
                        std::size_t best_conj_int_attribute = 0;
                        int best_conj_int_threshold = 0;
                        double best_conj_int_gain_ratio_4_zero_iv = -1000000;
                        std::size_t best_conj_int_attribute_4_zero_iv = 0;
                        int best_conj_int_threshold_4_zero_iv = 0;

                        bool cat_split_possible = false;
                        double best_cat_gain_ratio = -1000000;
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
				double total_intrinsic_value = 0;
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

                                                double n1 = 1.0 * num_classified_points(_datapoint_ptrs, cur_left, cur_right);
                                                double n = 1.0 * num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index);
                                                total_intrinsic_value += (n1 == 0) ? 0.0 : -1.0 * (n1/n) * log2(n1/n);

						cur_left = cur_right + 1;
						cur_right = cur_left;
					}
				}
			
                                if (split_possible)
                                {
					double info_gain;
					if (num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index) == 0)
					{
						info_gain = entropy(_datapoint_ptrs, sl._left_index, sl._right_index);
					}
					else
					{
						info_gain = entropy(_datapoint_ptrs, sl._left_index, sl._right_index) - \
							    	total_weighted_entropy / num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index);

					}
					assert (total_intrinsic_value > 0.0);
                                        double gain_ratio = info_gain / total_intrinsic_value;
                                        // split is possible on the current "attribute"
                                        if (! cat_split_possible || gain_ratio > best_cat_gain_ratio)
                                        {
                                                cat_split_possible = true;
                                                best_cat_gain_ratio = gain_ratio;
                                                best_cat_attribute = attribute;
                                        }
                                }

			}
			
			//std::cout << "Now processing integer splits" << std::endl;
			//
			// Process integer attributes
			//
			for (std::size_t attribute = 0; attribute < _datapoint_ptrs[sl._left_index]->_int_data.size(); ++attribute)
			{
				int tries = 0;
                                double best_int_entropy_for_given_attribute = 1000000;
                                bool int_split_possible_for_given_attribute = false;
                                int best_int_split_index_for_given_attribute = 0;
                                double best_intrinsic_value_for_given_attribute = 0;

				double best_conj_int_entropy_for_given_attribute = 1000000;
                                bool conj_int_split_possible_for_given_attribute = false;
                                int best_conj_int_split_index_for_given_attribute = 0;
                                double best_conj_intrinsic_value_for_given_attribute = 0;

	
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
                                                tries++;

						// if cuts have been thresholded, check that a split at the current value of the numerical attribute is allowed
						if (! _are_numerical_cuts_thresholded || \
							       	((-1 * _threshold <= _datapoint_ptrs[cur]->_int_data[attribute]) && \
										       (_datapoint_ptrs[cur]->_int_data[attribute] <= _threshold)))
						{
							//std::cout << "considering attribute: " << attribute << " sl._left_index: " << cur << " cut: " << _datapoint_ptrs[cur]->_int_data[attribute] << std::endl;
							auto weighted_entropy_left = weighted_entropy(_datapoint_ptrs, sl._left_index, cur);
							auto weighted_entropy_right = weighted_entropy(_datapoint_ptrs, cur + 1, sl._right_index);
							auto total_weighted_entropy = weighted_entropy_left + weighted_entropy_right;
							double total_entropy; 
							if (num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index) == 0.0)
							{
								total_entropy = 0.0;
							}
							else
							{
								total_entropy = total_weighted_entropy / \
										     (double)num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index);
							}


							// Add a penalty based on the number of implications in the horn constraints that are cut by the current split.
							if (_entropy_computation_criterion == PENALTY)
							{
								// number of implications in the horn constraints cut by the current split.
								int left2right = 0;
								int right2left = 0;
								penalty(sl, sl._left_index, cur, sl._right_index, &left2right, &right2left);		
								double nleft = num_points_with_classification(_datapoint_ptrs, sl._left_index, cur, false);
								double pleft = num_points_with_classification(_datapoint_ptrs, sl._left_index, cur, true);
								double nright = num_points_with_classification(_datapoint_ptrs, cur+1, sl._right_index, false);
								double pright = num_points_with_classification(_datapoint_ptrs, cur+1, sl._right_index, true);
								double total_classified_points = nleft + pleft + nright + pright;

								nleft = nleft == 0 ? 0 : nleft / (nleft + pleft);
								pleft = pleft == 0 ? 0 : pleft / (nleft + pleft);
								nright = nright == 0 ? 0 : nright / (nright + pright);
								pright = pright == 0 ? 0 : pright / (nright + pright);

								double penaltyVal = (1 - nleft * pright) * left2right + (1 - nright * pleft) * right2left;
								penaltyVal = 2 * penaltyVal / (2 * (left2right + right2left) + total_classified_points);
								total_entropy += penaltyVal;
							}


							// If the learner prefers conjunctive splits
							if (_conjunctive_setting == PREFERENCEFORCONJUNCTS)
							{
								// Check if a conjunctive split is possible
								if (! positive_points_present(_datapoint_ptrs, sl._left_index, cur) || \
										! positive_points_present(_datapoint_ptrs, cur + 1, sl._right_index))
								{
									// One of the sub node consists purely of negative or unclassified points.
									// Consider this as a prospective candidate for a conjunctive split
			
									if (! conj_int_split_possible_for_given_attribute || \
											total_entropy < best_conj_int_entropy_for_given_attribute)
									{
										//std::cout << "updated the entropy; split is now definitely possible" << std::endl;	
										conj_int_split_possible_for_given_attribute = true;
							
										best_conj_int_entropy_for_given_attribute = total_entropy;
										best_conj_int_split_index_for_given_attribute = cur;

										// computation of the intrinsic value of the attribute
                		                                                double n1 = 1.0 * num_classified_points(_datapoint_ptrs, sl._left_index, cur);
                                		                                double n2 = 1.0 * num_classified_points(_datapoint_ptrs, cur + 1, sl._right_index);
                                                		                double n = n1 + n2;
                                                                		best_conj_intrinsic_value_for_given_attribute = \
														(n1 == 0.0 ? 0.0 : -1.0 * (n1/n) * log2(n1/n)) + \
													        (n2 == 0.0 ? 0.0 : - 1.0 * (n2/n) * log2(n2/n));

									}
								}
							}
	
							if (!int_split_possible_for_given_attribute || total_entropy < best_int_entropy_for_given_attribute)
                                                        {
                                                                //std::cout << "updated the entropy; split is now definitely possible" << std::endl;    
                                                                int_split_possible_for_given_attribute = true;

                                                                best_int_entropy_for_given_attribute = total_entropy;
                                                                best_int_split_index_for_given_attribute = cur;

                                                                // computation of the intrinsic value of the attribute
                                                                double n1 = 1.0 * num_classified_points(_datapoint_ptrs, sl._left_index, cur);
                                                                double n2 = 1.0 * num_classified_points(_datapoint_ptrs, cur + 1, sl._right_index);
                                                                double n = n1 + n2;
                                                                best_intrinsic_value_for_given_attribute = (n1 == 0.0 ? 0.0 : -1.0 * (n1/n) * log2(n1/n)) + \
													       (n2 == 0.0 ? 0.0 : - 1.0 * (n2/n) * log2(n2/n));

                                                        }

						}
							
						++cur;			
					}
					
				}

				if (_conjunctive_setting == PREFERENCEFORCONJUNCTS && conj_int_split_possible_for_given_attribute)
                                {
                                        // We have found the best split threshold for the given attribute
                                        // Now compute the information gain to optimize across different attributes
                                        double best_info_gain_for_attribute;
					best_info_gain_for_attribute = entropy(_datapoint_ptrs, sl._left_index, sl._right_index) - best_conj_int_entropy_for_given_attribute;

                                        double interval = (_datapoint_ptrs[sl._right_index]->_int_data[attribute] - _datapoint_ptrs[sl._left_index]->_int_data[attribute]) / \
                                                               (_datapoint_ptrs[best_conj_int_split_index_for_given_attribute+1]->_int_data[attribute] - \
                                                                          _datapoint_ptrs[best_conj_int_split_index_for_given_attribute]->_int_data[attribute]);

					assert (num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index) > 0);
                                        double threshCost = ( interval < (double)tries ? log2(interval) : log2(tries) ) / \
                                                                 num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index);

                                        best_info_gain_for_attribute -= threshCost;
					if (best_conj_intrinsic_value_for_given_attribute > 0.0)
					{
						conj_non_zero_intrinsic_value_possible = true;
	                                        double best_gain_ratio_for_given_attribute = best_info_gain_for_attribute / best_conj_intrinsic_value_for_given_attribute;

                                        	if (! conj_int_split_possible || (best_gain_ratio_for_given_attribute > best_conj_int_gain_ratio) || \
        	                                    (best_gain_ratio_for_given_attribute == best_conj_int_gain_ratio && \
                	                              std::abs(_datapoint_ptrs[best_conj_int_split_index_for_given_attribute]->_int_data[attribute]) < std::abs(best_conj_int_threshold)))
                        	                {
                                	                // if this is the first attribute for which a split is possible then
                                        	        // initialize all variables: best_int_gain_ratio, best_int_attribute, best_int_threshold
                                                	conj_int_split_possible = true;
	                                                best_conj_int_gain_ratio = best_gain_ratio_for_given_attribute;
        	                                        best_conj_int_attribute = attribute;
                	                                best_conj_int_threshold = _datapoint_ptrs[best_conj_int_split_index_for_given_attribute]->_int_data[attribute];
                        	                }
					}
					else
					{
						if (! conj_non_zero_intrinsic_value_possible)
						{
		                                        double best_gain_ratio_for_given_attribute = best_info_gain_for_attribute;

               	                         		if (! conj_int_split_possible || (best_gain_ratio_for_given_attribute > best_conj_int_gain_ratio_4_zero_iv) || \
	        	                                    (best_gain_ratio_for_given_attribute == best_conj_int_gain_ratio_4_zero_iv && \
        	        	                              std::abs(_datapoint_ptrs[best_conj_int_split_index_for_given_attribute]->_int_data[attribute]) < std::abs(best_conj_int_threshold_4_zero_iv)))
                	        	                {
                        	        	                // if this is the first attribute for which a split is possible then
                                	        	        // initialize all variables: best_int_gain_ratio, best_int_attribute, best_int_threshold
                                        	        	conj_int_split_possible = true;
	                                        	        best_conj_int_gain_ratio_4_zero_iv = best_gain_ratio_for_given_attribute;
        	                                        	best_conj_int_attribute_4_zero_iv = attribute;
	                	                                best_conj_int_threshold_4_zero_iv = _datapoint_ptrs[best_conj_int_split_index_for_given_attribute]->_int_data[attribute];
        	                	                }
							
						}
					}
                                }


                                if (int_split_possible_for_given_attribute)
                                {
                                        // We have found the best split threshold for the given attribute
                                        // Now compute the information gain to optimize across different attributes
                                        double best_info_gain_for_attribute;
					best_info_gain_for_attribute = entropy(_datapoint_ptrs, sl._left_index, sl._right_index) - best_int_entropy_for_given_attribute;
                                        
					double interval = (_datapoint_ptrs[sl._right_index]->_int_data[attribute] - _datapoint_ptrs[sl._left_index]->_int_data[attribute]) / \
                                                               (_datapoint_ptrs[best_int_split_index_for_given_attribute+1]->_int_data[attribute] - \
                                                                          _datapoint_ptrs[best_int_split_index_for_given_attribute]->_int_data[attribute]);

					assert (num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index) > 0);
                                        double threshCost = ( interval < (double)tries ? log2(interval) : log2(tries) ) / \
                                                                 num_classified_points(_datapoint_ptrs, sl._left_index, sl._right_index);

                                        best_info_gain_for_attribute -= threshCost;
					if (best_intrinsic_value_for_given_attribute > 0.0)
					{
						non_zero_intrinsic_value_possible = true;
						double best_gain_ratio_for_given_attribute = best_info_gain_for_attribute / best_intrinsic_value_for_given_attribute;

	                                        if (! int_split_possible || (best_gain_ratio_for_given_attribute > best_int_gain_ratio) || \
        	                                    (best_gain_ratio_for_given_attribute == best_int_gain_ratio && \
                	                              std::abs(_datapoint_ptrs[best_int_split_index_for_given_attribute]->_int_data[attribute]) < std::abs(best_int_threshold)))
                        	                {
                                	                // if this is the first attribute for which a split is possible then
                                        	        // initialize all variables: best_int_gain_ratio, best_int_attribute, best_int_threshold
                                                	int_split_possible = true;
	                                                best_int_gain_ratio = best_gain_ratio_for_given_attribute;
        	                                        best_int_attribute = attribute;
                	                                best_int_threshold = _datapoint_ptrs[best_int_split_index_for_given_attribute]->_int_data[attribute];
                        	                }
					}
					else
					{
						if (! non_zero_intrinsic_value_possible)
						{
							double best_gain_ratio_for_given_attribute = best_info_gain_for_attribute;
								
                                        		if (! int_split_possible || (best_gain_ratio_for_given_attribute > best_int_gain_ratio_4_zero_iv) || \
		                                          (best_gain_ratio_for_given_attribute == best_int_gain_ratio_4_zero_iv && \
                		                          std::abs(_datapoint_ptrs[best_int_split_index_for_given_attribute]->_int_data[attribute]) < std::abs(best_int_threshold_4_zero_iv)))
                                        		{
		                                                // if this is the first attribute for which a split is possible then
                		                                // initialize all variables: best_int_gain_ratio, best_int_attribute, best_int_threshold
                                		                int_split_possible = true;
                                                		best_int_gain_ratio_4_zero_iv = best_gain_ratio_for_given_attribute;
		                                                best_int_attribute_4_zero_iv = attribute;
                		                                best_int_threshold_4_zero_iv = _datapoint_ptrs[best_int_split_index_for_given_attribute]->_int_data[attribute];
                                		        }
						}
					}
                                }

			
			}

			
			//
			// Return best split
			//
			
			if (!int_split_possible && !cat_split_possible)
			{
				assert (!conj_int_split_possible);
				throw split_not_possible_error("No split possible!");
			}
			
			else if (int_split_possible && !cat_split_possible)
			{
				if (_conjunctive_setting == PREFERENCEFORCONJUNCTS && conj_int_split_possible)
				{
					if (! conj_non_zero_intrinsic_value_possible)
					{
						best_conj_int_gain_ratio = best_conj_int_gain_ratio_4_zero_iv;
						best_conj_int_attribute = best_conj_int_attribute_4_zero_iv;
						best_conj_int_threshold = best_conj_int_threshold_4_zero_iv;
					}
					return std::unique_ptr<abstract_job> { std::make_unique<int_split_job>(sl, best_conj_int_attribute, best_conj_int_threshold) };
				}
				else
				{
					if (! non_zero_intrinsic_value_possible)
					{
                                        	best_int_gain_ratio = best_int_gain_ratio_4_zero_iv;				                  
						best_int_attribute = best_int_attribute_4_zero_iv;
						best_int_threshold = best_int_threshold_4_zero_iv;
					}
					return std::unique_ptr<abstract_job> { std::make_unique<int_split_job>(sl, best_int_attribute, best_int_threshold) };
				}
			}
			
			else if (!int_split_possible && cat_split_possible)
			{
				assert (!conj_int_split_possible);
				return std::unique_ptr<abstract_job> { std::make_unique<categorical_split_job>(sl, best_cat_attribute) };
			}
			
			else
			{
				// If conjunctive splits are preferred, overwrite the best int split variabls with those corresponding to conjunctive splits.
				if (_conjunctive_setting == PREFERENCEFORCONJUNCTS && conj_int_split_possible)
				{
					if (! conj_non_zero_intrinsic_value_possible)
					{
						best_int_gain_ratio = best_conj_int_gain_ratio_4_zero_iv;
						best_int_attribute = best_conj_int_attribute_4_zero_iv;
						best_int_threshold = best_conj_int_threshold_4_zero_iv;
					}
					else
					{
						best_int_gain_ratio = best_conj_int_gain_ratio;
						best_int_attribute = best_conj_int_attribute;
						best_int_threshold = best_conj_int_threshold;
					}
				}
				else
				{
					if (! non_zero_intrinsic_value_possible)
					{
                                	       	best_int_gain_ratio = best_int_gain_ratio_4_zero_iv;				                  
						best_int_attribute = best_int_attribute_4_zero_iv;
						best_int_threshold = best_int_threshold_4_zero_iv;
					}
				}
				if (best_int_gain_ratio <= best_cat_gain_ratio)
				{
					return std::unique_ptr<abstract_job> { std::make_unique<int_split_job>(sl, best_int_attribute, best_int_threshold) };
				}
				else
				{
					return std::unique_ptr<abstract_job> { std::make_unique<categorical_split_job>(sl, best_cat_attribute) };
				}

			}
			
		}
		

	void penalty(const slice & sl, std::size_t left_index, std::size_t cur_index, std::size_t right_index, int* left2right, int* right2left)
	{
		int _left2right = 0;
		int _right2left = 0;
		for (const auto & horn_clause : _horn_constraints)
                {
			enum Position {out_of_scope, left, right};
			Position conclusion = out_of_scope;
			int num_premise_left = 0;
			int num_premise_right = 0;

			// for i ranging from left_index to cur, loop over premises and conclusion
                        for (std::size_t i = left_index; i <= cur_index; ++i)
                        {
				for (auto *const dp : horn_clause._premises)
				{
					if (dp == _datapoint_ptrs[i] && !_datapoint_ptrs[i]->_is_classified)
					{
						num_premise_left++;	
					}
				}
				if (_datapoint_ptrs[i] == horn_clause._conclusion && !_datapoint_ptrs[i]->_is_classified)
				{
					conclusion = left;
				}
			}

			// for i ranging from cur+1 to right_index, loop over premises and conclusion
                        for (std::size_t i = cur_index+1; i <= right_index; ++i)
                        {
				for (auto *const dp : horn_clause._premises)
				{
					if (dp == _datapoint_ptrs[i] && !_datapoint_ptrs[i]->_is_classified)
					{
						num_premise_right++;	
					}
				}
				if (_datapoint_ptrs[i] == horn_clause._conclusion && !_datapoint_ptrs[i]->_is_classified)
				{
					conclusion = right;
				}
			}
			if (conclusion == left)
			{
				_right2left += num_premise_right;
			}
			if (conclusion == right)
			{
				_left2right += num_premise_left;
			}

		}
		*right2left = _right2left;
		*left2right = _left2right;
		return;
	}




	};



}; // End namespace horn_verification

#endif
