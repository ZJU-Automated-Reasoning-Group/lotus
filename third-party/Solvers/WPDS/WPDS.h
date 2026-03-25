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

#ifndef wpds_WPDS_
#define wpds_WPDS_ 1

#include <map>
#include <set>
#include <list>
#include <iostream>
#include <algorithm>
#include "common.h"
#include "UTIL.h"
#include "ref_ptr.h"
#include "HashMap.h"
#include "Rule.h"
#ifdef SWPDS
#   include "Schema.h"
#endif // SWPDS
#include "semiring.h"
#include "key_source.h"
#include "keys.h"

/*
 * The below are used in parsing rules and schemas
 * DO NOT EDIT w/out changing parse_(string|rule|schema)
 */
#define RULE_PARSE_STRING \
    "%d) <%[^,>], %[^>]> -> <%[^,>], %[^> ] %[^>] >"
#define RULE_SCHEMA_PARSE_STRING \
        "%d) <[%[^,>], %[^]>]] %[^>]> -> <%[^,], %[^> ] %[^>] >"

namespace wpds
{

    using namespace ::wpds::util;

    template< typename T > class SaturationProcess;
    template< typename T > class RuleExtender;

    template< typename T > class WPDS {

        friend class SaturationProcess<T>;

        public:     // typedefs
#ifdef GRAMMATECH_TRACEBACK
            typedef void (*BTDeleteAction)(T *, wpds_key_t);
#endif
            GEN_WPDS_TYPEDEFS(T);

            // Rule Sets
            typedef std::pair< RuleList,RuleList > Rules12ListPair;

            // Rule0SetHash
            typedef HashMap< KeyPair,RuleList,HashKeyPair,EqualKeyPair > Rule0SetHash;
            typedef GPP_IMP_TYPENAME__ Rule0SetHash::iterator Rule0SetHashIter;
            typedef GPP_IMP_TYPENAME__ Rule0SetHash::const_iterator ConstRule0SetHashIter;

            // Rules12SetHash
            typedef HashMap< KeyPair,Rules12ListPair,HashKeyPair,EqualKeyPair > Rules12SetHash;
            typedef GPP_IMP_TYPENAME__ Rules12SetHash::iterator Rules12SetHashIter;
            typedef GPP_IMP_TYPENAME__ Rules12SetHash::const_iterator ConstRules12SetHashIter;

#ifdef SWPDS
            typedef Schema<T> *wpds_schema_t;

            // Schema Sets
            typedef STDLIST(wpds_schema_t) SchemaSet;
            typedef GPP_IMP_TYPENAME__ SchemaSet::iterator SchemaSetIter;
            typedef GPP_IMP_TYPENAME__ SchemaSet::const_iterator ConstSchemaSetIter;
            typedef std::pair< SchemaSetIter,SchemaSetIter > RuleSchemaSetIterPair;
            typedef std::pair< ConstSchemaSetIter,ConstSchemaSetIter > ConstSchemaSetIterPair;

            // Schema Hashes
            typedef Triple< SchemaSet,SchemaSet,SchemaSet > Schemas012Triple;
            typedef HashMap< KeyPair,Schemas012Triple,HashKeyPair,EqualKeyPair > SchemaSetHash;
            typedef GPP_IMP_TYPENAME__ SchemaSetHash::iterator SchemaSetHashIter;
            typedef GPP_IMP_TYPENAME__ SchemaSetHash::const_iterator ConstSchemaSetHashIter;
#endif // SWPDS

        /****************************************************/
        /*                      METHODS                     */
        /****************************************************/
        protected:     // methods

            /* Constructor / Destructor*/
            WPDS( ) : s(0), query(Query::both()),extender(NULL) {}
        public:
            WPDS( Semiring<T>& s_,
                    Query q = Query::both())
                : s(s_),query(q),extender(NULL) {}

            // The constructor takes away ownership of extender__
            WPDS( Semiring<T>& s_,
                    RuleExtender<T> *extender__,
                    Query q = Query::both())
                : s(s_),query(q),extender(extender__) {}

            virtual ~WPDS() 
            {
                DeleteRuleActionFunctor<T> deleter;
                for_each_rule( deleter );
#ifdef SWPDS
                DeleteSchemaActionFunctor<T> sDeleter;
                for_each_schema( sDeleter );
#endif
                if (extender)
                    delete extender;
            }

            /* Add rule methods
               <p,y> -> <q,eps>
               <p,y> -> <q,g1>
               <p,y> -> <q,g1 g2>
             */
            void add_rule(
                    const wpds_key_t p,
                    const wpds_key_t y,
                    const wpds_key_t q,
                    T *t)
            {
                rule_t r = new Rule<T>(t,p,y,q);
                p_insert_rule( r,Rule<T>::RULE0 );
            }

            void add_rule(
                    const wpds_key_t p, const wpds_key_t y,
                    const wpds_key_t q, const wpds_key_t g1,
                    T *t)
            {
                if( g1 != WPDS_EPSILON ) {
                    rule_t r = new Rule<T>(t,p,y,q,g1);
                    p_insert_rule( r,Rule<T>::RULE1 );
                } /* someone did not follow the calling convention...*/
                else
                    add_rule(p,y,q,t);
            }

            void add_rule(
                    const wpds_key_t p, const wpds_key_t y,
                    const wpds_key_t q, const wpds_key_t g1,
                    const wpds_key_t g2, T *t)
            {
                if( g2 != WPDS_EPSILON ) {
                    rule_t r = new Rule<T>(t,p,y,q,g1,g2);
                    p_insert_rule( r,Rule<T>::RULE2 );
                } /* someone did not follow the calling convention...*/
                else
                    add_rule(p,y,q,g1,t);
            }


            // check to see if state q is element of P
            bool is_element_of_P( const wpds_key_t q ) const
            {
                return (P.find(q) != P.end());
            }

            // Getting statistics
            wpds_size_t count_rules () const {
                if( query.is_prestar() )
                    return (rules0Hash.size() + delta_pre.size());
                else
                    return (rules0Hash.size() + delta_post.size());
            } // count_rules

        protected:  // methods

            int p_insert_rule_set( rule_t r, RuleList& rs );

            int p_insert_rule0_hash( rule_t r );

            int p_insert_rule12_hash( rule_t r,
                    GPP_IMP_TYPENAME__ Rule<T>::type index,
                    KeyPair& kp,Rules12SetHash& hash );

            int p_insert_rule( rule_t r,GPP_IMP_TYPENAME__ Rule<T>::type );


        public:
            RuleListConstIterPair match_rule0( const KeyPair& kp ) const;

            RuleListConstIterPair match_rule12(
                    const KeyPair& kp,
                    GPP_IMP_TYPENAME__ Rule<T>::type index,
                    const Rules12SetHash& hash ) const;

            RuleListConstIterPair match_rule_pre(
                    const KeyPair& kp,
                    GPP_IMP_TYPENAME__ Rule<T>::type index ) const;

            RuleListConstIterPair match_rule_pre0( const KeyPair& kp ) const
            {
                return match_rule_pre( kp,Rule<T>::RULE0 );
            }

            RuleListConstIterPair match_rule_pre1( const KeyPair& kp ) const
            {
                return match_rule_pre( kp,Rule<T>::RULE1 );
            }

            RuleListConstIterPair match_rule_pre2( const KeyPair& kp )
            {
                return match_rule_pre( kp,Rule<T>::RULE2 );
            }

            RuleListConstIterPair match_rule_post(
                    const KeyPair& kp,
                    GPP_IMP_TYPENAME__ Rule<T>::type index ) const;

            RuleListConstIterPair match_rule_post0( const KeyPair& kp ) const
            {
                return match_rule_post( kp,Rule<T>::RULE0 );
            }

            RuleListConstIterPair match_rule_post1( const KeyPair& kp ) const
            {
                return match_rule_post( kp,Rule<T>::RULE1 );
            }

            RuleListConstIterPair match_rule_post2( const KeyPair& kp ) const
            {
                return match_rule_post( kp,Rule<T>::RULE2 );
            }

#ifdef GRAMMATECH_TRACEBACK
            void remove_bt(const KeyPair &kp, wpds_key_t i_fromStateKey, BTDeleteAction action);
#endif

            void add_element_to_P( const wpds_key_t p )
            {
                P.insert( p,p );
            }


            void for_each_rule(  RuleActionFunctor<T>& action ) const;
            void for_each_rule0( RuleActionFunctor<T>& action ) const;
            void for_each_rule1( RuleActionFunctor<T>& action ) const;
            void for_each_rule2( RuleActionFunctor<T>& action ) const;


        public:     // other methods

            std::ostream& print( std::ostream& ) const;

            bool parse_string( char *buf );

            bool parse_rule( const char *buf,const char *sem_elem_start );

            bool parse_schema( const char *buf, const char *sem_elem_start );

            void p_bad_parse_error_msg( const char *buf ) const;

            /****************************************************/
            /*                      VARS                        */
            /****************************************************/
        private:    // vars
            Semiring<T> s;
            /*! Query is the type of query being performed
             * If we know ahead of time that we will only be doing
             * a pre or poststar then we can not keep as much information 
             * in the hash tables.
             */
            Query query;

            // Delta holds the rules
            Rule0SetHash rules0Hash;
            Rules12SetHash delta_pre;
            Rules12SetHash delta_post;

        protected :
            // For extending the set of rules
            RuleExtender< T > *extender;
            // This is a quick hack b/c we do not have
            // empty references
            RuleList EMPTY_RULE_SET;
        private:
            // P is the set of states of the PDS
            // We really want a HashSet but that is
            // not currently available :)
            HashMap< wpds_key_t,wpds_key_t > P;


            /*********************************/
            /****** BEGIN SCHEMA DECLS *******/
            /*********************************/

#ifdef SWPDS
        public:
            /* Add rule schema methods
               <[p,y] y'> -> <q, eps>
               <[p,y] y'> -> <q, g1>
               <[p,y] y'> -> <q, g1 g2>
             */
            void add_schema(
                    const wpds_key_t p, const wpds_key_t y,
                    const wpds_key_t yp, const wpds_key_t q, T *t)
            {
                wpds_schema_t schema = new Schema<T>(t,p,y,yp,q);
                p_insert_schema( schema,Schema<T>::SCHEMA0 );
            }

            void add_schema(
                    const wpds_key_t p, const wpds_key_t y,
                    const wpds_key_t yp, const wpds_key_t q,
                    const wpds_key_t g1, T *t)
            {
                if( g1 != WPDS_EPSILON ) {
                    wpds_schema_t schema = new Schema<T>(t,p,y,yp,q,g1);
                    p_insert_schema( schema,Schema<T>::SCHEMA1 );
                }
                else
                    add_schema(p,y,yp,q,t);
            }

            void add_schema(
                    const wpds_key_t p, const wpds_key_t y,
                    const wpds_key_t yp, const wpds_key_t q,
                    const wpds_key_t g1, const wpds_key_t g2,
                    T *t)
            {
                if( g2 != WPDS_EPSILON ) {
                    wpds_schema_t schema = new Schema<T>(t,p,y,yp,q,g1,g2);
                    p_insert_schema( schema,Schema<T>::SCHEMA2 );
                }
                else
                    add_schema(p,y,yp,q,g1,t);
            }

            void for_each_schema( SchemaActionFunctor<T>& action ) const;
            void for_each_schema0( SchemaActionFunctor<T>& action ) const;
            void for_each_schema1( SchemaActionFunctor<T>& action ) const;
            void for_each_schema2( SchemaActionFunctor<T>& action ) const;

        private:
            bool p_insert_schema_set( wpds_schema_t schema, SchemaSet& s );
            void p_insert_schema( wpds_schema_t,GPP_IMP_TYPENAME__ Schema<T>::type );

            void for_each_schema_set( 
                    SchemaActionFunctor<T>& action,const SchemaSet& sSet ) const;

            /* Use 012 if you just want schemas of that type */
            ConstSchemaSetIterPair match_schema0( KeyPair& kp ) const;
            ConstSchemaSetIterPair match_schema1( KeyPair& kp ) const;
            ConstSchemaSetIterPair match_schema2( KeyPair& kp ) const;

        private:    // vars

            SchemaSetHash sdelta;
            // like rules, we need this for null refs
            SchemaSet EMPTY_SCHEMA_SET;

#endif // SWPDS

    };  // class WPDS

