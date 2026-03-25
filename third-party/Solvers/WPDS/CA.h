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

#ifndef wpds_CA_H_
#define wpds_CA_H_ 1

#include <algorithm>
#include <set>
#include <list>
#include "common.h"
#include "inst_counter.h"
#include "catransition.h"
#include "UTIL.h"
#include "Traits.h"

/*
 * Modify these to adjust the initial number
 * of buckets in the three hash tables.
 */
#define INIT_P_HASH_SIZE 43
#define INIT_PG_HASH_SIZE 1543
#define INIT_PGQ_HASH_SIZE 3079
#define INIT_EPSQ_HASH_SIZE 43

/*
 * class CA
 * This class is an input and output to the pre* and post* algorithms.
 */
namespace wpds
{

    template< typename T > class SaturationProcess;
    template< typename T > class Witness;
    template< typename T > struct InjectTransActionFunctor;
    template< typename T > struct ProjectTransActionFunctor;

    using namespace util;

    template< typename T > class CA
    {

        friend class SaturationProcess<T>;
        friend struct InsertTransActionFunctor<T>;
        friend struct InjectTransActionFunctor<T>;
        friend struct ProjectTransActionFunctor<T>;

        public:
            GEN_WPDS_TYPEDEFS(T);

            // Key Sets (for F)
            typedef STDSET(wpds_key_t) KeySet;
            typedef GPP_IMP_TYPENAME__ KeySet::iterator KeySetIter;

            // Transition Hashes
            // (p,*,*)
            typedef HashMap< wpds_key_t,TransList > PTransListHash;
            typedef GPP_IMP_TYPENAME__ PTransListHash::iterator PTransListHashIter;
            typedef GPP_IMP_TYPENAME__ PTransListHash::const_iterator ConstPTransListHashIter;

            // (p,g,*)
            typedef HashMap< KeyPair,TransList,HashKeyPair,EqualKeyPair > PGTransListHash;
            typedef GPP_IMP_TYPENAME__ PGTransListHash::iterator PGTransListHashIter;

            // (p,g,q)
            typedef HashMap< KeyTriple, catrans_t, HashKeyTriple, EqualKeyTriple > PGQTransHash;
            typedef GPP_IMP_TYPENAME__ PGQTransHash::iterator PGQTransHashIter;

            // (*,EPS,q)
            typedef HashMap< wpds_key_t,TransList > EpsQTransListHash;
            typedef GPP_IMP_TYPENAME__ EpsQTransListHash::iterator EpsQTransListHashIter;
            typedef GPP_IMP_TYPENAME__ EpsQTransListHash::const_iterator ConstEpsQTransListHashIter;

            // Used in path_summary
      protected:
            struct State;
      public:
            typedef HashMap< wpds_key_t,State * > StateWeightHash;
            typedef GPP_IMP_TYPENAME__ StateWeightHash::iterator StateWeightHashIter;

            CA( const Semiring<T>& s_ )
                : s( s_ )
                , p_hash( PTransListHash( INIT_P_HASH_SIZE ) )
                , pg_hash( PGTransListHash( INIT_PG_HASH_SIZE ) )
                , pgq_hash( PGQTransHash( INIT_PGQ_HASH_SIZE ) )
                , epsq_hash( EpsQTransListHash( INIT_EPSQ_HASH_SIZE ) )
                , init_st( WPDS_EPSILON )
                , query( Query::user() )
                {}

            CA( const CA& ca )
                : s( ca.s )
                , p_hash( PTransListHash( ca.p_hash.capacity() ) )
                , pg_hash( PGTransListHash( ca.pg_hash.capacity() ) )
                , pgq_hash( PGQTransHash( ca.pgq_hash.capacity() ) )
                , epsq_hash( EpsQTransListHash( ca.epsq_hash.capacity() ) )
                , init_st( ca.init_st )
                , F( ca.F )
                , Q( ca.Q )
                , query( ca.query)
                {
                    copy_from( ca );
                }

            CA<T>& operator=( const CA<T>& ca )
            {
                // Clear our allocated structures
                erase_me();

                // Set our vars
                s = ca.s;
                init_st = ca.init_st;
                F = ca.F;
                Q = ca.Q;
                query = ca.query;
                // Copy transitions
                copy_from( ca );
                return *this;
            }

            ~CA() {
                erase_me();
            }

            void clear() {
                erase_me();
            }

            void erase_me() {
#ifdef WITNESS_TRACE
                // What do you get if a Witness points to a transition
                // and a transition points to a witness?  A Cycle.
                // What is reference counting not good at removing?
                // ... Cycles!  Let's break those cycles.
                ClearWitnessTransActionFunctor<T> eraser;
                for_each(eraser);
#endif /* WITNESS_TRACE */
                StateWeightHashIter i;
                for( i = st_W_hash.begin(); i != st_W_hash.end(); ++i ) {
                    delete ((*i).second);
                }
                st_W_hash.clear();
                F.clear();
                init_st = WPDS_EPSILON;
                query = Query::user();
                // Need to clear our transition hashes. They are reference
                // counted so no need to worry about memory
                p_hash.clear();
                pg_hash.clear();
                pgq_hash.clear();
                epsq_hash.clear();
            }

            const Semiring<T>& semiring() const { return s; }
            const Query get_query() const { return query; }
            void set_query( Query q ) { query = q; }

            const catrans_t add(
                    const wpds_key_t from,
                    const wpds_key_t name,
                    const wpds_key_t to,
                    T *t )
            {
                sem_elem_t se( t );
                catrans_t trans = insert( from,name,to,se );
#ifdef WITNESS_TRACE
                witness_t witness = new TransWitness(se);
                witness->transition( trans );
                trans->witness( witness );
#endif
                return trans;
            }

            /*! Set the initial state of the CA
             * @param p the initial state of the CA
             */
            void add_initial_state( wpds_key_t p )
            {
                init_st = p;
            }

            /*! Get the initial state of the CA
             *
             * @return the initial state
             */
            wpds_key_t initial_state() const
            {
                return init_st;
            }

            /*! Check if p is the initial state
             *
             * @param p the state to check
             * @return true if p is the initial state
             */
            bool is_initial_state( wpds_key_t p ) const
            {
                return p == init_st;
            }

            /*! Add a final state to the CA
             * @param q the final state to be added
             */
            void add_final_state( wpds_key_t q )
            {
                F.insert( q );
            }

            /*! Query if a state q is a final state
             * @param q the state being queried
             * @return true if q is a final state
             */
            bool is_final_state( const wpds_key_t q ) const
            {
                return (F.find(q) != F.end());
            }

            /*! @brief Lookup transition (p,g,q) in the CA
             *
             * This is the preferred method for finding
             * a transition in the CA
             *
             * @param p the from state
             * @param g the stack symbol
             * @param q the to state
             * @param t a reference to be filled if the transition exists
             * @return true if find succeeded
             *
             */
            bool find( wpds_key_t p,
                    wpds_key_t g,
                    wpds_key_t q,
                    catrans_t& t )
            {

                PGQTransHashIter iter = pgq_hash.find( KeyTriple( p,g,q ) );

                if( iter != pgq_hash.end() ) {
                    t = iter->second;
                    return true;
                }
                else
                    return false;

            }

            /*! @brief DEPRACATED: lookup transition (p,g,q) in the CA
             *
             * find looks up transition (p,g,q) in the CA and returns
             * the transition if it exists.  An "empty" transition is returned
             * if no such transition exists.  This method is depracated and
             * is not guaranteed to continue to exist.
             *
             * @param p the from state
             * @param g the stack symbol
             * @param q the to state
             * @return the transition (p,g,q) or an empty ref_ptr
             */
            catrans_t find( wpds_key_t p,wpds_key_t g,wpds_key_t q )
            {

                PGQTransHashIter iter = pgq_hash.find( KeyTriple( p,g,q ) );

                if( iter != pgq_hash.end() )
                    return iter->second;
                else
                    return catrans_t(0);

            }

            /*! @brief Match all transitions with from state p
             *
             * match returns a pair of Iterators that allow the user to
             * iterate over all transitions of the form (p,*,*).  The iterator
             * has type CA<T>::TransListIterator.
             *
             * @param p the from state to match on
             * @return a pair of TransListIterators
             */
            TransListIterPair match( wpds_key_t p )
            {

                PTransListHashIter iter = p_hash.find( p );

                if( iter != p_hash.end() ) {
                    TransList& tset = iter->second;
                    return TransListIterPair( tset.begin(),tset.end() );
                }
                else {
                    return TransListIterPair(
                            EMPTY_TRANS_SET.end(),EMPTY_TRANS_SET.end() );
                }

            }


            /*! @brief Match all transitions with from state p and stack g
             *
             * match returns a pair of Iterators that allow the user to
             * iterate over all transitions of the form (p,g,*).  The iterator
             * has type CA<T>::TransListIterator.
             *
             * @param p the from state to match on
             * @param g the stack symbol to match on
             * @return a pair of TransListIterators
             */
            TransListIterPair match( wpds_key_t p,wpds_key_t g )
            {

                PGTransListHashIter iter = pg_hash.find( KeyPair(p,g) );

                if( iter != pg_hash.end() ) {
                    TransList& tset = iter->second;
                    return TransListIterPair( tset.begin(),tset.end() );
                }
                else {
                    return TransListIterPair(
                            EMPTY_TRANS_SET.end(),EMPTY_TRANS_SET.end() );
                }

            }

            /*! @brief Match all transitions of form (*,eps,q)
             *
             * match_eps_to returns a pair of Iterators that allow the user to
             * iterate over all transitions of the form (*,eps,q).  The iterator
             * has type CA<T>::TransListIterator.
             *
             * @return A pair of transition set iterators
             */
            TransListIterPair match_eps_to( wpds_key_t q )
            {

                EpsQTransListHashIter iter = epsq_hash.find( q );

                if( iter != epsq_hash.end() ) {
                    TransList& tset = iter->second;
                    return TransListIterPair( tset.begin(),tset.end() );
                }
                else {
                    return TransListIterPair(
                            EMPTY_TRANS_SET.end(),EMPTY_TRANS_SET.end() );
                }
            }

            std::ostream& print( std::ostream& out ) const
            {
                out << "CA\n  initial_state: ";
                if( initial_state() != WPDS_EPSILON )
                {
                    printkey( initial_state(),out );
                }
                out << "\n  F    : {";
                for( KeySet::const_iterator it = F.begin(); it != F.end() ; it++ )
                    printkey(*it,out << " ");
                out << " }\n";
                WriteTransActionFunctor<T> writer(out);
                for_each( writer );
                return out;
            }

            sem_elem_t state_weight( wpds_key_t q ) const {
                GPP_IMP_TYPENAME__ StateWeightHash::const_iterator iter
                    = st_W_hash.find( q );
                if( iter != st_W_hash.end() )
                    return iter->second->W;
                else
                    return s.zero();
            }

            // Return 1 if q is not a state?
            //typedef HashMap< wpds_key_t,State * > StateWeightHash;
            //typedef GPP_IMP_TYPENAME__ StateWeightHash::iterator StateWeightHashIter;
            const sem_elem_t state_quasione( wpds_key_t q ) const
            {
                GPP_IMP_TYPENAME__ StateWeightHash::const_iterator iter
                    = st_W_hash.find( q );
                if( iter == st_W_hash.end() )
                    return s.one();
                else
                    return iter->second->quasione;
            }

            void state_quasione( wpds_key_t q, const sem_elem_t& quasi )
            {
                StateWeightHashIter iter = st_W_hash.find( q );
                if( iter != st_W_hash.end() ) {
                    iter->second->quasione = quasi;
                }
            }

            void for_each( TransActionFunctor<T>& action ) const;

            struct KeyPairPredicate {
                typedef KeyPair argument_type;
                typedef bool result_type;
                virtual ~KeyPairPredicate() {}
                virtual bool operator()(KeyPair k) const = 0;
            };
            struct AllSymsTrueFn : public KeyPairPredicate {
                AllSymsTrueFn() {}
                virtual ~AllSymsTrueFn() {}
                bool operator()(KeyPair k) const { return true; }
            };
            class SymsInSetFn : public KeyPairPredicate {
                public:
                    SymsInSetFn(const KeySet& R) { pKeySet = &R; }
                    virtual ~SymsInSetFn() {}
                    bool operator()(KeyPair k) const { 
                        return (pKeySet->find(k.second) != pKeySet->end()); 
                    }
                private:
                    const KeySet * pKeySet;
            };


            void path_summary(const KeyPairPredicate & R_fn);
            void path_summary(const KeySet &R) { 
                path_summary(SymsInSetFn(R)); 
            }
            void path_summary() {
                path_summary(AllSymsTrueFn());
            }

            /*! @brief Depracated. use reglang_query
             *
             * @see reglang_query
             */
            sem_elem_t regex_query( const CA<T>& lang ) const
            {
                std::cerr <<
                    "[ WARNING ] CA<T>::reg_query depracated. Use CA<T>::reglang_query.\n";
                return reglang_query( lang );
            }

            /*! @brief return MOP with respect to language
             *
             * reglang_query takes a CA<T> as input where the CA<T> specifies
             * an automaton for the language in which to query this.  In other
             * words, it is a regular expression.  Running reglang_query will
             * intersect "this" with the input CA and perform path_summary
             * returning the resulting weight.  The weight will be zero()
             * if either automaton does not have an initial state or if the
             * intersection is empty.
             *
             * @see CA<T>::path_summary
             * @see wpds::util::intersect
             * @see sem_elem_t
             *
             * @input lang the automaton defining the input language
             * @return sem_elem_t the MOP weight
             */
            sem_elem_t reglang_query( const CA<T>& lang ) const
            {
                sem_elem_t answer = s.zero();
                if( WPDS_EPSILON == lang.initial_state() )
                    std::cerr << "No initial state for reglang automaton.\n";
                if( WPDS_EPSILON == initial_state() )
                    std::cerr << "No initial state for queried automaton.\n";
                else
                {
                    CA<T> result(s);
                    result.query = query;
                    util::KeepLeft<T> wmaker;
                    util::intersect<T,T,T>(*this,lang,wmaker,result);
                    DBGS(print( std::cout << "--- reglang --- \n\n") << "\n\n";);
                    result.path_summary();
                    answer = result.state_weight( result.initial_state() );
                }
                return answer;
            }

            inline wpds_size_t count_transitions () const {
                return pgq_hash.size();
            } // count_transitions

            /*!
             * Erases the transition (p,g,q) from the CA.
             *
             * All hashtables will be modified (except possibly epsq).
             * No checks are performed on removing states if it is the last
             * trans in the p_hash or clearing list from hashtables if they
             * are now empty.
             *
             * @param p the from state key
             * @param g the stack key
             * @param q the to state key
             */
            void erase( wpds_key_t p, wpds_key_t g, wpds_key_t q );

        protected:    // types

             /*! State is an inner struct used to hold information for both
              * SaturationProcess::prestar and SaturationProcess::poststar as
              * well as CA::path_summary. prestar and poststar use it for
              * quasione while it is the main data for path_summary.  A State
              * represents (what else :) a State in the automaton.
              */
            struct State PI_STATS_INST_COUNT(State) {
                bool is_in_workset;
                wpds_key_t key;
                sem_elem_t W;
                sem_elem_t quasione;
#ifdef DWPDS
                sem_elem_t delta;
#endif

                State()
                    : is_in_workset(false), key(0) {}

                State( wpds_key_t q_, sem_elem_t W_ )
                    : is_in_workset(false), key( q_ )
                    ,W( W_ ),quasione(W_.get_ptr()->zero())
#ifdef DWPDS
                    , delta( W_ )
#endif
                    {}


                /*! operator() is defined so that State can act as a functor
                 *  for comaparison in sets
                 *
                 *  @param lhs
                 *  @param rhs
                 *  @return true if lhs is less than rhs
                 */

                bool operator()( const State *a, const State *b ) const
                {
                    return (a->key && b->key)   ?
                        (a->key < b->key)    :
                        (a->key && ! b->key) ;
                }
            };

            typedef STDSET2(State *, State) pred_set_t;
            typedef HashMap< wpds_key_t, pred_set_t > PredHash;
            typedef GPP_IMP_TYPENAME__ PredHash::iterator PredHashIter;

        protected:    // virtual methods
            void copy_from( const CA& );

        public:    // methods

            void make_state( wpds_key_t st )
            {
                if( st_W_hash.find( st ) == st_W_hash.end() )
                {
                    st_W_hash.insert( st,new State(st,s.zero()) );
                    Q.insert( st );
                }
            }

            catrans_t make_catrans(
                    const wpds_key_t from,
                    const wpds_key_t name,
                    const wpds_key_t to,
                    const sem_elem_t& se )
            {
                // Create the State objs
                make_state( from );
                make_state( to );

                return catrans_t(
                        new CATransition<T>(from,name,to,se) );
            }

            const std::set< wpds_key_t > & states() const { return Q; }

            const std::set< wpds_key_t > & final_states() const { return F; }

        private:
            catrans_t insert(
                    const wpds_key_t from,
                    const wpds_key_t name,
                    const wpds_key_t to,
                    const sem_elem_t& );
        private:
            /*!
             * Performs the acutal work of path_summary
             */
            void p_readout( PredHash& pred_hash, 
                    const KeyPairPredicate& A_fn );

            /*!
             * Helper function for erase
             */
            bool erase_from_translist(
                    const wpds_key_t p,
                    const wpds_key_t g,
                    const wpds_key_t q,
                    TransList & tls );

        private:    // vars

            Semiring<T> s;
            PTransListHash p_hash;
            PGTransListHash pg_hash;
            PGQTransHash pgq_hash;
            EpsQTransListHash epsq_hash;
            wpds_key_t init_st; //! < initial state
            KeySet F;           //! < set of final states
            KeySet Q;           //! < set of all states
            Query query;

            // This is used for path_summary/readout
            StateWeightHash st_W_hash;

            // I would like this to be static but then we need
            // a .cpp file
            TransList EMPTY_TRANS_SET;

    };  // clas CA

