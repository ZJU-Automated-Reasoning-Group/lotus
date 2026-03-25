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
 * $Id: RuleActionFunctor.h,v 1.13 2005-09-06 16:11:35 radu Exp $
 */
#ifndef WPDS_UTIL_RULE_ACTION_FUNCTOR_H_
#define WPDS_UTIL_RULE_ACTION_FUNCTOR_H_

#include <iostream>
#include "HashMap.h"
#include "myallocator.h"
#include "Traits.h"

namespace wpds {

    template< typename T > class SaturationProcess;

    namespace util {

        template< typename T > struct RuleActionFunctor
        {
            GEN_WPDS_TYPEDEFS(T);

            RuleActionFunctor() {}

            virtual ~RuleActionFunctor() {}

            virtual void operator()( const_rule_t r )  = 0;

        };

        template< typename T >
        struct WriteRuleActionFunctor : RuleActionFunctor<T>
        {
            GEN_WPDS_TYPEDEFS(T);

            std::ostream& out;
            int i;

            WriteRuleActionFunctor( std::ostream& o ) : out(o),i(0) {}

            virtual ~WriteRuleActionFunctor() {}

            virtual void operator()( const_rule_t r )
            {
                r->print( out << "\t" << i++ << ")\t") << std::endl;
            }

        };

        template< typename T >
        struct DeleteRuleActionFunctor : RuleActionFunctor<T>
        {
            GEN_WPDS_TYPEDEFS(T);

            DeleteRuleActionFunctor( ) {}

            virtual ~DeleteRuleActionFunctor() {}

            virtual void operator()( const_rule_t r )
            {
                //r->print(std::cerr << "Deleting : ") << std::endl;
                delete r;
            }

        };


// Witness macros
#ifdef WITNESS_TRACE
#   define RUPDATE(a,b,c,d,e)                update(a,b,c,d,e)
#else
#   define RUPDATE(a,b,c,d,e)                update(a,b,c,d)
#endif // Witness

        template< typename T >
        struct UpdateRuleActionFunctor : RuleActionFunctor<T>
        {
            friend class SaturationProcess<T>;
            GEN_WPDS_TYPEDEFS(T);

            SaturationProcess<T>& prcs;

            UpdateRuleActionFunctor( SaturationProcess<T>& sp ) : prcs(sp) {}

            virtual ~UpdateRuleActionFunctor() {}

            virtual void operator()( const_rule_t r )
            {
                sem_elem_t se = r->semiring_element();
                prcs.RUPDATE(
                        r->from_state(),
                        r->from_stack(),
                        r->to_state(),
                        se,
                        //new TransWitness(se)
                        new TransWitnessApply(se,Query::prestar(),r,0,0)
                    );
            }

        };

        template< typename T >
        struct MakeRuleHashBActionFunctor : RuleActionFunctor<T>
        {
            GEN_WPDS_TYPEDEFS(T);

            typedef wpds::HashMap< wpds_key_t, ConstRuleList > raf_hash_t;

            raf_hash_t& hash;

            MakeRuleHashBActionFunctor( raf_hash_t& h )
                : hash( h ) {}

            virtual ~MakeRuleHashBActionFunctor() {}

            virtual void operator()( const_rule_t r )
            {
                GPP_IMP_TYPENAME__ raf_hash_t::iterator iter
                    = hash.find( r->hash_codeb() );

                if( iter == hash.end() ) {
                    ConstRuleList ls;
                    ls.push_back( r );
                    hash.insert( r->hash_codeb(),ls );
                }
                else {
                    iter->second.push_back( r );
                }
            }

        };

    }// namespace util


}    // namespace wpds

#endif //WPDS_UTIL_RULE_ACTION_FUNCTOR_H_