    /******************************************************************/
    /******************************************************************/
    /*************         PUBLIC METHODS               ***************/
    /******************************************************************/
    /******************************************************************/

    template< typename T >
    std::ostream& WPDS<T>::print( std::ostream& o ) const
    {
        WriteRuleActionFunctor<T> writer( o );

        o << "WPDS:\n\tP ==> { ";

        // P
        GPP_IMP_TYPENAME__ HashMap< wpds_key_t,wpds_key_t >::const_iterator pit = P.begin();
        for( ; pit != P.end() ; pit++ ) {
            o << ", ";
            printkey(pit->second,o);
        }

        // Rules
        o <<  " }\n  Rules:\n";
        for_each_rule( writer );

#ifdef SWPDS
        //Schemas
        WriteSchemaActionFunctor<T> sWriter(o);
        o <<  "  Schemas:\n";
        for_each_schema( sWriter );
#endif
        return o;
    }



    /******************************************************************/
    /******************************************************************/
    /*************         PRIVATE METHODS              ***************/
    /******************************************************************/
    /******************************************************************/

    // insert_rule
    // returns true if a new rule was added or the rule's SemiringElement
    // was modified.  False otherwise.
    //
    // FIXME: The return value is used b/c we generate rules on the fly.
    //      However, we also need to be able to distinguish between new rules
    //      and modified rules for the purposes of correctly filling hash
    //      tables used for indexing rules during pre/poststar For now, this
    //      is done using an int return value.
    //
    //      0 == not modified
    //      1 == old rule, but SemiringElement has changed
    //      3 == new rule
    //
    //      FIXME: make the above an enum!!!