    /*************************************************************/
    /*                  PUBLIC                                   */
    /*************************************************************/

    template< typename T > void
    CA<T>::for_each( TransActionFunctor<T>& action ) const
    {
        TransListConstIter titer;
        ConstPTransListHashIter setIter = p_hash.begin();

        for( ; setIter != p_hash.end(); setIter++ ) {
            const TransList& tSet = setIter->second;
            for( titer = tSet.begin(); titer != tSet.end(); titer++ ) {
                action( *titer );
            }
        }
    }

    // Used in path_summary
    //typedef std::pair< sem_elem_t, bool > state_info_t;
    //typedef HashMap< wpds_key_t,state_info_t > state_info_map_t;;

    template< typename T > void
    CA<T>::path_summary(const GPP_IMP_TYPENAME__ CA<T>::KeyPairPredicate & A_fn)
    {
        // We cannot do path summary if we do not know how to combine
        // the weights.  The only way this could happen is if the user
        // called path_summary before calling prestar or poststar
        if( query.is_user() )
        {
            CND_DBGS(b_readout,
                    {std::cout << "[ERROR] CA::query is defined as user.";
                    std::cout << "  CA::path_summary() aborted.\n";});
            return;
        }
        PredHash pred_hash;

        // zero the states
        StateWeightHashIter iter = st_W_hash.begin();
        for( ; iter != st_W_hash.end(); iter++ ) {
            iter->second->W = s.zero();
#ifdef DWPDS
            iter->second->delta = s.zero();
#endif // DWPDS
        }

        // set up predecessors
        PGTransListHashIter pgIter = pg_hash.begin();
        for( ; pgIter != pg_hash.end(); pgIter++ ) {

            // make sure <p,g> is elem of A
            if( ! A_fn.operator()(pgIter->first) ) {
                continue;
            }

            const TransList& tset = pgIter->second;
            TransListConstIter tIter = tset.begin();
            // tIter -> (p,g,q)
            for( ; tIter != tset.end(); tIter++ ) {
                const catrans_t& t = *tIter;
                PredHashIter phIter = pred_hash.find( t->to_state() );
                // Add State(p) to q's pred set
                if( phIter == pred_hash.end() ) {
                    pred_set_t predset;
                    phIter = pred_hash.insert( t->to_state(),predset ).first;
                }
                phIter->second.insert( st_W_hash.find( t->from_state() )->second );
            }
        }

        p_readout( pred_hash,A_fn );
    }

