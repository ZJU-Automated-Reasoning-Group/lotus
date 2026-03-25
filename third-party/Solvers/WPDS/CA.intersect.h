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

#ifndef CA_INTERSECT_GUARD
#define CA_INTERSECT_GUARD
#include "common.h"
#include "ref_ptr.h"
#include "key_source.h"
#include "keys.h"

namespace wpds
{
    namespace util
    {
        /*
         * StackHasher
         *
         * This class takes as input a reference to an object of type
         * StackHash_t and places CA<T>::ca_trans_t into the hash
         * reference.  We inherit from TransActionFunctor<T> to be
         * used w/ CA<T>::for_each.
         */
        template< typename T >
        struct StackHasher : wpds::util::TransActionFunctor<T>
        {
            typedef GPP_IMP_TYPENAME__ wpds::CA<T>::catrans_t Trans;
            typedef std::list< Trans > StackHashList_t;
            typedef wpds::HashMap< wpds::wpds_key_t, StackHashList_t > StackHash_t;

            StackHasher( StackHash_t& ref ) : hash(ref) {}

            virtual void operator()( const Trans& t )
            {
                Trans t1(t);
                GPP_IMP_TYPENAME__ StackHash_t::iterator it =
                    hash.find(t->stack());

                if( it == hash.end() )
                {
                    StackHashList_t ls;
                    ls.push_back( t1 );
                    hash.insert( t1->stack(),ls );
                }
                else
                {
                    it->second.push_back( t1 );
                }
            }

            StackHash_t& hash;

        };

        template< typename T, typename U,typename R > struct WeightMaker
        {
            /*
             * WeightMaker must define an operator() with the following
             * signature:

             R* operator()(
             const GPP_IMP_TYPENAME__ Semiring<T>::SemiringElement& t,
             const GPP_IMP_TYPENAME__ Semiring<U>::SemiringElement& u ) {...}

             *
             * This is b/c the method semiring_element() of catransition
             * returns a const Semiring<T>::SemiringElement&.
             *
             * R* must be returned b/c the add method of CA expects
             * a pointer to some type T that defines the semiring.  Remember
             * once a pointer is passed to CA<T>.add the CA assumes responsibility
             * for it!
             */
            virtual ~WeightMaker() {}

            virtual R* operator()(
                    const GPP_IMP_TYPENAME__ Semiring<T>::SemiringElement& t,
                    const GPP_IMP_TYPENAME__ Semiring<U>::SemiringElement& u ) = 0;

        };

        /*! @brief Performs intersection of 2 CAs
         *
         * This method intersects the 2 passed in CAs are places
         * the result in the passed in output CA.  A functor WeightMaker
         * is passed in that tells intersect how to deal with the weights
         * from the 2 CAs to be intersected.
         *
         * @see CA
         * @see utill::WeightMaker
         *
         * @param lhs CA on left side of intersect
         * @param rhs CA on right side of intersect
         * @param wmaker The WeightMaker functor
         * @param result CA that will be result of intersection
         * 
         * TODO:
         *     Make intersection work as:
         *       1. hash rhs on stack symbol
         *       2. Do for each on lhs creating
         *          new transitions as you go
         *       3. This will go through the transitions
         *          less times but will do potentially more hash lookups.
         *
         *     **NOTE** I think the algorithm above is a classic DB algorithm and
         *     was found to be efficient
         */
        template< typename T,typename U,typename R > void
        intersect(
                const CA<T>& lhs,
                const CA<U>& rhs,
                WeightMaker<T,U,R>& wmaker,
                CA<R>& result )
        {
            //
            wpds::wpds_key_t from,from1,from2,stack,to,to1,to2;
            wpds::Query origQuery = lhs.get_query();
            // Hash the transitions on stack symbol
            GPP_IMP_TYPENAME__ StackHasher<T>::StackHash_t lhash;
            GPP_IMP_TYPENAME__ StackHasher<U>::StackHash_t rhash;
            StackHasher<T> lhasher( lhash );
            StackHasher<U> rhasher( rhash );
            lhs.for_each( lhasher );
            rhs.for_each( rhasher );

            // Before result is cleared we need to record the new init state
            // and final states.
            wpds_key_t result_init =
                create_key( new key_pair_src(
                            lhs.initial_state(),rhs.initial_state()));

            // set for holding accept states
            std::set< wpds_key_t > result_accept;
            
            // hold onto a copy of the end iterators so they are not created
            // each time
            std::set< wpds_key_t >::const_iterator lhsend = lhs.final_states().end();
            std::set< wpds_key_t >::const_iterator rhsend = rhs.final_states().end();

            for( std::set< wpds_key_t >::const_iterator lhsbeg = lhs.final_states().begin();
                    lhsbeg != lhsend ;
                    lhsbeg++ )
            {
                wpds_key_t lhskey = *lhsbeg;
                for( std::set< wpds_key_t >::const_iterator rhsbeg = rhs.final_states().begin();
                        rhsbeg != rhsend ;
                        rhsbeg++ )
                {
                    wpds_key_t rhskey = *rhsbeg;
                    result_accept.insert(
                            create_key(
                                new key_pair_src( lhskey,rhskey ) ) );

                }
            }

            // Clearing result allows for the result of the intersection
            // being stored back into either the lhs or rhs because the
            // transitions are stored in the hash tables
            result.clear();

            // copy back in accept state
            result.add_initial_state( result_init );

            // copy back in final state(s)
            std::set< wpds_key_t >::iterator accend = result_accept.end();
            for( std::set< wpds_key_t >::iterator accit = result_accept.begin();
                    accit != accend ;
                    accit++ )
            {
                result.add_final_state( *accit );
            }

            // initialize results query mode
            result.set_query( origQuery );

            GPP_IMP_TYPENAME__ StackHasher<T>::StackHash_t::iterator iter;
            GPP_IMP_TYPENAME__ StackHasher<U>::StackHash_t::iterator lookup;
            for( iter = lhash.begin(); iter != lhash.end(); iter++ )
            {
                lookup = rhash.find( iter->first );
                if( lookup == rhash.end() )
                    continue;


                GPP_IMP_TYPENAME__ StackHasher<T>::StackHashList_t::iterator lit;
                GPP_IMP_TYPENAME__ StackHasher<U>::StackHashList_t::iterator rit;
                for( lit = iter->second.begin();
                        lit != iter->second.end(); lit++ )
                {
                    for( rit = lookup->second.begin();
                            rit != lookup->second.end(); rit++ )
                    { 
                        from1 = (*lit)->from_state();
                        from2 = (*rit)->from_state();
                        from = create_key( new key_pair_src(from1,from2) );
                        to1 = (*lit)->to_state();
                        to2 = (*rit)->to_state();
                        to = create_key( new key_pair_src(to1,to2) );
                        stack = iter->first;
                        result.add(from,stack,to,
                                wmaker((*lit)->semiring_element(),
                                    (*rit)->semiring_element()) );
                    }
                }
            }
        }

