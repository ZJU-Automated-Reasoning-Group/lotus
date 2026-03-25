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
 * $Id: SaturationProcess.h,v 1.57 2007-03-30 22:27:43 chi-hua Exp $
 */
#ifndef wpds_SATURATION_PROCESS_H_
#define wpds_SATURATION_PROCESS_H_
#include <map>
#include <list>
#include <vector>
#include <string>
#include "common.h"
#include "ref_ptr.h"
#include "semiring.h"
#include "CA.h"
#include "myallocator.h"
#include "Traits.h"

/* class SaturationProcess
 *
 * This class implements the prestar and poststar algorithms.
 *
 * Usage:
 *      wpds::prestar<T>( WPDS<T>&,CA<T>&,Semiring<T>& );
 *      wpds::poststar<T>( WPDS<T>&,CA<T>&,Semiring<T>& );
 *
 * TODO: Add RuleSchema support to post*
 */


// Common macros
#define L(t)                                (t)->semiring_element()
#define F(r)                                (r)->semiring_element()

// DWPDS macros
#ifdef DWPDS
#   define DELTA(t)                         (t)->delta()
#   define T_MOD_CONTINUE(t,s)              !s.equal((t)->delta(),s.zero())
#   define POST2_SELECT( se,t )             DELTA(t)
#   define APPLY_PRE1( wpds,t,delta )       apply_pre1(wpds,t,delta)
#   define APPLY_PRE2a( wpds,t,delta )      apply_pre2a(wpds,t,delta)
#   define APPLY_PRE2b( wpds,t,delta,hash ) apply_pre2b(wpds,t,delta,hash)
#   define APPLY_POST01( t,delta )          apply_post01(t,delta)
#   define APPLY_POST2( t,delta )           apply_post2(t,delta)
#else
#   define DELTA(t)                         L(t)
#   define T_MOD_CONTINUE(t,s)              (t)->is_modified()
#   define POST2_SELECT( se,t )             se 
#   define APPLY_PRE1( wpds,t,delta )       apply_pre1(wpds,t,L(t))
#   define APPLY_PRE2a( wpds,t,delta )      apply_pre2a(wpds,t,L(t))
#   define APPLY_PRE2b( wpds,t,delta,hash ) apply_pre2b(wpds,t,L(t),hash)
#   define APPLY_POST01( t,delta )          apply_post01(t,L(t))
#   define APPLY_POST2( t,delta )           apply_post2(t,L(t))
#endif // DWPDS

// Witness macros
#ifdef WITNESS_TRACE
#   define UPDATE(f,s,t,se,w)               update(f,s,t,se,w)
#   define UPDATE_PRIME(f,s,t,se,w)         update_prime(f,s,t,se,w)
#   define NEW_WITNESS(se)                  witness_t( new TransWitness(se) )
#   define NEW_WITNESS_EPS(se,w1,w2)        witness_t(new TransWitnessEps( se,w1,w2 ))
#   define NEW_WITNESS_APPL(se,tag,r,w1,w2) witness_t(new TransWitnessApply(se,tag,r,w1,w2))
#   define SET_TRANS_WIT(t,w)               { (w)->object( t ); (t)->witness( w );}
#else
#   define UPDATE(f,s,t,se,w)               update(f,s,t,se)
#   define UPDATE_PRIME(f,s,t,se,w)         update_prime(f,s,t,se)
#   define NEW_WITNESS( se )                0
#   define NEW_WITNESS_EPS( a,b,c )         0
#   define NEW_WITNESS_APPL( a,b,c,d,e )    0
#   define SET_TRANS_WIT(t,w)
#endif // Witness

namespace wpds {

    template< typename T > class WPDS;

    template< typename T > class RuleExtender;