    template< typename T >
    int WPDS<T>::p_insert_rule_set(
            rule_t r,
            GPP_IMP_TYPENAME__ WPDS<T>::RuleList& rs )
    {
        int retval = 3;
        rule_t rchk;
        
        // If you know that you will never add a rule
        // twice, then you can turn this off.  It might
        // be wise to just have this on for Schemas
        RuleListIter rsi = rs.begin();
        for( ; rsi != rs.end() ; rsi++ ) {
            rchk = *rsi;
            if( r->equal( rchk ) ) {
                /*
#ifdef DBGWPDS
                std::cout << "\tInsert RuleList Match\n";
                r->print(std::cout<<"\t\t") << std::endl;
                rchk->print(std::cout<<"\t\t") << std::endl;
#endif
                 */
                if( s.equal( r->se,rchk->se ) ) {
                    retval = 0;
                }
                else {
                    retval = 1;
                    rchk->se = s.combine( r->se,rchk->se );
                }
                break;
            }
        }
        // It was not found
        if( retval == 3 ) {
            rs.push_back( r );
        }
        return retval;
    }

    template< typename T >
    int WPDS<T>::p_insert_rule0_hash( rule_t r )
    {
        KeyPair kp( r->from_state(),r->from_stack() );
        Rule0SetHashIter r0shi = rules0Hash.find( kp );
        int retval;

        if( r0shi == rules0Hash.end() ) {
            RuleList rs;
            rs.push_back( r );
            rules0Hash.insert( kp,rs );
            retval = 3;
        }
        else {
            retval = p_insert_rule_set( r,r0shi->second );
        }
        return retval;

    }

