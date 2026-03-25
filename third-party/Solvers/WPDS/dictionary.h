//////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2005
//   Nicholas Kidd  <kidd@cs.wisc.edu>
//   Thomas Reps    <reps@cs.wisc.edu>
//   David Melski   <melski@grammatech.com>
//   Akash Lal      <akash@cs.wisc.edu>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//   1. Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//
//   2. Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in
//      the documentation and/or other materials provided with the
//      distribution.
//
//   3. Neither the name of the University of Wisconsin, Madison,
//      nor GrammaTech, Inc., nor the names of the copyright holders
//      and contributors may be used to endorse or promote
//      products derived from this software without specific prior
//      written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//////////////////////////////////////////////////////////////////////////////

#ifndef wpds_DICTIONARY_H_
#define wpds_DICTIONARY_H_ 1

#include <vector>
#include "common.h"
#include "HashMap.h"
#include "key_source.h"

namespace wpds
{

    class Dictionary
    {

        public:     // Constructor/DE

            /*!
             * Create dictionary reserving buckets space for the map.
             *
             * @param buckets initializes the internal HashMap size
             */
            Dictionary( wpds_size_t buckets=6151 );

            /*!
             * Destructor
             *
             * Clears the dictionary and reclaims all memory associated
             * with key sources.
             */
            ~Dictionary();

        public:     // methods

            /*!
             * Adds a key_source to the dictionary. Returns the unique key for
             * that key_source. add_item claims ownership of the pointer and
             * associated memory passed to it.
             */
            wpds_key_t add_item( key_source * s);

            /*!
             * add_new_item avoids the HashMap lookup to see if  key_source* s
             * already exists. Calling add_new_item with a key_source already
             * in the Dictionary can have unwanted results. add_new_item
             * claims ownership of the memory passed to it.
             */
            wpds_key_t add_new_item( key_source * s );

            /*!
             * Call function f on each key_source in the Dictionary.
             *
             * TODO : why are we using C-style function pointers?
             */
            void traverse(void (*f)(key_source *));

            /*!
             * Retrieve the key_source associated with parameter key
             *
             * @param key the unique key for desired key_source
             * @return key_source* associated with the key. Null if no such
             * key exists
             */
            key_source *retrieve_item( wpds_key_t );

            // Does not touch memory of s
            bool exists(key_source * s);

            /*!
             * Delete all key_sources from the Dictionary and invalidate all
             * keys
             */
            void clear();

            /*!
             * @deprecated use num_keys()
             */
            wpds_size_t idx() const { return values.size(); }

            /*!
             * @return the number of keys (items) in the dictionary
             */
            wpds_size_t num_keys() const { return values.size(); }

        private:    // typedef
            typedef HashMap< const key_source *,wpds_key_t > map_type;
            typedef std::vector< key_source * > vector_type;

        private:    // vars
            // Maps a hash to a set of keys
            map_type keys;
            vector_type values;

    }; // class Dictionary

} // namespace wpds

#endif  // wpds_DICTIONARY_H_

/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