    template< typename T > class SaturationProcess {

        friend struct UpdateRuleActionFunctor<T>;
        friend struct MakeRuleHashBActionFunctor<T>;
        friend struct WorksetTransActionFunctor<T>;

        public:     // Constructor/De

        SaturationProcess(
                WPDS<T>& wpds_,
                CA<T>& ca_,
                Semiring<T>& s_,
                Query q)
            : wpds(wpds_),ca(ca_),s(s_)
            ,query(q),numpops(0)
#ifdef SWPDS
                ,schema_wpds( s_,q)
#endif // SWPDS
                {
                    //! must set this for path_summary
                    ca.query = query;
                }

            virtual ~SaturationProcess() {}

        public:     // Methods
            virtual void prestar();
            virtual void poststar();

        protected:  // typedefs
            GEN_WPDS_TYPEDEFS(T);
            typedef HashMap< wpds_key_t, ConstRuleList > sat_rule_hash_t;

        protected:  // helper functions

            /* apply_pre1 and apply_pre21 need to take a WPDS arg b/c
             * we have 2 wpdses when schemas are involved.  
             *
             * We actually do not
             * need it for apply_pre2b b/c it is just a hash and the
             * rules generated by a schema will make it into that hash
             *
             * FIXME: I am not quite sure about the rule2b but I think it
             * is right.  Come back to this later.
             *
             */

            void apply_pre1(
                    const WPDS<T>& wpds,
                    catrans_t t,
                    const sem_elem_t& delta );

            virtual void apply_pre2a(
                    const WPDS<T>& wpds,
                    catrans_t t,
                    const sem_elem_t& delta );

            virtual void apply_pre2b(
                    const WPDS<T>& wpds,
                    catrans_t t,
                    const sem_elem_t& delta,
                    sat_rule_hash_t& );

            /*
             * Helper functions for poststar.
             * The inline functions are wrappers for their extra argument
             * brothers.  This makes the DWPDS macros work
             */

            void apply_post01_wpds_rule_set(
                    catrans_t t,
                    const sem_elem_t& delta,
                    const RuleListConstIterPair& p );

            void apply_post01(
                    catrans_t t,
                    const sem_elem_t& delta );

            virtual void apply_post2(
                    catrans_t t,
                    const sem_elem_t& delta );

            /*
             * update and update_prime for non witness tracing
             */
            void update(
                    wpds_key_t fstate,
                    wpds_key_t cname,
                    wpds_key_t tstate,
                    const sem_elem_t& se )
            {
                catrans_t t = ca.insert( fstate,cname,tstate,se );

                if( T_MOD_CONTINUE( t,s ) )
                    add_to_workset( t );
            }

            catrans_t update_prime(
                    wpds_key_t fstate,
                    wpds_key_t cname,
                    wpds_key_t tstate,
                    const sem_elem_t& se )
            {
                return ca.insert( fstate,cname,tstate,se );
            }

            /*
             * update and update_prime for witness tracing
             */
            void update(
                    wpds_key_t fstate,
                    wpds_key_t cname,
                    wpds_key_t tstate,
                    const sem_elem_t& se,
                    const witness_t witness )
            {
                // will call s.combine if configuration allready exists
                // Handles *BOTH* DWPDS and regular case
                witness_t wit =
                    (0 != witness.get_ptr()) ? witness : new TransWitness(se);
                catrans_t t = ca.insert( fstate,cname,tstate,se );

                if( T_MOD_CONTINUE( t,s ) )
                {
                    SET_TRANS_WIT(t,wit);
                    add_to_workset( t );
                }

            }

            catrans_t update_prime(
                        wpds_key_t fstate,
                        wpds_key_t cname,
                        wpds_key_t tstate,
                        const sem_elem_t& se,
                        witness_t witness )
            {
                // will call s.combine if configuration allready exists
                // Handles *BOTH* DWPDS and regular case
                catrans_t t = ca.insert( fstate,cname,tstate,se );
                SET_TRANS_WIT(t,witness);
                return t;
            }

        protected:  // Workset methods,defined here for inline
            void populate_workset()
            {
                workset.clear();
                WorksetTransActionFunctor<T> action( *this );
                ca.for_each( action );
            }

            void add_to_workset( const catrans_t& t ) {
                if( !t->is_in_workset() )  {
                    _add_to_workset( t );
                }
            }

#ifdef PRIORITY_WORKLIST
            // For use in the worklist
            struct StackSymLT {
                bool operator()(const catrans_t &a, 
                                const catrans_t &b) const {
                    return 
                        (a->stack() && b->stack())   ? 
                        (a->stack() < b->stack())    :
                        (a->stack() && ! b->stack()) ;
                }
            };
#endif // PRIORITY_WORKLIST


            void _add_to_workset( const catrans_t& t ) {
#ifndef PRIORITY_WORKLIST
                workset.push_back( t );
#else   // PRIORITY_WORKLIST
                workset.insert(t);
#endif  // PRIORITY_WORKLIST
                t->is_in_workset( true );
            }

            catrans_t get_from_workset() {
                catrans_t t;
#ifndef PRIORITY_WORKLIST
                t = workset.back();
                workset.pop_back();
#else   // PRIORITY_WORKLIST
            if(query.is_poststar()) {
                GPP_IMP_TYPENAME__ std::multiset< catrans_t, StackSymLT >::iterator i;
                i = workset.begin();
                t = (*i);
                workset.erase(i);
            } else {
                GPP_IMP_TYPENAME__ std::multiset< catrans_t, StackSymLT >::reverse_iterator i;
                i = workset.rbegin();
                t = (*i);
                workset.erase(t);
            }
#endif  // PRIORITY_WORKLIST

                t->is_in_workset( false ); // mark that we have removed t
                DBGS(numpops++);
                return t;
            }

            bool workset_is_empty() { return workset.empty(); }

            // generates a new state.  only used in poststar for rules
            // of type 2
            wpds_key_t gen_state( const_rule_t r ) const;

        protected:  // Vars
            WPDS<T>& wpds;
            CA<T>& ca;
            Semiring<T> s;
            Query query;
            int numpops;
#ifndef PRIORITY_WORKLIST
            std::vector< catrans_t > workset;
#else   // PRIORITY_WORKLIST
            std::multiset< catrans_t, StackSymLT > workset;
#endif  // PRIORITY_WORKLIST

#ifdef SWPDS

        protected:  // schema typdefs
            typedef GPP_IMP_TYPENAME__ WPDS<T>::wpds_schema_t sat_schema_t;

            // We need this b/c we DO NOT modify the passed in WPDS
            WPDS<T> schema_wpds;

#endif // SWPDS

    };  // class