    template< typename T >
    int WPDS<T>::p_insert_rule12_hash(
            rule_t r, 
            GPP_IMP_TYPENAME__ Rule<T>::type index,
            KeyPair& kp,
            GPP_IMP_TYPENAME__ WPDS<T>::Rules12SetHash& hash )
    {
        Rules12SetHashIter r12shi = hash.find( kp );
        int retval = 3;
        if( r12shi == hash.end() ) {
            RuleList rs1;
            RuleList rs2;
            if( index == Rule<T>::RULE1 ) {
                rs1.push_back( r );
            }
            else {
                rs2.push_back( r );
            }
            hash.insert( kp, Rules12ListPair( rs1,rs2 ) );
        }
        else {
            /*
#ifdef DBGWPDS
            printf("\tKeyPair (%s,%s) already present.\n",
                    key2str(kp.first),key2str(kp.second));
#endif
             */
            if( index == Rule<T>::RULE1 ) {
                retval = p_insert_rule_set( r,r12shi->second.first );
            }
            else {
                retval = p_insert_rule_set( r,r12shi->second.second );
            }
        }
        return retval;

    }

    template< typename T >
    int WPDS<T>::p_insert_rule(
            rule_t r, GPP_IMP_TYPENAME__ Rule<T>::type index )
    {
        int preval = 0,postval = 0,retval;
        /*
#ifdef DBGWPDS
        printf("p_insert_rule   :  type = %d\n",index);
#endif
         */
        if( index != Rule<T>::RULE0 ) {



            if( query.is_prestar() ) {
                KeyPair kp( r->to_state(),r->to_stack1() );
                preval = p_insert_rule12_hash( r,index,kp,delta_pre );
            }

            if( query.is_poststar() ) {
                KeyPair kp( r->from_state(),r->from_stack() );
                postval = p_insert_rule12_hash( r,index,kp,delta_post );
            }

            if( query.is_both() && preval == 3 ) {
                // SANITY check.
                // if the rule was new in the pre hashsets
                // it better be new in the post hashsets
                assert( preval == postval );
            }

            retval = preval | postval;
        }
        else {
            retval = p_insert_rule0_hash( r );
        }
        if( retval != 3 ) {
            // We delete the rule if it existed b/c it is
            // "new"d in add_rule.  This should be the exception
            delete r;
        }
        else {
            add_element_to_P( r->from_state() );
            add_element_to_P( r->to_state() );
        }
        return retval;
    }

    template< typename T >
    GPP_IMP_TYPENAME__ WPDS<T>::RuleListConstIterPair
    WPDS<T>::match_rule0( const KeyPair& kp ) const
    {
        RuleListConstIterPair p;
        ConstRule0SetHashIter r0shi = rules0Hash.find( kp ); 

        if( r0shi != rules0Hash.end() ) {
            p = RuleListConstIterPair(
                    r0shi->second.begin(),r0shi->second.end() );
        }
        else {
            p = RuleListConstIterPair( EMPTY_RULE_SET.end(),EMPTY_RULE_SET.end() );
        }
        return p;
    }

