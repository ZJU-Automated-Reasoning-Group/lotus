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
/*
 * $Id: KeyPair.h,v 1.5 2005-09-06 16:11:35 radu Exp $
 */
#ifndef WPDS_UTIL_KEY_PAIR_H_
#define WPDS_UTIL_KEY_PAIR_H_
#include <utility>  // std::pair
#include "common.h"
#include "hm_hash.h"
#include "Triple.h"

#define combine_wpds_keys( k1,k2 ) ((k1) + 997*(k2))
//#define combine_wpds_keys( k1,k2 ) (((k1) << sizeof(wpds_key_t) <<2)  | (k2))

namespace wpds {

    namespace util {

        typedef std::pair< wpds_key_t,wpds_key_t > KeyPair;

        struct HashKeyPair
        {

            wpds::hm_hash< wpds_size_t > hasher;

            const wpds_size_t operator()( const KeyPair& kp ) const
            {
                return hasher( combine_wpds_keys( kp.first,kp.second ) );
            }

        };

        struct EqualKeyPair
        {

            const wpds_size_t operator()( const KeyPair& lhs,const KeyPair& rhs ) const
            {
                return ((lhs.first == rhs.first) && (lhs.second == rhs.second));
            }

        };

        typedef struct Triple< wpds_key_t,wpds_key_t,wpds_key_t > KeyTriple;

        struct HashKeyTriple
        {

            wpds::hm_hash< wpds_size_t > hasher;

            wpds_size_t operator()( const KeyTriple& kt ) const
            {
                wpds_size_t hashval = combine_wpds_keys( kt.first,kt.second );
                return hasher( combine_wpds_keys( kt.third,hashval ) );
            }

        };

        struct EqualKeyTriple
        {

            bool operator()( const KeyTriple& lhs, const KeyTriple& rhs ) const
            {
                return ((lhs.first == rhs.first) &&
                        (lhs.second == rhs.second) &&
                        (lhs.third == rhs.third));
            }

        };

    }

}
#endif  // WPDS_UTIL_KEY_PAIR_H_