    /************************************************/
    /* for all rule1s <p1,g1>-><q,g> --> (p1,g1,q') */
    /************************************************/
    template< typename T >
    void SaturationProcess<T>::apply_pre1(
            const WPDS<T>& _wpds,
            catrans_t t,
            const sem_elem_t& delta )
    {

        sem_elem_t se;

        RuleListConstIterPair ruleIterPair =
            _wpds.match_rule_pre( t->key_pair(),Rule<T>::RULE1 );

        RuleListConstIter riter;

        for( riter = ruleIterPair.first;
                riter != ruleIterPair.second; riter++ )
        {
            const_rule_t r = *riter;

            DBGS({
                    std::cout << "\t[prestar] Rule1 match found:\t";
                    r->print(std::cout) << std::endl;
                    });
            // Extend the weights

            se = s.extend( F(r),delta );

            // (p1,g1,q')
            UPDATE( r->from_state(),
                    r->from_stack(),
                    t->to_state(),
                    se,
                    NEW_WITNESS_APPL(se,Query::prestar(),r,t->witness(),0));

        }

    }

    /************************************************/
    /* for all rule2s <p1,g1>-><q,g g2> (Rule2a)    */
    /*  + (q',g2,q'') --> (p1,g1,q'')               */
    /************************************************/
    template< typename T >
    void SaturationProcess<T>::apply_pre2a(
            const WPDS<T>& _wpds,
            catrans_t t,
            const sem_elem_t& delta )
    {
        sem_elem_t se;

        RuleListConstIterPair ruleIterPair =
            _wpds.match_rule_pre( t->key_pair() ,Rule<T>::RULE2 );

        RuleListConstIter riter;

        TransListIterPair tIterPair;
        TransListIter titer;
        
        for( riter = ruleIterPair.first;
                riter != ruleIterPair.second; riter++ )
        {
            const_rule_t r = *riter;

            DBGS({
                    std::cout << "\t[prestar] Rule2a match found:\t";
                    r->print(std::cout) << std::endl;
                    });
            
            tIterPair = ca.match( t->to_state(),r->to_stack2() );

            for( titer = tIterPair.first;
                    titer != tIterPair.second; titer++ )
            {
                // (q',g2,g'')
                catrans_t& tprime = *titer;

                se = s.extend( delta, L(tprime) );
                se = s.extend( F(r),se );

                // (p1,g1,q'')
                UPDATE( r->from_state(),
                        r->from_stack(),
                        tprime->to_state(),se,
                        NEW_WITNESS_APPL(se,Query::prestar(),
                            r,t->witness(), tprime->witness()) );
            }
        }
    }

