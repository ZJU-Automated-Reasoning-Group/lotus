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
#include "dictionary.h"
#include "key_source.h"
#include "keys.h"

#include <iostream>
#include <stdio.h>

namespace wpds {

    Dictionary::Dictionary( wpds_size_t buckets ) :
        keys( map_type(buckets) ) 
    {
        values.reserve( 250000 );// init values to some large space
    }

    Dictionary::~Dictionary()
    {
        clear();
    }

    bool Dictionary::exists(key_source *s)
    {
        if(s == NULL)
            return false;
        map_type::iterator it = keys.find(s);
        return (it != keys.end());
    }

    wpds_key_t Dictionary::add_item( key_source *s )
    {
        // map_type::value_type is a pair< Key,Data >
        // iterator iterates through values, therefore
        // it->first is Key and it->second is Data
        map_type::iterator it = keys.find( s );
        wpds_key_t key;
        if( it != keys.end() ) {
            key = it->second;
            delete s;
        } else
            key = add_new_item( s );
        return key;
    }
    
    wpds_key_t Dictionary::add_new_item( key_source * s )
    {
        wpds_key_t key = values.size();
        keys.insert( map_type::value_type(s,key) );
        values.push_back( s );
        return key;
    }

    void Dictionary::clear()
    {
        map_type tmpMap;
        // HashMap::operator= will release the array of buckets
        // reinitializing it to a small size
        keys = tmpMap;
        vector_type::iterator it = values.begin();
        vector_type::iterator itEND = values.end();
        for( ; it != itEND ; it++ )
        {
            key_source *ks = *it;
            delete ks;
            *it = 0;
        }
        values.clear();
        {
            vector_type VTMP;
            // erase the array associated with values when
            // VTMP goes out of scope
            VTMP.swap(values);
        }
    }

    key_source *Dictionary::retrieve_item(wpds_key_t key)
    {
        if (key >= values.size())
            return NULL;
        else
            return values[key];
    }

    void Dictionary::traverse(void (*f)(key_source *))
    {
        vector_type::iterator iter;
        for (iter = values.begin() ; iter != values.end() ; iter++)
            (f)(*iter);
    }

} // namespace

/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
