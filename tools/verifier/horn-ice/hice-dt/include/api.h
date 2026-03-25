/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __API_H__
#define __API_H__

// C++ includes
#include <algorithm>  
#include <iostream>
#include <vector>

// Project includes
#include "../../hice-dt/include/api_helper.h"
#include "../../hice-dt/include/datapoint.h"


namespace horn_verification
{
	class api
	{

	private:
			bool use_bounds;

			bool do_horndini_prephase;

			attributes_metadata metadata;

			std::vector<std::pair<unsigned, unsigned>> intervals;

			mutable std::vector<horn_verification::datapoint<bool> *> datapoint_ptrs;

			std::vector<std::pair<std::set<unsigned>, std::set<unsigned>>> horn_indexes;

			std::unordered_map<unsigned, horn_verification::datapoint<bool>> identifier_to_datapoint;

	public:
			void reserve_datapoint_ptrs (unsigned number_of_datapoints) {

				datapoint_ptrs.reserve(number_of_datapoints);
			}

			void configure_learner (bool _do_horndini_prephase, bool _use_bounds) {

				use_bounds = _use_bounds;

				do_horndini_prephase = _do_horndini_prephase;
			}
			
			void add_intervals(unsigned left, unsigned right) {

				intervals.push_back(std::pair<unsigned, unsigned>(left, right));
			}

			void add_integer_attribute(const std::string & name) {

				metadata.add_int_attribute(name);
			}
			
			void add_categorical_attribute(const std::string &name, const std::size_t &number_of_categories) {

				metadata.add_categorical_attribute(name, number_of_categories);
			}

			void add_datapoints(datapoint<bool> &datapoint) const {

				datapoint_ptrs.emplace (datapoint_ptrs.begin() + datapoint._identifier, &datapoint);
			}

			unsigned index_of_datapoint_ptrs() const {

				if (datapoint_ptrs.cbegin() == datapoint_ptrs.cend()) {
	
					return	0;
						
				} else {

			 		return	(datapoint_ptrs.back()->_identifier + 1);
				}
			}

		void add_horn_constraints(const horn_constraint<bool> &horn_constraint) {

			std::set<unsigned> premises;

			std::set<unsigned> consequence;

			for(auto *itrator: horn_constraint._premises) {

				premises.insert(itrator->_identifier);
			}

			if (horn_constraint._conclusion != NULL) {

				consequence.insert(horn_constraint._conclusion->_identifier);
			}

			horn_indexes.push_back(std::make_pair(premises, consequence));
		}

			
		decision_tree learn_decision_tree() {

			std::vector<datapoint<bool>> datapoints_copy;

			datapoints_copy.reserve(datapoint_ptrs.size());

			for (unsigned i = 0; i < datapoint_ptrs.size(); ++i) {

				datapoints_copy.push_back(*datapoint_ptrs[i]);

				//assert (datapoint_ptrs[i]->_identifier == i);
			}

			horn_verification::api_helper learner_obj(metadata, datapoints_copy, horn_indexes, intervals);

			return learner_obj.learn_decision_tree(do_horndini_prephase, use_bounds);
		}

	};
} // namespace horn_verification

#endif