    /************************************************/
    /* for all rule2s <p1,y1>-><p',g2 g> (Rule2b)   */
    /*  + (p',g2,q)  -->  (p1,g1,q')                */
    /************************************************/
    template< typename T >
    void SaturationProcess<T>::apply_pre2b(
            const WPDS<T>& _wpds,
            catrans_t t,
            const sem_elem_t& delta,
            sat_rule_hash_t& rule2b_map )
    {

        catrans_t tprime;
        sem_elem_t se;

        GPP_IMP_TYPENAME__ sat_rule_hash_t::iterator iter = rule2b_map.find( t->stack() );
        if( iter == rule2b_map.end() )
            return;

        ConstRuleList& ls = iter->second;
        ConstRuleListIter lsiter = ls.begin();
        for( ; lsiter != ls.end(); lsiter++ )
        {

            const Rule<T> *r = *lsiter;

            DBGS({
                    std::cout << "\t[prestar] Rule2b match found:\t";
                    r->print(std::cout) << std::endl;
                    });
            // (p',g2,q)
            if( ca.find(
                    r->to_state(),r->to_stack1(),t->from_state(),tprime) )
            {

                se = s.extend( L(tprime),delta );
                se = s.extend( F(r),se );

                // (p1,g1,q')
                UPDATE( r->from_state(),
                        r->from_stack(),
                        t->to_state(),se,
                        NEW_WITNESS_APPL(se,Query::prestar(),
                            r,tprime->witness(), t->witness()));
            }
        }
    }

