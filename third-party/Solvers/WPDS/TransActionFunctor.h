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
 * $Id: TransActionFunctor.h,v 1.14 2005-09-06 16:11:35 radu Exp $
 */
#ifndef WPDS_UTIL_TRANS_ACTION_FUNCTOR_H_
#define WPDS_UTIL_TRANS_ACTION_FUNCTOR_H_

#include <iostream>
#include "Traits.h"

namespace wpds {

    template< typename T > class CA;
    template< typename T > class WPDS;
    template< typename T > class Witness;
    template< typename T > class WitnessAppl;
    template< typename T > class Semiring;
    template< typename T > class SaturationProcess;
    
    namespace util {

        template< typename T > struct TransActionFunctor
        {
            GEN_WPDS_TYPEDEFS(T);

            TransActionFunctor() {}

            virtual ~TransActionFunctor() {}

            virtual void operator()( const catrans_t& )  = 0;

        };

        template< typename T >
        struct WriteTransActionFunctor : TransActionFunctor<T>
        {
            GEN_WPDS_TYPEDEFS(T);

            std::ostream& out;
            int i;

            WriteTransActionFunctor( std::ostream& o ) : out(o),i(0) {}

            virtual ~WriteTransActionFunctor() {}

            virtual void operator()( const catrans_t& t )
            {
                t->print( out << "\t" << i++ <<") " ) << std::endl;
            }

        };
// Witness macros
#ifdef WITNESS_TRACE
#   define TSET_TRANS_WIT(t,w)   { w->object( t ); t->witness( w );}
#else
#   define TSET_TRANS_WIT(t,w)
#endif // Witness

#ifdef WITNESS_TRACE
        template< typename T > 
        struct ClearWitnessTransActionFunctor : TransActionFunctor<T>
        {
            GEN_WPDS_TYPEDEFS(T);

            ClearWitnessTransActionFunctor() {}

            virtual ~ClearWitnessTransActionFunctor() {}

            virtual void operator()( const catrans_t& t) {
                t->wit = 0;
            }
        };
#endif

        template< typename T >
        struct InsertTransActionFunctor : TransActionFunctor<T>
        {

            GEN_WPDS_TYPEDEFS(T);

            CA<T> *ca;

            InsertTransActionFunctor( CA<T> *ca_ ) : ca(ca_) {}

            virtual ~InsertTransActionFunctor() {}

            virtual void operator()( const catrans_t& t )
            {
                catrans_t tnew =
                    ca->insert( t->from_state(),
                            t->stack(),
                            t->to_state(),
                            t->semiring_element() );
                TSET_TRANS_WIT(tnew,t->witness());
            }

        };

        template< typename T >
        struct WorksetTransActionFunctor : TransActionFunctor<T>
        {
            GEN_WPDS_TYPEDEFS(T);

            SaturationProcess<T>& prcs;

            WorksetTransActionFunctor( SaturationProcess<T>& sat )
                : prcs( sat ) {}

            virtual ~WorksetTransActionFunctor() {}

            virtual void operator()( const catrans_t& t )
            {
                prcs._add_to_workset( t );
            }
        };

    }// namespace util

}    // namespace wpds

#endif //WPDS_UTIL_TRANS_ACTION_FUNCTOR_H_
