/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __DATAPOINT_H__
#define __DATAPOINT_H__

// C++ includes
#include <ostream>
#include <string>
#include <unordered_set>
#include <vector>

// Project includes
#include "horn_constraint.h"


namespace horn_verification
{
	template <class T> class horn_constraint;
	template <class T>
	class datapoint
	{
	
	public:
	
		/// The data of the categorical attributes
		std::vector<unsigned int> _categorical_data;
		
		/// The data of the integer attributes
		std::vector<int> _int_data;
	
		/// Flag for detecting whether the datapoint is already classified
		mutable bool _is_classified;

		/// Used to classify the datapoint as positive or negative
		mutable T _classification;

		/**
		* Creates a new unlabeled data point.
		*/
		datapoint()
		{
			_is_classified = false;
			_identifier = 0;
		}
	
	
		/**
		 * Creates a new data point with the given label.
		 *
		 * @param classification The label of the data point
		 * @param is_classified Indicates whether the data point considered to
		 *                      be is labaled or not
		 */
		datapoint(const T & classification, bool is_classified = true) : _is_classified(is_classified), _classification(classification)
		{
			_identifier = 0;
			// Nothing
		}

		/**
		 * Writes a textual representation of a data point to an output stream.
		 *
		 * @param out A reference to the output stream to write to
		 * @param dp The data point
		 *
		 * @return the given reference to the output stream
		 */
		friend std::ostream & operator<<(std::ostream & out, const datapoint<T> & dp)
		{
			
			out << "[";

			// Categorical attributes
			for (auto it = dp._categorical_data.cbegin(); it != dp._categorical_data.cend(); ++it)
			{
				out << (it == dp._categorical_data.cbegin() ? "" : ", ") << *it;
			}
			if (dp._categorical_data.size() > 0)
			{
				out << ", " ;
			}

			// Int attributes
			for (auto it = dp._int_data.cbegin(); it != dp._int_data.cend(); ++it)
			{
				out << (it == dp._int_data.cbegin() ? "" : ", ") << *it;
			}
			
			out << "] -> " << (dp._is_classified ? std::to_string(dp._classification) : "?");
			
			
			return out;
			
		}

		/// Identifier of a datapoint.
		unsigned _identifier;

		/// Container for the weak markings of a datapoint.
		mutable std::unordered_set <datapoint<T> *> _list_of_marking;

		/// Container for the horn constraints in which the datapoint is present.
		mutable std::unordered_set <horn_constraint<T> *> _list_of_horn_constraints;

		/**
		* Set the classification of a datapoint.
		* @param classification
		*/
		bool set_classification(T classification);

		/**
		* Remove all satisfied horn constraints.
		*/
		void remove_satisfied_horn_clauses();

		/**
		* Constructor.
		* @param identifier
		*/
		datapoint(unsigned identifier);

		/**
		* Copy constructor.
		* @param classification
		* @param is_classified
		* @param identifier
		*/
		datapoint(T classification, bool is_classified, unsigned identifier);


		/**
		* Copy constructor.
		* @param classification
		* @param is_classified
		* @param list_of_marking
		* @param list_of_horn_constraints
		* @param identifier
		*/
		datapoint(T classification, bool is_classified, std::unordered_set <datapoint<T> *> list_of_marking, std::unordered_set <horn_constraint<T> *> list_of_horn_constraints, unsigned identifier);


		/**
		 * Checks whether this data point is distinguishable from the other datapoint, given the threshold
		 *
		 * @param other The other data point
		 * @param threshold Used to determine distinguishability
		 *
		 * @return whether this data point is distinguishable from the other data point
		 */
		bool is_distinguishable(const datapoint<T> & other, unsigned int threshold);

	};


	/**
	 * This class implements a hash function and an equality test for data points.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class datapoint_hasher
	{
		
	public: 
	
		
		/**
		 * Computes the hash of a data point.
		 *
		 * @param dp The data point
		 *
		 * @returns the hash of the given data point
		 */
		std::size_t operator()(const datapoint<bool> & dp) const;
		
		
		/**
		 * Computes the hash of a data point.
		 *
		 * @param dp The data point
		 *
		 * @returns the hash of the given data point
		 */
		std::size_t operator()(const datapoint<bool> * dp) const;
		
		
		/**
		 * Checks whether two data points represent the same data, ignoring their label.
		 *
		 * @param x The first data point
		 * @param y The second data point
		 *
		 * @return whether the given data points are equal
		 */
		bool operator()(const datapoint<bool> & x, const datapoint<bool> & y) const;
		
		
		/**
		 * Checks whether two data points represent the same data, ignoring their label.
		 *
		 * @param x The first data point
		 * @param y The second data point
		 *
		 * @return whether the given data points are equal
		 */
		bool operator()(const datapoint<bool> * x, const datapoint<bool> * y) const;


	};
	
} // namespace horn_verification

#endif
