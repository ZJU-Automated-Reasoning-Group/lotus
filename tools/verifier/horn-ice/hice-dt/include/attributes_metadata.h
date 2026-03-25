/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __ATTRIBUTES_METADATA_H__
#define __ATTRIBUTES_METADATA_H__

// C++ includes
#include <algorithm>
#include <ostream>
#include <string>
#include <vector>

namespace horn_verification
{

	/**
	 * This class represents meta data of attributes. Especially, the following information are
	 * stored:
	 * <ul>
	 *   <li> <em>Categorical attributes:</em> name and number of categories</li>
	 *   <li> <em>Integer attributes:</em> name</li>
	 * </ul>
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class attributes_metadata
	{
		
		/// The names of categorical attributes
		std::vector<std::string> _categorical_names;
		
		/// Number of categories per categorical attribute
		std::vector<std::size_t> _number_of_categories;
		
		/// The names of integer attributes
		std::vector<std::string> _int_names;


	public:
	
		/**
		 * Adds a new categorical attribute to the end of the list of categorical attributes.
		 *
		 * @param name The name of the attribute
		 * @param number_of_categories The number of different categories of this attribute
		 */
		void add_categorical_attribute(const std::string & name, std::size_t number_of_categories)
		{
			_categorical_names.push_back(name);
			_number_of_categories.push_back(number_of_categories);
		}
		
	
		/**
		 * Adds a new categorical attribute to the end of the list of categorical attributes.
		 *
		 * @param name The name of the attribute
		 * @param number_of_categories The number of different categories of this attribute
		 */
		void add_categorical_attribute(std::string && name, std::size_t number_of_categories)
		{
			_categorical_names.push_back(std::move(name));
			_number_of_categories.push_back(number_of_categories);
		}
		
		
		/**
		 * Adds a new integer attribute to the end of the list of integer attributes.
		 *
		 * @param name The name of the attribute
		 */
		void add_int_attribute(const std::string & name)
		{
			_int_names.push_back(name);
		}
		
		
		/**
		 * Adds a new integer attribute to the end of the list of integer attributes.
		 *
		 * @param name The name of the attribute
		 */
		void add_int_attribute(std::string && name)
		{
			_int_names.push_back(std::move(name));
		}
		
		
		/**
		 * Returns the names of the categorical attributes.
		 *
		 * @returns the names of the categorical attributes
		 */
		inline const std::vector<std::string> & categorical_names() const
		{
			return _categorical_names;
		}
		
		
		/**
		 * Returns the number of categories per categorical attribute.
		 *
		 * @returns the number of categories per categorical attribute
		 */
		inline const std::vector<std::size_t> & number_of_categories() const
		{
			return _number_of_categories;
		}
		
		
		/**
		 * Returns the names of the categorical attributes.
		 *
		 * @returns the names of the categorical attributes
		 */
		inline const std::vector<std::string> & int_names() const
		{
			return _int_names;
		}
		
		
		/**
		 * Writes a textual representation to an output stream.
		 *
		 * @param out The output stream to write to
		 * @param data The attributes to display
		 *
		 * @returns the output stream
		 */
		friend std::ostream & operator<<(std::ostream & out, const attributes_metadata & data)
		{
			
			out << "Categorical attributes: ";
			for (std::size_t i = 0; i < data._categorical_names.size(); ++i)
			{
				out << (i > 0 ? ", " : "") << data._categorical_names[i] << " (" << data._number_of_categories[i] << ")";
			}
			
			out << '\n' << "Integer attributes: ";
			for (std::size_t i = 0; i < data._int_names.size(); ++i)
			{
				out << (i > 0 ? ", " : "") << data._int_names[i];
			}
			
			return out;
			
		}
		
	};

}; // End namespace horn_verification

#endif