    template< typename T >
    GPP_IMP_TYPENAME__ WPDS<T>::RuleListConstIterPair
    WPDS<T>::match_rule12( const KeyPair& kp,
            GPP_IMP_TYPENAME__ Rule<T>::type index,
            const GPP_IMP_TYPENAME__ WPDS<T>::Rules12SetHash& hash ) const
    {
        RuleListConstIterPair p;
        ConstRules12SetHashIter r12shi = hash.find( kp );

        if( r12shi != hash.end() ) {
            const Rules12ListPair& r12SetPair = r12shi->second;
            if( index == Rule<T>::RULE1 ) {
                const RuleList& l_s = r12SetPair.first;
                p = RuleListConstIterPair( l_s.begin(),l_s.end() );
            }
            else {
                const RuleList& l_s = r12SetPair.second;
                p = RuleListConstIterPair( l_s.begin(),l_s.end() );
            }
        }
        else {
            p = RuleListConstIterPair( EMPTY_RULE_SET.end(),EMPTY_RULE_SET.end() );
        }
        return p;
    }

    template< typename T >
    GPP_IMP_TYPENAME__ WPDS<T>::RuleListConstIterPair
    WPDS<T>::match_rule_pre( const KeyPair& kp,
            GPP_IMP_TYPENAME__ Rule<T>::type index ) const
    {

        if( index == Rule<T>::RULE0 ) {
            return match_rule0( kp );
        }
        else {
            return match_rule12( kp,index,delta_pre );
        }

    }

    template< typename T >
    GPP_IMP_TYPENAME__ WPDS<T>::RuleListConstIterPair
    WPDS<T>::match_rule_post( const KeyPair& kp,
            GPP_IMP_TYPENAME__ Rule<T>::type index ) const
    {

        if( index == Rule<T>::RULE0 ) {
            return match_rule0( kp );
        }
        else {
            return match_rule12( kp,index,delta_post );
        }

    }

#ifdef GRAMMATECH_TRACEBACK
    template< typename T >
    void WPDS<T>::remove_bt(
            const KeyPair &kp,
            wpds_key_t i_fromStateKey,
            BTDeleteAction action)
    {
        WriteRuleActionFunctor<T> writer( std::cout );
        RuleListIter iter;
        Rule0SetHashIter r0shi = rules0Hash.find(kp);
        if(r0shi != rules0Hash.end()) {
            for(iter = r0shi->second.begin(); iter != r0shi->second.end(); iter ++) {
                rule_t r = *iter;
                DBGS(writer(r));
                action(r->se.get_ptr(),i_fromStateKey);
                DBGS(writer(r));
            }
        }
        assert( query.is_poststar() );
        Rules12SetHashIter r12shi = delta_post.find(kp);
        if(r12shi != delta_post.end()) {
            for(iter = r12shi->second.first.begin(); iter != r12shi->second.first.end(); iter ++) {
                rule_t r = *iter;
                DBGS(writer(r));
                action(r->se.get_ptr(),i_fromStateKey);
                DBGS(writer(r));
            }
            for(iter = r12shi->second.second.begin(); iter != r12shi->second.second.end(); iter ++) {
                rule_t r = *iter;
                DBGS(writer(r));
                action(r->se.get_ptr(),i_fromStateKey);
                DBGS(writer(r));
            }
        }
    }
#endif

    template< typename T >
    void WPDS<T>::for_each_rule( RuleActionFunctor<T>& action ) const
    {
        for_each_rule0( action );
        for_each_rule1( action );
        for_each_rule2( action );
    }

    template< typename T >
    void WPDS<T>::for_each_rule0( RuleActionFunctor<T>& action ) const
    {
        ConstRule0SetHashIter hashIter = rules0Hash.begin();
        RuleListConstIter lsIter;
        for( ; hashIter != rules0Hash.end(); hashIter++ ) {
            for( lsIter = hashIter->second.begin();
                    lsIter != hashIter->second.end(); lsIter++ ) {
                const_rule_t r = *lsIter;
                action( r );
            }
        }
    }

    template< typename T >
    void WPDS<T>::for_each_rule1( RuleActionFunctor<T>& action ) const
    {
        ConstRules12SetHashIter hashIter = 
            ( query.is_prestar() ) ? delta_pre.begin() : delta_post.begin();
        ConstRules12SetHashIter endIter = 
            ( query.is_prestar() ) ? delta_pre.end() : delta_post.end();
        RuleListConstIter lsIter;
        for( ; hashIter != endIter ; hashIter++ ) {
            const Rules12ListPair& r12Pair = hashIter->second;
            for( lsIter = r12Pair.first.begin();
                    lsIter != r12Pair.first.end(); lsIter++ ) {
                const_rule_t r = *lsIter;
                action( r );
            }
        }
    }

