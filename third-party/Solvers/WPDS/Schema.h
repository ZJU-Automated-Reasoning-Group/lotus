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

#ifndef WPDS_SCHEMA_H_
#define WPDS_SCHEMA_H_ 1

#include <string>
#include <iostream>
#include "semiring.h"
#include "common.h"
#include "UTIL.h"

namespace wpds
{

    using namespace ::wpds::util;

    //template< typename T > class CA;
    template< typename T > class Rule;
    template< typename T > class WPDS;

    template< typename T > class Schema
    {

        /* Used by all Schema derivations */
        friend std::ostream& operator<<( std::ostream& o, const Schema<T>& rs )
        {
            return rs.print(o);
        }

        friend class WPDS<T>;   // for access to SemringElement

        public:
            enum type { SCHEMA0=0,SCHEMA1=1,SCHEMA2=2,MAX=3 };
            GEN_WPDS_TYPEDEFS(T);

        public: // Constructor / Destructor

            Schema(
                    const sem_elem_t& se_,
                    wpds_key_t match_state = WPDS_EPSILON,
                    wpds_key_t match_stack = WPDS_EPSILON,
                    wpds_key_t from_state  = WPDS_EPSILON,
                    wpds_key_t to_state    = WPDS_EPSILON,
                    wpds_key_t to_stack1   = WPDS_EPSILON,
                    wpds_key_t to_stack2   = WPDS_EPSILON
                    )
                : kp( KeyPair( match_state,match_stack ) )
                , s3( from_state )
                , s4( to_state )
                , s5( to_stack1 )
                , s6( to_stack2 )
                , se( se_ )
                , count(0) {}

            Schema(
                    T *t,
                    wpds_key_t match_state = WPDS_EPSILON,
                    wpds_key_t match_stack = WPDS_EPSILON,
                    wpds_key_t from_state  = WPDS_EPSILON,
                    wpds_key_t to_state    = WPDS_EPSILON,
                    wpds_key_t to_stack1   = WPDS_EPSILON,
                    wpds_key_t to_stack2   = WPDS_EPSILON
                    )
                : kp( KeyPair( match_state,match_stack ) )
                , s3( from_state )
                , s4( to_state )
                , s5( to_stack1 )
                , s6( to_stack2 )
                , se( sem_elem_t(t) )
                , count(0) {}

            ~Schema() { }

        public: // Get methods
            wpds_key_t match_state()    const { return kp.first; }
            wpds_key_t match_stack()    const { return kp.second; }
            wpds_key_t from_stack()     const { return s3; }
            wpds_key_t to_state()       const { return s4; }
            wpds_key_t to_stack1()      const { return s5; }
            wpds_key_t to_stack2()      const { return s6; }
            const sem_elem_t& weight()  const { return se; }
            const sem_elem_t& semiring_element() const { return se; }


        public: // other methods 

            wpds_key_t hash_code(bool pre=true) const
            {
                return combine_wpds_keys( match_state(),match_stack() );
            }

            const KeyPair& key_pair() const
            {
                return kp;
            }

            rule_t gen_rule( const catrans_t& t ) const
            {
                // FIXME: combine the weights somehow?
                rule_t r = new Rule<T>( se,
                        t->to_state(), from_stack(),
                        to_state(),to_stack1(),to_stack2() );
                return r;
            }

            bool equal( const Schema *schema ) const
            {
                return ((this == schema) ||
                        (
                         (schema != 0) &&
                         (match_state() == schema->match_state()) &&
                         (match_stack() == schema->match_stack()) &&
                         (from_stack() == schema->from_stack()) &&
                         (to_state()   == schema->to_state())   &&
                         (to_stack1()  == schema->to_stack1())  &&
                         (to_stack2()  == schema->to_stack2())
                        ));
            }

            std::ostream& print( std::ostream& o ) const
            {
                o << "<[";
                printkey(match_state(),o)   <<  ", ";
                printkey(match_stack(),o)   <<  "] ";
                printkey(from_stack(),o)    <<  "> -> <";
                printkey(to_state(),o)      << ", ";
                if( WPDS_EPSILON != to_stack1() ) {
                    printkey(to_stack1(),o);
                }
                if( WPDS_EPSILON != to_stack2() ) {
                    o << " ";
                    printkey(to_stack2(),o);
                }
                o << ">\t";
                return se->print(o);
            }

        protected: // vars
            KeyPair kp; // store as key pair for indexing
            wpds_key_t s3,s4,s5,s6;
            sem_elem_t se;

        public: // vars
            unsigned int count;

    };  // Schema

}   // namespace wpds

#endif  // WPDS_SCHEMA_H_
/* Yo, Emacs!
   ;;; Local Variables: ***
   ;;; tab-width: 4 ***
   ;;; End: ***
 */