    template< typename T > void SaturationProcess<T>::prestar()
    {
        DBGS({
                std::cout << "-----\n\tSaturationProcess::prestar()\n-----\n";
                //std::cout << fa << std::endl;
                });
        // Rule hashes
        sat_rule_hash_t rule2b_map; // hash rules 2 on b hash code

        // what do you think it does?
        populate_workset();

        // FIXME: Lets get rid of this nasty hash table
        // Hash rules of type 2 ( non default hash )
        // We do not use hash_rules b/c hash_codeb is needed..
        MakeRuleHashBActionFunctor<T> b_hasher( rule2b_map );
        wpds.for_each_rule2( b_hasher );

        // Add transitions for rules of type 0
        UpdateRuleActionFunctor<T> apply_pre0( *this );
        wpds.for_each_rule0( apply_pre0 );

        // vars for the chaotic iteration
        catrans_t t,tprime;
        sem_elem_t se;
        sem_elem_t delta;
#ifdef SWPDS
        catrans_t tret;
        TransListIterPair tIterPair;
        TransListIterPair tIterPair2;
        TransListIter titer;
        TransListIter titer2;
        int rule_insert_status;
#endif // SWPDS

        // Chaotic iteration
        while( !workset_is_empty() ) {
            // t = (q,g,q')

            t = get_from_workset();
            delta = DELTA(t);

            DBGS({
                    std::cout << "Popped:\n\t ";
                    t->print(std::cout) << " from workset.\n";
                    });

            /************************************************/
            /* for all rule1s <p1,g1>-><q,g> --> (p1,g1,q') */
            /************************************************/

            APPLY_PRE1( wpds,t,delta );

            /************************************************/
            /* for all rule2s <p1,g1>-><q,g g2> (Rule2a)    */
            /*  + (q',g2,g'') --> (p1,g1,q'')               */
            /************************************************/

            APPLY_PRE2a( wpds,t,delta );

            /************************************************/
            /* for all rule2s <p1,y1>-><p',g2 g> (Rule2b)   */
            /*  + (p',g2,q)  -->  (p1,g1,q')                */
            /************************************************/

            APPLY_PRE2b( wpds,t,delta,rule2b_map );

            /*****************************************************************
             * BEGIN SCHEMAS
             *****************************************************************/
#ifdef SWPDS

            /* Because we have 2 wpdses, we have to do apply_pre1 and
             * apply_pre2 again.
             *
             * FIXME: Again, I do not think pre2b needs to be done
             * again b/c the rules generated share the same hash
             */

            APPLY_PRE1( schema_wpds,t,delta );
            APPLY_PRE2a( schema_wpds,t,delta );
            /**********************************************/
            /* Only generate rules if q is element        */ 
            /* of wpds.P                                  */
            /**********************************************/

            if( !wpds.is_element_of_P(t->to_state()) ) {
                CND_DBGS( DBGSWPDS,
                        {
                        printkey(t->to_state(),std::cerr);
                        std::cerr << " is *NOT* an element of WPDS.P\n";
                        });
                continue;
            }

            // We are breaking the abstraction for efficiency
            GPP_IMP_TYPENAME__ WPDS<T>::ConstSchemaSetHashIter csshi
                = wpds.sdelta.find( t->key_pair() );

            if( csshi == wpds.sdelta.end() )
                continue;

            const GPP_IMP_TYPENAME__ WPDS<T>::Schemas012Triple& trip =
                csshi->second;

            GPP_IMP_TYPENAME__ WPDS<T>::ConstSchemaSetIter schemaIter;
            
            sem_elem_t schema_se;

            /**********************************************/
            /* for all schemas <[q,g]g''> -> <r,eps>      */
            /*      generate rule: <q',g''> -> <r,eps>    */
            /**********************************************/
            for( schemaIter = trip.first.begin();
                    schemaIter != trip.first.end(); schemaIter++ ) {

                // for readability
                const sat_schema_t schema = *schemaIter;

                rule_t r = schema->gen_rule(t);
#ifdef DBGSWPDS
                std::cout << "RS0 generated ";
                r->print(std::cout) << std::endl;
#endif
                rule_insert_status = schema_wpds.p_insert_rule( r,Rule<T>::RULE0 );
                // 0 means rule already existed and was not changed
                // see wpds.h.  This should be fixed sometime soon...so ugly
                if( 0 != rule_insert_status ) {
                    UPDATE( r->from_state(),r->from_stack(),
                            r->to_state(),F(r),0 );
                }
            }

            /**********************************************/
            /* for all schemas <[q,g]g''> -> <r,g'>       */
            /*      generate rule: <q',g''> -> <r,g'>     */
            /**********************************************/
            for( schemaIter = trip.second.begin();
                    schemaIter != trip.second.end(); schemaIter++ ) {

                // for readability
                const sat_schema_t schema = *schemaIter;

                rule_t r = schema->gen_rule(t);
#ifdef DBGSWPDS
                std::cout << "RS1 generated\t";
                r->print(std::cout) << std::endl;
#endif
                rule_insert_status = schema_wpds.p_insert_rule( r,Rule<T>::RULE1 );

                // 0 means rule insertion had no affect on the WPDS
                if( 0 != rule_insert_status ) {

                    // udpate on transitions generated from
                    // <q',g''>-><r,g'> + (r,g',*) ==> (q',g'',*)
                    tIterPair = ca.match( r->to_state(),r->to_stack1() );
                    for( titer = tIterPair.first;
                            titer != tIterPair.second; titer++ )
                    {
                        tret = *titer;

                        schema_se = s.extend( F(r),L(tret) );

                        UPDATE(r->from_state(),
                                r->from_stack(),
                                tret->to_state(),
                                schema_se,
                                NEW_WITNESS_APPL(schema_se,Query::prestar(),
                                    r,t->witness(), 0));
                    }
                }
            }

            /**********************************************/
            /* for all schemas <[q,g]g''> -> <r,g'g'''>   */
            /*      generate rule: <q',g''> -> <r,g'g'''> */
            /**********************************************/
            for( schemaIter = trip.third.begin();
                    schemaIter != trip.third.end(); schemaIter++ ) {

                // for readability
                const sat_schema_t schema = *schemaIter;

                rule_t r = schema->gen_rule(t);

#ifdef DBGSWPDS
                std::cout << "RS2 generated ";
                r->print(std::cout) << std::endl;
#endif
                rule_insert_status = schema_wpds.p_insert_rule( r,Rule<T>::RULE2 );
                // 0 means rule not modified in wpds.  se WPDS::insert_rule
                if( 0 != rule_insert_status ) {
                    // 3 means new rule
                    if( 3 == rule_insert_status ) {
                        GPP_IMP_TYPENAME__ sat_rule_hash_t::iterator iter
                            = rule2b_map.find( r->hash_codeb() );
                        if( iter != rule2b_map.end() ) {
                            iter->second.push_back( r );
                        }
                        else {
                            ConstRuleList ls;
                            ls.push_back( r );
                            rule2b_map.insert( r->hash_codeb(),ls );
                        }
                    }

                    // udpate on transitions generated from
                    // <q',g''>-><r,g'g'''> + (r,g',*) + (*,g''',*2) ==>
                    // (q',g'',*2)
                    tIterPair = ca.match( r->to_state(),r->to_stack1() );
                    for( titer = tIterPair.first;
                            titer != tIterPair.second; titer++ ) {
                        tret = *titer;

                        // for all t' (*,g''',*2)
                        tIterPair2 = ca.match( tret->to_state(),r->to_stack2() ); 
                        for( titer2 = tIterPair2.first;
                                titer2 != tIterPair2.second; titer2++ )
                        {
                            tprime = *titer2;
                            schema_se =
                                s.extend( L(tret), L(tprime) );

                            schema_se =
                                s.extend( F(r),schema_se );

                            UPDATE(r->from_state(),
                                    r->from_stack(),
                                    tprime->to_state(),
                                    schema_se,
                                    NEW_WITNESS_APPL(schema_se,Query::prestar(),
                                        r,tret->witness(), tprime->witness()) );
                        }
                    }

                }
            }
#endif  // SPWDS
        }

#ifdef DBGDWPDS
        ca.print( std::cout ) << std::endl;
#endif // DBGDWPDS

        DBGS({
                std::cerr << "p_hash ";
                ca.p_hash.print_stats( std::cerr );
                std::cerr << "pg_hash ";
                ca.pg_hash.print_stats( std::cerr );
                std::cerr << "pgq_hash ";
                ca.pgq_hash.print_stats( std::cerr );
                });
    }

