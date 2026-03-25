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
#include <cassert>


using namespace wpds;

static Dictionary& get_dict()
{
    // The static dictionary used by WPDS
    static Dictionary thedict;

    // create key for WPDS_EPSILON. Removing the static
    // qualifier will cause major overhead
    static wpds_key_t epskey UNUSED_VAR = thedict.add_item( new string_src("*") );
    return thedict;
}

wpds_size_t num_keys(void)
{
  return get_dict().num_keys();
}

wpds_key_t create_key(key_source *x)
{
    return get_dict().add_item(x);
}

wpds_key_t str2key( const char* s ) 
{
    string_src *x = new string_src(s);
    return get_dict().add_item(x); 
}

wpds_key_t str2key( const std::string & s )
{
    return str2key( s.c_str() );
}

wpds_key_t new_str2key( const char* s ) 
{
    string_src *x = new string_src(s);
    return get_dict().add_new_item(x); 
}

wpds_key_t int2key(int i)
{
    int_src *item = new int_src(i);
    return get_dict().add_item(item);
}

std::ostream& printkey(wpds_key_t key, std::ostream& o)
{
    key_source *ks;
    ks = get_dict().retrieve_item(key);
    if (ks)
        return ks->print(o);
    else
        return o;
}

void showkey(wpds_key_t key, std::string &s)
{
    key_source *ks;
    ks = get_dict().retrieve_item(key);
    assert(ks);
    ks->show(s);
}

key_source *retrieve_item(wpds_key_t key)
{
    key_source *ks;
    ks = get_dict().retrieve_item(key);
    return ks;
}


void traverse_dict(void (*f)(key_source *))
{
    get_dict().traverse(f);
}

void clear_dict()
{
    get_dict().clear();
    // readd WPDS_EPSILON
    str2key( "*" );
}

/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
