/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __CHCTEACHER_PROBLEM_H__
#define __CHCTEACHER_PROBLEM_H__

// C++ includes
#include <ostream>
#include <unordered_map>
#include <vector>

// Z3 includes
#include "z3++.h"


namespace chc_teacher
{
	
	class problem
	{
		
	public:
		
		/// The relations to synthesize
		decl_set relations;
		
		/// The constrained Horn clauses
		std::vector<constrainted_horn_clause> chcs;
		
		
		problem(decl_set && relations, std::vector<constrainted_horn_clause> && chcs)
			: relations(std::move(relations)), chcs(std::move(chcs))
		{
			// Nothing
		}

		
		friend std::ostream & operator<<(std::ostream & out, const problem & p)
		{

			// Output uninterpreted predicates that need to be synthesized
			out << "---------- Relations to synthesize ----------" << '\n';
			for (const auto & r : p.relations)
			{
				out << r << '\n';
			}
			
		
			// Output CHCs
			out << "---------- CHCs ----------" << '\n';
			for (const auto & chc : p.chcs)
			{
				out << chc << '\n';
			}
			
			return out;
		
		}
		
	};

}; // End namespace chc_teacher

#endif