    /************************************************************/
    /************************************************************/
    /*          POSTSTAR */
    /************************************************************/
    /************************************************************/



    /************************************************************/
    // Rule0s: <p,g>-><p',eps> --> (p',eps,q)
    // Rule1s: <p,g>-><p',g1> --> (p',g1,q)
    /************************************************************/
    template< typename T >
    void SaturationProcess<T>::apply_post01_wpds_rule_set(
            catrans_t t,
            const sem_elem_t& delta,
            const RuleListConstIterPair& p )
    {
        RuleListConstIter iter;
        sem_elem_t se;
        witness_t witness;

        for( iter = p.first ; iter != p.second ; iter++ ) {

            const_rule_t r = *iter;

            DBGS({
                    std::cout << "[poststar] Rule0 match found:\t";
                    r->print(std::cout) << std::endl;
                    });
            // Extend the weights
            se = s.extend( delta,F(r));
            UPDATE( r->to_state(),
                    r->to_stack1(),
                    t->to_state(),
                    se,
                    NEW_WITNESS_APPL(se,Query::poststar(),
                        r, t->witness(),0) );

        }

    }

    template< typename T >
    void SaturationProcess<T>::apply_post01(
            catrans_t t,
            const sem_elem_t& delta )
    {
        apply_post01_wpds_rule_set( t,delta,wpds.match_rule_post0( t->key_pair() ) );
        apply_post01_wpds_rule_set( t,delta,wpds.match_rule_post1( t->key_pair() ) );
    }