    template< typename T >
    void WPDS<T>::for_each_rule2( RuleActionFunctor<T>& action ) const
    {
        ConstRules12SetHashIter hashIter = 
            ( query.is_prestar() ) ? delta_pre.begin() : delta_post.begin();
        ConstRules12SetHashIter endIter = 
            ( query.is_prestar() ) ? delta_pre.end() : delta_post.end();
        RuleListConstIter lsIter;
        for( ; hashIter != endIter ; hashIter++ ) {
            const Rules12ListPair& r12Pair = hashIter->second;
            for( lsIter = r12Pair.second.begin();
                    lsIter != r12Pair.second.end(); lsIter++ ) {
                const_rule_t r = *lsIter;
                action( r );
            }
        }
    }




    /******************************************************************/
    /******************************************************************/
    /*************         OTHER METHODS              *****************/
    /******************************************************************/
    /******************************************************************/

    template< typename T >
    bool WPDS<T>::parse_string( char *buf )
    {
        char *tmp = 0;
        char *tmp2 = 0;
        char hold_arrow_start;
        int i=0;
        tmp = strstr( buf, "->" );
        if( !tmp ) {
            p_bad_parse_error_msg( buf );
            return false;
        }
        hold_arrow_start = *tmp;
        *tmp = 0;
        tmp2 = strchr( buf, '[' );
        *tmp = hold_arrow_start;
        /* Any rule or schema (for the current print/readout)
         * must have 3 '>' symbols.  Lets make sure of that and 
         * pass the index of the 3rd one to the p_ methods
         */
        tmp = buf;
        for(;i<3;i++) {
            if( NULL != (tmp = strchr(tmp,'>')) ) {
                tmp++;
            }
            else {
                p_bad_parse_error_msg( buf );
                return false;
            }
        }
        //printf("tmp --> %s\n",tmp);
        if( tmp2 )
            return parse_schema( buf,tmp );
        else
            return parse_rule( buf,tmp );
    } 

    /*
     * p_parse_rule & p_parse_schema
     *
     * Parses a rule/schema out of the buf according to the above
     * defined parse rules (see top of this file).
     *
     * sem_elem_start points to the first character after the rule/schema
     * so that we may call s.parse_element( sem_elem_start ).  This should
     * return a semiring element (a nice way of saying T* )
     */

    template< typename T >
    bool WPDS<T>::parse_rule( const char *buf,const char *sem_elem_start )
    {
        int rulenum;
        char fstate[1024],fstack[1024];
        char tstate[1024],tstack1[1024],tstack2[1024];
        int rs;
        GPP_IMP_TYPENAME__ Rule<T>::type rule_type;
        rule_t rule;
        wpds_key_t fstt = WPDS_EPSILON,
            fstk = WPDS_EPSILON,
            tstt = WPDS_EPSILON,
            tstk1 = WPDS_EPSILON,
            tstk2 = WPDS_EPSILON;
        memset(fstate,0,sizeof(fstate));
        memset(fstack,0,sizeof(fstack));
        memset(tstate,0,sizeof(tstate));
        memset(tstack1,0,sizeof(tstack1));
        memset(tstack2,0,sizeof(tstack2));

        rs = sscanf( buf,RULE_PARSE_STRING,
                &rulenum,fstate,fstack,tstate,tstack1,tstack2);
        switch( rs ) {
            case 6:
                rule_type = Rule<T>::RULE2;
                tstk2 = str2key(tstack2);
                tstk1 = str2key(tstack1);
                break;
            case 5:
                rule_type = Rule<T>::RULE1;
                tstk1 = str2key(tstack1);
                break;
            case 4:
                rule_type = Rule<T>::RULE0;
                break;
            default:
                p_bad_parse_error_msg( buf );
                return false;
        }
        fstt = str2key(fstate);
        fstk = str2key(fstack);
        tstt = str2key(tstate);
        rule = new Rule<T>(
                s.parse_element(sem_elem_start),
                fstt,fstk,tstt,tstk1,tstk2 );
        p_insert_rule( rule,rule_type );
        return true;
    }


    template< typename T >
    void WPDS<T>::p_bad_parse_error_msg( const char *buf ) const
    {
        if( b_wpds_parse_debug ) {
            fprintf(stderr,"WPDS: Invalid input string for Rule(Schema).\n");
            fprintf(stderr,"\t%s\n",buf);
            fprintf(stderr,"WPDS: Rule(Schema) input format:\n");
            fprintf(stderr,"\tRule:\t\t%s\n",RULE_PARSE_STRING);
            fprintf(stderr,"\tRuleSchema:\t%s\n",RULE_SCHEMA_PARSE_STRING);
        }
    }

    /************************************************************/
    /************************************************************/
    /*                                                          */
    /*              SCHEMAS                                     */
    /*                                                          */
    /************************************************************/
    /************************************************************/

