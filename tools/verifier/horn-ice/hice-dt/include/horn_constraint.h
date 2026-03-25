/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef HORN_CONSTRAINT_H_
#define HORN_CONSTRAINT_H_

// C++ includes
#include <ostream>
#include <vector>

// Project includes
#include "datapoint.h"


namespace horn_verification {

	/// datapoint class
	template <class T> class datapoint;
	/**
	 * This class represents a horn constraint.
	 *
	 * @tparam T Type of a horn constraint.
	 *
	 * @author Ezudheen P
	 * @version 1.0
	 */
	template <class T> class horn_constraint {
		public:
			/// Indicate whether the given horn constraint is satisfiable or unsatisfiable, default value is unsatisfiable.
			bool _satisfiable;

			/// Store the conclusion of a horn constraint.
			datapoint<T> *_conclusion;

			/// Keep the number of premises of a horn constraint.
			unsigned _size_of_premises;

			/// Store the premises of the horn constraint.
			std::vector <datapoint<T> *> _premises;

			horn_constraint();
			virtual ~horn_constraint();

			/**
			 * Find the common markings of premises and insert the common markings to list_of_marking of conclusion.
			 * If the markings of conclusion get incremented then compute markings of all horn clauses depends on conclusion.
			 * This is a costlier operation, will modify using bitvectors.
			 */
			void compute_marking() const;

			/**
			 * Constructor.
			 * @param premises
			 * @param conclusion
			 */
			horn_constraint(const std::vector <datapoint<T> *> &premises, datapoint<T>  *conclusion);

			/**
			 * Constructor.
			 * @param premises
			 * @param conclusion
			 * @param satisfiable
			 */
			horn_constraint(const std::vector <datapoint<T> *> &premises, datapoint<T>  *conclusion, bool satisfiable);
			
			
			/**
			 * Writes a textual representation of a Horn constraint to an output stream.
			 *
			 * @param out The output stream to write to
			 * @param c The horn constraint to write
			 *
			 * @return a reference to the output stream given as argument
			 */
			friend std::ostream & operator<<(std::ostream & out, const horn_constraint<T> & c)
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
			
	};

} // namespace horn_verification

#endif /* HORN_CONSTRAINT_H_ */