    /************************************************************/
    // Rule2s: <p,g>-><p',g'g''> -->
    //      (p',g',q_p'g') && (q_p'g',g'',q)
    /************************************************************/
    template< typename T >
    void SaturationProcess<T>::apply_post2(
            catrans_t t,
            const sem_elem_t& delta )
    {

        sem_elem_t se;
        sem_elem_t quasi;
        witness_t witness;
        catrans_t tprime;

        RuleListConstIterPair p
            = wpds.match_rule_post2( t->key_pair() );

        RuleListConstIter iter;

        for( iter = p.first ; iter != p.second ; iter++ ) {
        
            const_rule_t r = *iter;

            DBGS({
                    std::cout << "[poststar] Rule2 match found:\t";
                    r->print(std::cout) << std::endl;
                    });
            wpds_key_t new_state = gen_state( r );

            // We must add the midstate in case it does not exist
            ca.make_state( new_state );

            se = s.extend( delta,F(r) );

            // For quasione, we must combine se with the quasione
            // of the state if it already existed.  If new_state was
            // truly new, its quasione starts at zero so the combining
            // will result in "se" created above
            quasi = ca.state_quasione( new_state );
            quasi = s.combine( quasi,se );
            ca.state_quasione( new_state, quasi );

            // (p',g',q_p'g')
            UPDATE( r->to_state(),
                    r->to_stack1(),
                    new_state,
                    s.quasiOne( quasi ),
                    NEW_WITNESS_APPL(s.one(),Query::poststar(),
                        r,t->witness(), witness_t(0)) );

            // (q_p'g',g'',q)
            tprime =
                UPDATE_PRIME( new_state,
                        r->to_stack2(),
                        t->to_state(), se,
                        NEW_WITNESS_APPL(se,Query::poststar(),
                            r,t->witness(), witness_t(0)) );

            if( T_MOD_CONTINUE( tprime,s ) )
            {

                TransListIterPair tIterPair;
                TransListIter titer;

                tIterPair = ca.match_eps_to(tprime->from_state());
                for( titer  = tIterPair.first;
                        titer != tIterPair.second; ++titer )
                {

                    catrans_t teps(*titer);

                    sem_elem_t seExtend =
                        s.extend( POST2_SELECT(se,tprime), L(teps) );

                    UPDATE( teps->from_state(),
                            tprime->stack(),
                            tprime->to_state(),
                            seExtend,
                            NEW_WITNESS_EPS(seExtend, 
                                teps->witness(), 
                                tprime->witness()) );
                }
            }
        }
    }