    /*!
     * Erase transition (p,g,q) from CA
     */
    template< typename T >
    void CA<T>::erase(
            const wpds_key_t p,
            const wpds_key_t g,
            const wpds_key_t q )
    {

        KeyTriple kt(p,g,q);
        PGQTransHashIter pgqit = pgq_hash.find( kt );
        if( pgqit != pgq_hash.end() ) {
            // remove from pgq
            pgq_hash.erase( pgqit );

            // remove from pg
            {
                KeyPair kp(p,g);
                PGTransListHashIter pgit = pg_hash.find( kp );
                TransList & pgls = pgit->second;
                bool rb = erase_from_translist( p,g,q,pgls );
                assert( rb );
            }

            // remove from p
            {
                PTransListHashIter pit = p_hash.find( p );
                TransList & pls = pit->second;
                bool rb = erase_from_translist( p,g,q,pls );
                assert( rb );
            }

            // remove from eps
            if( g == WPDS_EPSILON )
            {
                EpsQTransListHashIter epit = epsq_hash.find( q );
                TransList & epls = epit->second;
                bool rb = erase_from_translist( p,g,q,epls );
                assert( rb );
            }
        }
    }

    /*************************************************************/
    /*                  PRIVATE                                  */
    /*************************************************************/
    template< typename T > GPP_IMP_TYPENAME__ CA<T>::catrans_t
    CA<T>::insert(
            const wpds_key_t from,
            const wpds_key_t name,
            const wpds_key_t to,
            const sem_elem_t& se )
    {

        KeyTriple kt( from,name,to );
        PGQTransHashIter pgqiter = pgq_hash.find( kt );
        catrans_t t;

        if( pgqiter == pgq_hash.end() ) {
            // new transition.  insert in hash tables
            t = make_catrans( from,name,to,se );

            // (p,g,q) => catrans_t
            pgq_hash.insert( kt,t );

            // (p,g) => Set
            PGTransListHashIter pgiter = pg_hash.find( t->key_pair() );
            if( pgiter == pg_hash.end() ) {
                pgiter = pg_hash.insert( t->key_pair(),TransList() ).first;
            }
            pgiter->second.push_back( t );

            // (p) => Set
            PTransListHashIter piter = p_hash.find( from );
            if( piter == p_hash.end() ) {
                piter = p_hash.insert( from,TransList() ).first;
            }
            piter->second.push_back( t );
             
            // (*,EPS,q) => Set
            if( WPDS_EPSILON == name ) {
                EpsQTransListHashIter epsq_iter = epsq_hash.find(to);
                if( epsq_iter == epsq_hash.end() ) {
                    epsq_iter = epsq_hash.insert( to, TransList() ).first;
                }
                epsq_iter->second.push_back( t );
            }

            return t;
        }
        else {
            // transition exists
            t = pgqiter->second;
#ifdef DWPDS
            // Make sure to do this first b/c we need to use the original
            // l(t) not the updated one that the below statement makes
            //t->delta( s.diff(se,t->semiring_element()) );
            t->delta(
                    sem_elem_t(se->diff(t->semiring_element().get_ptr())) );
#endif // DWPDS
            // this will set the modified flag in the FAT accordingly
            //t->semiring_element( s.combine(t->semiring_element(),se) );
            t->semiring_element(
                    sem_elem_t(t->semiring_element()->combine(se.get_ptr())) );

            return t;
        }

    }