        /*! Util class defined for extra definition of intersect
         * in case the user does not want to define a WeightMaker
         * and template on the return type.  A SemiringElemPair is
         * as it says, a cross product of two SemiringElements.  The
         * template types T and U are expected to fully implement the 
         * interface layed out in semiring.h.
         *
         * Like std::pair, just access the SemiringElements of the pair
         * by their class name first and second and not by using methods.
         *
         * SemiringElemPair relies on uniqueness of the ONE and ZERO
         * pointers.  
         *  TODO: This has not been thoroughly tested!
         *
         */

        template< typename T,typename U > class SemiringElemPair
        {
            public:
                typedef ref_ptr< T > first_type;
                typedef ref_ptr< U > second_type;
                unsigned int count;
                first_type first;
                second_type second;

                SemiringElemPair(
                        T &t_,
                        U &u_,
                        unsigned int c=0 )
                    : count(c),first(&t_),second(&u_) {}

                SemiringElemPair(
                        T *t_,
                        U *u_,
                        unsigned int c=0 )
                    : count(c),first(t_),second(u_) {}

                SemiringElemPair(
                        const first_type& t_,
                        const second_type& u_,
                        unsigned int c=0 )
                    : count(c),first(t_),second(u_) {}

                T *get_first() const {
                    return first.get_ptr();
                }
                U *get_second() const {
                    return second.get_ptr();
                }

                // This is a rather cludgy way to set up
                // one and zero
                static void WPDS_CALL init(T *t_, U *u_) {
                    if(0 == ZERO) {
                        ZERO = new SemiringElemPair(t_->zero(), u_->zero(),1);
                    }
                    if(0 == ONE) {
                        ONE = new SemiringElemPair(t_->one(), u_->one(),1);
                    }
                }

                static SemiringElemPair* WPDS_CALL 
                make_one( first_type fone, second_type sone )
                {
                    if( 0 == ONE )
                        ONE = new SemiringElemPair( fone,sone,60 );
                    return ONE;
                }

                static SemiringElemPair* WPDS_CALL 
                make_zero( first_type fzero, second_type szero )
                {
                    if( 0 == ZERO )
                        ZERO = new SemiringElemPair( fzero,szero,60 );
                    return ZERO;
                }

                static SemiringElemPair* WPDS_CALL one()
                {
                    return ONE;
                }

                static SemiringElemPair* WPDS_CALL zero()
                {
                    return ZERO;
                }

                SemiringElemPair * extend ( SemiringElemPair *p ) const
                {
                    SemiringElemPair *sp;
                    //p->print( print( std::cout ) << "\tX\t" );
                    if( equal(zero()) || p->equal(zero()) )
                    {
                        sp = zero();
                    }
                    else
                    {
                        sp = new SemiringElemPair(
                                first->extend(p->first.get_ptr()),
                                second->extend(p->second.get_ptr()) );
                    }
                    //sp->print(std::cout << "\tis:  ") << std::endl;
                    return sp;

                }

                SemiringElemPair * combine( SemiringElemPair *p ) const
                {
                    if( equal(zero()) )
                        return p;
                    else if( p->equal(zero()) )
                    {
                        // Do this b/c ONE & ZERO are unique pointers
                        // and returning this causing trouble due
                        // to the const qualifier
                        if( this == one() )
                            return one();
                        else
                            return new SemiringElemPair(first,second);
                    }
                    else
                        return new SemiringElemPair(
                                first->combine(p->first.get_ptr()),
                                second->combine(p->second.get_ptr()) );
                }

                SemiringElemPair * quasiOne() const
                {
                    return new SemiringElemPair(
                            first->quasiOne(),second->quasiOne() );
                }

#ifdef DWPDS
                SemiringElemPair *diff(SemiringElemPair *p) const {
                    return new SemiringElemPair(first->diff(p->first.get_ptr()), 
                            second->diff(p->second.get_ptr()));
                }
#endif 

                bool equal( SemiringElemPair *p ) const
                {
                    return ((this == p) ||
                            (first->equal(p->first.get_ptr()) &&
                             second->equal(p->second.get_ptr())) );
                }

                std::ostream& print( std::ostream& o ) const
                {
                    first->print(o << "[ ");
                    return (second->print( o << " , ") << " ]");
                }

                // Not currently supported for pairs
                SemiringElemPair * parse_element( const char * )
                {
                    return one();
                }

            private:
                static SemiringElemPair *ONE;
                static SemiringElemPair *ZERO;
        };

