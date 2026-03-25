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

#ifndef wpds_CONFIGURATION_H_
#define wpds_CONFIGURATION_H_ 1

#include <iostream>
#include "common.h"
#include "inst_counter.h"
#include "ref_ptr.h"
#include "semiring.h"
#include "UTIL.h"
#include "Traits.h"
#if defined(GENERIC_WITNESSES)
#include "TWitness.h"
#define COMBINE_WITNESSES(se,w1,w2) combine_twitnesses< ref_ptr<CATransition<T> >, sem_elem_t, const Rule<T>* >( se,w1,w2)
#else
#include "Witness.h"
#define COMBINE_WITNESSES(se,w1,w2) combine_witnesses(w1,w2)
#endif

namespace wpds
{

    /* Class pre defs */
    template< typename T > class CA;
    template< typename T > class SaturationProcess;
    template< typename T > class Witness;
    template< typename T > class Rule;
    namespace util {
        template< typename T > struct InsertTransActionFunctor;
        template< typename T > struct ClearWitnessTransActionFunctor;
    }

    using namespace util;

    /*! @class CATransition
     *
     * A CATransition is a transition in the Configuration Automaton.
     * It has a from and to state and a stack symbol.
     *
     * @see CA
     */
    template< typename T > class CATransition PI_STATS_INST_COUNT(CATransition<T>)
    {
        public:

            friend std::ostream& operator<<(
                    std::ostream& o,
                    const CATransition<T>& C )
            {
                return C.print(o);
            }

            friend class CA<T>;
            friend class SaturationProcess<T>;
            friend class ref_ptr< CATransition >; // for access to count
            friend struct util::InsertTransActionFunctor< T >;
            friend struct util::ClearWitnessTransActionFunctor< T >;

        public:     /* typedefs/enum */
            enum modify_t { C_NONE=0,C_CHX=1,C_NEW=3 };
            GEN_WPDS_TYPEDEFS(T);

        public:     /* Copy Constructor */
            CATransition( const CATransition& C ) :
                kp( C.kp ), to( C.to ) ,se( C.se ), wit(C.wit),
                modified( C.modified ) ,in_workset( C.in_workset )
#ifdef DWPDS
                ,d(C.d)
#endif // DWPDS
            { incTransCount(); }

            ~CATransition() { decTransCount(); }

        public: /* Overloaded operators */
            CATransition& operator=( const CATransition<T>& C );

            bool operator==( const CATransition<T>& C )
            {
                return (
                        (from_state() == C.from_state()) && 
                        (stack() == C.stack()) &&
                        (to_state() == C.to_state())
                        );
            }

        public:     /* getters */
            /*! return the CATransition's from state
             * @return the from state of the CATransition
             * @see wpds_key_t
             */
            const wpds_key_t from_state() const
            {
                return kp.first;
            }

            const wpds_key_t transition() const
            {
                return kp.second;
            }

            /*!
             * @return the stack symbol of the CATransition
             * @see wpds_key_t
             */
            const wpds_key_t stack() const
            {
                return kp.second;
            }

            /*!
             * @return the to state of the CATransition
             * @see wpds_key_t
             */
            const wpds_key_t to_state() const
            {
                return to;
            }

            /*!
             * @return the weight of the transitions
             * @see Traits::sem_elem_t
             */
            const sem_elem_t weight() const
            {
                return se;
            }

            /*!
             * @return the weight of the transitions
             * @see sem_elem_t
             */
            const sem_elem_t semiring_element() const
            {
                return se;
            }
            
            /*!
             * @return the Witness to the generation of the CATransition
             * @see Witness
             */
            const witness_t witness() const
            {
                return wit;
            }

            bool is_new() const
            {
                return modified == C_NEW;
            }
            
            bool is_modified() const
            {
                return modified != C_NONE;
            }

        public:     /* other methods */
            /*!
             * Print a CATransition
             *
             * @param o a reference to the ostream to print to
             * @param printW whether or not to print the weight (default true)
             * @return reference to ostream this was printed to
             */
            std::ostream& print( std::ostream& o,bool printW=true ) const;

        private:    /* vars */
            KeyPair kp;
            wpds_key_t to;
            sem_elem_t se;
            witness_t wit;
            RefCounter count;
            modify_t modified;
            // used in pre/poststar
            // this is mutable b/c it is not a var used by
            // CATransition but by the sat proc
            mutable bool in_workset;

        private:    /* Constructor used by CA */
            CATransition(
                    const wpds_key_t from_,
                    const wpds_key_t trans_,
                    const wpds_key_t to_,
                    const sem_elem_t& se_ )
                : kp( KeyPair(from_,trans_) ), to(to_), se( se_ )
                ,count( 0 ), modified( C_NEW ), in_workset( false )
#ifdef DWPDS
                ,d( se_ )
#endif // DWPDS
                { 
                    incTransCount();
                }


        private:    /* Methods used by CA/SatProcs */
            bool is_in_workset() const
            {
                return in_workset;
            }
           
            void is_in_workset( bool change ) const
            {
                in_workset = change;
            }
           
            wpds_key_t hash_code() const
            {
                return combine_wpds_keys( from_state(),stack() );
            }

            const KeyPair& key_pair() const 
            {
                return kp;
            }

            void semiring_element( const sem_elem_t& se_ );
            void witness( const witness_t& w_ );

#ifdef DWPDS
            /* These will only be used by the SatPrcs and ONLY when DWPDS is
             * enabled.  d (delta) is mutable b/c it is really a member of the
             * SatPrcs and the C has no use for it.  But we keep it here to
             * make pre/poststar alg faster.
             */
            mutable sem_elem_t d;
        public:
            /* Methods for accessing/setting delta */
            sem_elem_t& delta() const
            {
                return d;
            }

            void delta( const sem_elem_t& d_ ) const;
        private:
#endif // DWPDS

    }; // class CATransition

    template< typename T >
    CATransition<T>& CATransition<T>::operator=( const CATransition<T>& C )
    {
        kp      = C.kp;
        to      = C.to;
        se      = C.se;
        wit     = C.wit;
        in_workset = C.in_workset;
        modified = C.modified;
#ifdef DWPDS
        d       = C.d;
#endif // DWPDS
        return *this;
    }

    template< typename T >
    std::ostream& CATransition<T>::print( std::ostream& o,bool printW ) const
    {
        printkey(from_state(), o) << " --- ";
        printkey(stack(), o) << " ---> ";
        printkey(to_state(), o);
        if( printW )
            se->print(o << "\t\t");
        return o;
    }

    /**************************************
     * Private
     **************************************/
    template< typename T >
    void CATransition<T>::semiring_element( const sem_elem_t& se_ )
    {
        // cheap hack.  All <T>s must define an equal method
        if( se->equal(se_.get_ptr()) )
            modified = C_NONE;
        else {
            se = se_;
            modified = C_CHX;
        }
    }

    // Add "wit_new" as a witness for this transition
    template< typename T >
    void CATransition<T>::witness(const witness_t& wit_new)
    {
        if( is_new() )
        {
            wit = wit_new;
        }
        else
        {
            COMBINE_WITNESSES(semiring_element(),wit,wit_new);
        }
    }

#ifdef DWPDS
    template< typename T >
    void CATransition<T>::delta( const sem_elem_t& d_) const
    {
        // I do not like doing this here but it really was the
        // simplest solution.
        if( is_in_workset() )
            d = sem_elem_t( d->combine(d_.get_ptr()) );
        else
            d = d_;
    }
#endif // DWPDS

}   // namespace

#endif  // wpds_CONFIGURATION_H_
/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