    template< typename T >
    void CA<T>::copy_from( const CA<T>& ca )
    {
        InsertTransActionFunctor<T> copier( this );
        ca.for_each( copier );

        KeySet::const_iterator qit = ca.states().begin();
        KeySet::const_iterator qend = ca.states().end();
        for( ; qit != qend ; qit++ ) {
            make_state(*qit);
        }
    }

    /*
     * This is the second half of path_summary.  You must set up the
     * predecessors before calling this.  This will hopefully allow for
     * Algorithm 4 to use the below by computing all predecessors
     */
    template< typename T > void
    CA<T>::p_readout(
            PredHash& pred_hash, 
            const GPP_IMP_TYPENAME__ CA<T>::KeyPairPredicate & A_fn )
    {

        // All final states should start on workset
        // and have initial weight of one

#ifdef PRIORITY_PATH_SUMMARY
        pred_set_t workset;
#else
        STDLIST(State *) workset;
#endif
        State *q;

        // Initialize workset
        KeySetIter keyIter = F.begin();
        for( ; keyIter != F.end(); keyIter++ ) {
            StateWeightHashIter stWiter = st_W_hash.find( *keyIter );
            // final states may not have a state
            if( stWiter == st_W_hash.end() ) {
                q = new State( *keyIter,s.one() );
                st_W_hash.insert( *keyIter, q );
            }
            else {
                q = stWiter->second;
                q->W = s.one();
            }
#ifdef DWPDS
            q->delta = s.one();
#endif
#ifdef PRIORITY_PATH_SUMMARY
            workset.insert( q );
#else
            workset.push_back( q );
#endif
            q->is_in_workset = true;
        }

        // fix point
        sem_elem_t combined = s.zero();
        sem_elem_t extended;

        while( !workset.empty() ) {

            // get state q' off workset
#ifdef PRIORITY_PATH_SUMMARY
            GPP_IMP_TYPENAME__ pred_set_t::iterator stateIt = workset.begin();
            State* qprime = *stateIt;
            workset.erase(stateIt);
#else
            State *qprime = workset.back();
            workset.pop_back();
#endif
            qprime->is_in_workset = false;
#ifdef DWPDS
            sem_elem_t qprime_delta ( qprime->delta);
            qprime->delta = s.zero();
#endif

            CND_DBGS( b_readout, {
                    printkey( qprime->key,std::cerr<< "Popped ") << " : ";
                    qprime->W->print( std::cerr ) << std::endl;
                    } );

#ifdef PATH_SUMMARY_WORKSET_STATS
            {
                extern void ::pathSummaryWorksetStats(sem_elem_t e);
                ::pathSummaryWorksetStats(qprime->W);
            }
#endif

            // Get pred set
            PredHashIter predHashIter = pred_hash.find( qprime->key );
            if( predHashIter == pred_hash.end() ) {
                // Used to flag this as an error but it is not
                continue;
            }

            pred_set_t& predset = predHashIter->second;
            GPP_IMP_TYPENAME__ pred_set_t::iterator predIter =
                predset.begin();

            // for each (q,y,q')
            for( ; predIter != predset.end() ; predIter++ ) {
                // get q
                q = *predIter;
                sem_elem_t oldW( q->W );

                // q.W = combine( q.W, q'.W X l(t) )
                TransListIterPair tpair = match( q->key );
                TransListIter tIter = tpair.first;
                sem_elem_t newW( s.zero() );
#ifdef DWPDS
                for( ; tIter != tpair.second; tIter++ ) { 
                    catrans_t& t = *tIter;
                    // make sure t is of (q,*,q')
                    if( t->to_state() != qprime->key || 
                        ! A_fn(t->key_pair()) ) {
                        continue;
                    }
                    CND_DBGS( b_readout, {
                            t->print( std::cout << "\t matched " ) << std::endl;
                            printkey( q->key, std::cout << "\t\t" );
                            std::cout << " has weight ";
                            q->W->print(std::cout) << std::endl;
                            } );
                    sem_elem_t extendedW;
                    if( query.is_poststar() )
                        extendedW = s.extend( qprime_delta, t->semiring_element() );
                    else
                        extendedW = s.extend( t->semiring_element(),qprime_delta );
                    newW = s.combine( newW, extendedW );
                }
                q->delta = s.combine( q->delta, s.diff( newW, q->W ) );
                q->W = s.combine( newW, q->W );
                if( !s.equal( q->delta, s.zero() ) && !q->is_in_workset) {
                    q->is_in_workset = true;
#ifdef PRIORITY_PATH_SUMMARY
                    workset.insert( q );
#else
                    workset.push_back( q );
#endif
                }

#else
                for( ; tIter != tpair.second; tIter++ ) { 
                    catrans_t& t = *tIter;
                    // make sure t is of (q,*,q')
                    if( (t->to_state() != qprime->key) ||
                            ! A_fn(t->key_pair()) ) { 
                        continue;
                    }
                    CND_DBGS( b_readout, {
                            t->print( std::cout << "\t matched " ) << std::endl;
                            printkey( q->key, std::cout << "\t\t" );
                            std::cout << " has weight ";
                            q->W->print(std::cout) << std::endl;
                            } );
                    sem_elem_t extendedW;
                    if( query.is_poststar() )
                        extendedW = s.extend( qprime->W, t->semiring_element() );
                    else
                        extendedW = s.extend( t->semiring_element(),qprime->W );
                    q->W = s.combine( q->W, extendedW );
                }

                if( !s.equal( q->W, oldW ) ) {
                    if( !q->is_in_workset ) {
                        q->is_in_workset = true;
#ifdef PRIORITY_PATH_SUMMARY
                        workset.insert( q );
#else
                        workset.push_back( q );
#endif

                    }
                }
#endif // DWPDS
            }
        }
    }

    // return true if the erasure worked.
    template< typename T >
    bool CA<T>::erase_from_translist(
            wpds_key_t p,
            wpds_key_t g,
            wpds_key_t q,
            TransList & tls )
    {
        TransListIter tlit = tls.begin();
        TransListIter tlend = tls.end();
        for( ; tlit != tlend ; tlit++ )
        {
            catrans_t & t = *tlit;
            if( (t->from_state() == p)
                    && (t->stack() == g)
                    && (t->to_state() == q) )
            {
                tls.erase( tlit );
                break;
            }
        }
        return (tlit != tlend);
    }

    /********
     * Helper functions
     ********/
    template< typename T > std::ostream&
    operator<<( std::ostream& o, const CA<T>& ca )
    {
        return ca.print(o);
    }

}   // namespace

#endif  // WPDS_CA_H_

/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