    template< typename T > void SaturationProcess<T>::poststar()
    {
        DBGS({
                WPDS_ERR << "-----\n\tSaturationProcess::poststar()\n-----\n";
                //WPDS_ERR << ca << std::endl;
                });
        populate_workset();

        // Local vars for chaotic iteration
        catrans_t t,tprime;
        sem_elem_t se;
        TransListIterPair tIterPair;
        TransListIter titer;
        witness_t witness;
        sem_elem_t delta;

        // Local variables for allowing rule extensions
        typedef std::pair< wpds_key_t, wpds_key_t > key_pair_t;
        STDSET(key_pair_t) visited;
        key_pair_t sym_stack_pair;
        STDSET(key_pair_t)::const_iterator found;

        // Chaotic iteration
        while( !workset_is_empty() ) {

            // (p,g,q)
            t = get_from_workset();
            delta = DELTA(t);

            DBGS(t->print(WPDS_ERR << "Popped ") << " from workset\n");

            if (wpds.extender)
            {
                // Ask if the pair (from-state,stack-sym has been
                // visited before.  If not, then mark it as so
                // and call the rule-extension method.
                sym_stack_pair.first = t->from_state();
                sym_stack_pair.second = t->transition();
                found = visited.find(sym_stack_pair);
                if (found == visited.end())
                {
                    wpds.extender->add_rules(wpds,
                            sym_stack_pair.first,
                            sym_stack_pair.second);
                    visited.insert(sym_stack_pair);
                }
            }

            if( t->stack() != WPDS_EPSILON ) {
            
                // Rule0s: <p,g>-><p',eps> --> (p',eps,q)
                // Rule1s: <p,g>-><p',g'> --> (p',g',q)

                APPLY_POST01( t,delta );

                // Rule2s: <p,g>-><p',g'g''> -->
                //      (p',g',q_p'g') && (q_p'g',g'',q)

                APPLY_POST2( t,delta );

            }
            else {

                // for all t' = (q,g',q')
                tIterPair = ca.match( t->to_state() );
                for( titer = tIterPair.first;
                        titer != tIterPair.second; titer++ ) {

                    tprime = *titer;
                    DBGS({
                            std::cout << "[poststar] eps trans, think ";
                            tprime->print(std::cout) << "matches...\n";
                            });

                    se = s.extend( L(tprime),delta );

                    // (p,g',q')
                    UPDATE( t->from_state(),
                            tprime->stack(),
                            tprime->to_state(),
                            se,
                            NEW_WITNESS_EPS(se, t->witness(), tprime->witness()) );

                }
            }
        }

        DBGS( std::cerr << "numpops is " << numpops << std::endl );
    }


    /* Generates a new state (currently states are std::strings).
     * Used by postar.  See pseudo code for poststar or comments above */
    template< typename T >
    wpds_key_t SaturationProcess<T>::gen_state( const_rule_t r ) const
    {
            wpds_key_t k1 = r->to_state();
            wpds_key_t k2 = r->to_stack1();
            wpds_key_t k3 = create_key(new key_pair_src(k1,k2));
            return k3;
    }


    /* This wrapper creates the SaturationProcess to perform the prestar.
     * ca_in is copied into ca_out and the resulting automaton of the query
     * will be ca_out.
     */
    template< typename T >
    inline void prestar(
            WPDS<T>& wpds,
            const CA<T>& ca_in,
            Semiring<T>& s,
            CA<T>& ca_out )
    {
        ca_out.operator=( ca_in );
        wpds::SaturationProcess<T> prcs(wpds,ca_out,s,Query::prestar());
        prcs.prestar();
    }

    /* A wrapper function for prestar so the user does not have to
     * worry about SaturationProcesses */
    template< typename T >
    inline CA<T> prestar(
            WPDS<T>& wpds,
            const CA<T>& ca_in,
            Semiring<T>& s )
    {
        CA<T> ca_out( s );
        prestar<T>(wpds,ca_in,s,ca_out );
        return ca_out;
    }

    /* This wrapper creates the SaturationProcess to perform the poststar.
     * ca_in is copied into ca_out and the resulting automaton of the query
     * will be ca_out.
     */
    template< typename T >
    inline void poststar(
            WPDS<T>& wpds,
            const CA<T>& ca_in,
            Semiring<T>& s,
            CA<T>& ca_out )
    {
        ca_out.operator=( ca_in );
        wpds::SaturationProcess<T> prcs(wpds,ca_out,s,Query::poststar());
        prcs.poststar();
    }

    /* A wrapper function for poststar so the user does not have to
     * worry about SaturationProcesses */
    template< typename T >
    inline CA<T> poststar(
            WPDS<T>& wpds,
            const CA<T>& ca_in,
            Semiring<T>& s )
    {
        CA<T> ca_out( s );
        poststar<T>(wpds,ca_in,s,ca_out );
        return ca_out;
    }

}   // wpds

#endif  // wpds_SATURATION_PROCESS_H_
/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