    /*
     * This gets defined no matter what b/c of the way parse_string
     * works.  Perhaps a better ifdef scheme can be thought of
     */
    template< typename T >
    bool WPDS<T>::parse_schema( const char *buf,const char *sem_elem_start )
    {
#ifndef SWPDS
        // It is an error to call this if schemas
        // are not defined

        fprintf(stderr,"***I think I see a Schema but schemas are not compiled in?\n");
        p_bad_parse_error_msg(buf);
        return false;
#else
        GPP_IMP_TYPENAME__ Schema<T>::type schema_type;
        int rulenum;
        char mstate[1024],mstack[1024],fstack[1024];
        char tstate[1024],tstack1[1024],tstack2[1024];
        int rs;
        wpds_schema_t schema;
        wpds_key_t mstt = WPDS_EPSILON,
            mstk = WPDS_EPSILON,
            fstk = WPDS_EPSILON,
            tstt = WPDS_EPSILON,
            tstk1 = WPDS_EPSILON,
            tstk2 = WPDS_EPSILON;
        memset(mstate,0,sizeof(mstate));
        memset(mstack,0,sizeof(mstack));
        memset(fstack,0,sizeof(fstack));
        memset(tstate,0,sizeof(tstate));
        memset(tstack1,0,sizeof(tstack1));
        memset(tstack2,0,sizeof(tstack2));
        rs = sscanf( buf,RULE_SCHEMA_PARSE_STRING,
                &rulenum,mstate,mstack,fstack,tstate,tstack1,tstack2);
        switch( rs ) {
            case 5:
                //printf("RuleSchema 0(%s, %s, %s, %s )\n", mstate,mstack,fstack,tstate);
                schema_type = Schema<T>::SCHEMA0;
                break;

            case 6:
                //printf("RuleSchema 1(%s, %s, %s, %s, %s )\n", mstate,mstack,fstack,tstate,tstack1);
                schema_type = Schema<T>::SCHEMA1;
                tstk1 = str2key(tstack1);
                break;

            case 7:
                //printf("RuleSchema 2(%s, %s, %s, %s, %s, %s )\n", mstate,mstack,fstack,tstate,tstack1,tstack2);
                schema_type = Schema<T>::SCHEMA2;
                tstk1 = str2key(tstack1);
                tstk2 = str2key(tstack2);
                break;

            default:
                p_bad_parse_error_msg( buf );
                return false;
        }
        mstt = str2key(mstate);
        mstk = str2key(mstack);
        fstk = str2key(fstack);
        tstt = str2key(tstate);
        schema = new Schema<T>(
                s.parse_element(sem_elem_start),
                mstt,mstk,fstk,tstt,tstk1,tstk2 );
        p_insert_schema( schema,schema_type );
        return true;
#endif  // SWPDS
    }

#ifdef SWPDS

    template< typename T >
    bool WPDS<T>::p_insert_schema_set(
            wpds_schema_t schema,
            WPDS<T>::SchemaSet& schemaSet )
    {
        SchemaSetIter iter = schemaSet.begin();
        wpds_schema_t schk;
        for( ; iter != schemaSet.end() ; iter++ ) {
            schk = *iter;

            if( schema->equal( schk ) ) {
                schk->se = s.combine( schema->se,schk->se );
            }

        }

        if( iter == schemaSet.end() ) {
#ifdef DBGSWPDS
            schema->print( std::cout << "\tAdding : " ) << std::endl;
#endif
            schemaSet.push_back( schema );
            return false;
        }
        else {
            // Must delet b/c new'd in add_schema
#ifdef DBGSWPDS
            printf("Schema Existed...deleting\n");
#endif
            delete schema;
            return true;
        }
    }

    template< typename T >
    void WPDS<T>::p_insert_schema(
            wpds_schema_t schema,
            GPP_IMP_TYPENAME__ Schema<T>::type index )
    {
        KeyPair kp = schema->key_pair();
        SchemaSetHashIter sshi = sdelta.find( kp );
        bool exists = false;
        if( sshi == sdelta.end() ) {
            SchemaSet s1;
            SchemaSet s2;
            SchemaSet s3;
            //Schemas012Triple trip( s1,s2,s3 );
            switch( index ) {
                case Schema<T>::SCHEMA0:
                    s1.push_back( schema );
                    break;
                case Schema<T>::SCHEMA1:
                    s2.push_back( schema );
                    break;
                case Schema<T>::SCHEMA2:
                    s3.push_back( schema );
                    break;
                case Schema<T>::MAX:
                    fprintf(stderr,"Schema MAX??\n");
                    break;
            }
            sdelta.insert( kp,Schemas012Triple(s1,s2,s3) );
        }
        else {
            Schemas012Triple& trip = sshi->second;
            switch( index ) {
                case Schema<T>::SCHEMA0:
                    exists = p_insert_schema_set( schema,trip.first );
                    break;
                case Schema<T>::SCHEMA1:
                    exists = p_insert_schema_set( schema,trip.second );
                    break;
                case Schema<T>::SCHEMA2:
                    exists = p_insert_schema_set( schema,trip.third );
                    break;
                case Schema<T>::MAX:
                    fprintf(stderr,"Schema MAX??\n");
                    break;
            }
        }
        /* FIXME: decide if we want this
         *
         * This is probably not a good idea b/c we really only care
         * about rule states and they get added to P when a rule
         * is generated from a schema.
         */
        if( !exists ) {
            add_element_to_P( schema->match_state() );
            add_element_to_P( schema->to_state() );
        }
    }