        /*
         * ONE and ZERO must be defined here for them to be
         * static pointers.  I ?think? they must be pointers b/c
         * there is no .cpp file but instead this file is inlined
         * macro style in a .cpp file it is needed by??  Someone clarify
         * this please
         */
        template< typename T,typename U >
            SemiringElemPair<T,U>* SemiringElemPair<T,U>::ONE = 0;

        template< typename T,typename U >
            SemiringElemPair<T,U>* SemiringElemPair<T,U>::ZERO = 0;

        /*!
         * WeighMaker impl that ignores the r.h.s. weight passed in.
         *
         * Input:  2 semiring elements
         * Output: The first semiring element
         */
        template< typename T >
        struct KeepLeft : public WeightMaker<T,T,T>
        {
            virtual ~KeepLeft() {}

            virtual T* operator()(
                    const GPP_IMP_TYPENAME__ Semiring<T>::SemiringElement& t,
                    const GPP_IMP_TYPENAME__ Semiring<T>::SemiringElement& u )
            {
                return t.get_ptr();
            }
        };

        /*!
         * WeightMaker that pairs up the weights
         */
        template< typename T, typename U >
        struct KeepBoth : public WeightMaker< T,U,SemiringElemPair< T,U > >
        {
            virtual ~KeepBoth() {}

            virtual T* operator()(
                    const GPP_IMP_TYPENAME__ Semiring<T>::SemiringElement& t,
                    const GPP_IMP_TYPENAME__ Semiring<T>::SemiringElement& u )
            {
                // Check for 1 and 0 b/c SemiringElemPair uses unique pointers
                // to represent ONE and ZERO.
                // TODO:
                //      Test this code to make sure uniqueness is not lost
                if( t->equal(t->zero()) && u->equal(u->zero()) )
                {
                    return SemiringElemPair<T,U>::zero();
                }
                else if(t->equal(t->one()) && u->equal(u->one()) )
                {
                    return SemiringElemPair<T,U>::one();
                }
                else
                {
                    return new SemiringElemPair<T,U>(t,u);
                }
            }
        };

#ifdef __GNUG__
        /*! Partially specialized implementation of intersect with the return
         * type being CA< SemiringElemPair<T,U> >.  This is defined to give
         * the user a "quick" intersect w/out worrying about WeightMakers...
         * This method returns a CA< SemiringElemPair<T,U> > instead of taking
         * one as an argurment b/c of the annoying way that
         * SemiringElemPair::one and zero have to be set up.
         *
         * @param lhs CA on left side of intersect
         * @param rhs CA on right side of intersect
         * @return CA that is result of intersect w/ weights pairwise combined
         *
         *
         * FIXME:
         *      Fix SemiringElemPair so that a result reference
         *      CA can be passed in to keep the interface consisten
         *      with the more general intersect
         */

        template< typename T, typename U > CA< SemiringElemPair<T,U> >
        intersect( const CA<T>& lhs,const CA<T>& rhs )
        {
            // Initialize the pair
            SemiringElemPair< T,U > *pone = 
                SemiringElemPair<T,U>::make_zero(
                        lhs.semiring().zero(),rhs.semiring().zero() );

            pone = SemiringElemPair<T,U>::make_one(
                    lhs.semiring().one(),rhs.semiring().one() );

            // Create semiring
            Semiring< SemiringElemPair<T,U> > s(pone);

            // Create resultant CA
            CA< SemiringElemPair<T,U> > result( s );

            KeepBoth<T,U> wmaker;
            intersect< T,U,SemiringElemPair<T,U> >( lhs,rhs,wmaker,result );
            return result;
        }

#endif // __GNUG__
    }

}
#endif

/* Yo, Emacs!
   ;;; Local Variables: ***
   ;;; tab-width: 4 ***
   ;;; End: ***
 */