    template< typename T >
    void WPDS<T>::for_each_schema_set( 
            SchemaActionFunctor<T>& action,const SchemaSet& sSet ) const
    {
        ConstSchemaSetIter setIter;
        for( setIter = sSet.begin() ; setIter != sSet.end() ; setIter++ )
            action( *setIter );
    }

    template< typename T >
    void WPDS<T>::for_each_schema( SchemaActionFunctor<T>& action ) const
    {
        for_each_schema0( action );
        for_each_schema1( action );
        for_each_schema2( action );
    }

    template< typename T >
    void WPDS<T>::for_each_schema0( SchemaActionFunctor<T>& action ) const
    {
        ConstSchemaSetHashIter sshi = sdelta.begin();

        for( ; sshi != sdelta.end() ; sshi++ ) {
            const Schemas012Triple& trip = sshi->second;
            for_each_schema_set( action,trip.first );
        }

    }

    template< typename T >
    void WPDS<T>::for_each_schema1( SchemaActionFunctor<T>& action ) const
    {
        ConstSchemaSetHashIter sshi = sdelta.begin();

        for( ; sshi != sdelta.end() ; sshi++ ) {
            const Schemas012Triple& trip = sshi->second;
            for_each_schema_set( action,trip.second );
        }

    }

    template< typename T >
    void WPDS<T>::for_each_schema2( SchemaActionFunctor<T>& action ) const
    {
        ConstSchemaSetHashIter sshi = sdelta.begin();

        for( ; sshi != sdelta.end() ; sshi++ ) {
            const Schemas012Triple& trip = sshi->second;
            for_each_schema_set( action,trip.third );
        }

    }

    /* Use 012 if you just want schemas of that type */
    template< typename T >
    GPP_IMP_TYPENAME__ WPDS<T>::ConstSchemaSetIterPair 
    WPDS<T>::match_schema0( KeyPair& kp ) const
    {
        ConstSchemaSetHashIter csshi = sdelta.find( kp );
        if( csshi != sdelta.end() ) {
            const Schemas012Triple& trip = csshi->second;
            return ConstSchemaSetIterPair( trip.first.begin(),trip.first.end() );
        }
        else {
            return ConstSchemaSetIterPair(
                    EMPTY_SCHEMA_SET.end(), EMPTY_SCHEMA_SET.end() );
        }
    }

    template< typename T >
    GPP_IMP_TYPENAME__ WPDS<T>::ConstSchemaSetIterPair 
    WPDS<T>::match_schema1( KeyPair& kp ) const
    {
        ConstSchemaSetHashIter csshi = sdelta.find( kp );
        if( csshi != sdelta.end() ) {
            const Schemas012Triple& trip = csshi->second;
            return ConstSchemaSetIterPair( trip.second.begin(),trip.second.end() );
        }
        else {
            return ConstSchemaSetIterPair(
                    EMPTY_SCHEMA_SET.end(),
                    EMPTY_SCHEMA_SET.end() );
        }
    }

    template< typename T >
    GPP_IMP_TYPENAME__ WPDS<T>::ConstSchemaSetIterPair 
    WPDS<T>::match_schema2( KeyPair& kp ) const
    {
        ConstSchemaSetHashIter csshi = sdelta.find( kp );
        if( csshi != sdelta.end() ) {
            const Schemas012Triple& trip = csshi->second;
            return ConstSchemaSetIterPair( trip.third.begin(),trip.third.end() );
        }
        else {
            return ConstSchemaSetIterPair(
                    EMPTY_SCHEMA_SET.end(),EMPTY_SCHEMA_SET.end() );
        }
    }

#endif  // SWPDS

    
    // helper print
    template< typename T >
    std::ostream& operator<<(std::ostream& o, const WPDS<T>& wpds )
    {
        return wpds.print(o);
    }

}   // namespace wpds

#include "RuleExtender.h"
#include "SaturationProcess.h" // include at bottom b/c of cycle
#endif  // wpds_WPDS_

/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
