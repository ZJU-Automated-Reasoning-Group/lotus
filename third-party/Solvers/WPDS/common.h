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

#ifndef WPDS_COMMON_H_
#define WPDS_COMMON_H_ 1

#include <cstdio>
#include <iostream>
#include <string>
#include "wpds_sysdep.h"
/**************************************************/
/*            MACROS                                  */
/**************************************************/

#ifdef GRAMMATECH_TRACEBACK
#define PRIORITY_WORKLIST
#endif

/* runtime flag (only operative if compile-time flag WPDS_DEBUG != 0) */  
extern int b_wpds_debug;
extern int b_wpds_parse_debug;
extern int b_wpds_stat;
extern int b_dwpds_debug;
extern int b_readout;

extern std::ostream * pWpdsErr; // ostream * pWpdsErr = & std::cout; in common.cpp
#define WPDS_ERR (*pWpdsErr)

#ifndef CND_DBGS
#   ifdef DBGWPDS
#       define CND_DBGS(flag, stmts) do { if(flag) { stmts; } } while(0)
#   else
#       define CND_DBGS(flag, stmts) do { } while(0)
#   endif
#endif /* CND_DBGS */

#define DBGS(stmts) CND_DBGS(b_wpds_debug,stmts)

/* When printing out a line for PI_STATS*, prefix the line with
   PI_STATS_PREFIX. This makes it easier to grep for those lines
   or deleting unwanted lines in long output.
 */
#ifndef PI_STATS_PREFIX
#define PI_STATS_PREFIX  "PI_STATS: "
#endif /* PI_STATS_PREFIX (*/

/**************************************************/
/*            TYPEDEFS/CLASSES                      */
/**************************************************/

namespace wpds {
    typedef unsigned long wpds_size_t;
    typedef wpds_size_t WPDSKey;
    typedef wpds_size_t wpds_key;
    typedef wpds_size_t wpds_key_t;

    class Query
    {
        public: // static
            static Query WPDS_CALL base() { return BASE; }

            static Query WPDS_CALL user() { return BASE; }

            static Query WPDS_CALL prestar() { return PRESTAR; }

            static Query WPDS_CALL poststar() { return POSTSTAR; }

            static Query WPDS_CALL both() { return BOTH; }

        public: // creation
            Query( const Query& q)
                : query(q.query) {}

            Query& operator=( const Query& q )
            {
                query = q.query;
                return *this;
            }

        public: // instance
            bool is_base() const
            { return query == WPDS_BASE; }

            bool is_user() const
            { return query == WPDS_BASE; }

            bool is_prestar() const
            { return WPDS_PRESTAR == (query & WPDS_PRESTAR); }

            bool is_poststar() const
            { return WPDS_POSTSTAR == (query & WPDS_POSTSTAR); }

            bool is_both() const
            { return query == WPDS_BOTH; }

            bool operator==( const Query& q )
            { return query == q.query; }

            bool operator!=( const Query& q )
            { return query != q.query; }

            std::ostream& print( std::ostream& out ) const;

            std::string to_string() const;

        private:    // types
            enum query_t {
                WPDS_BASE = 0,      /*! used by witnesses */
                WPDS_PRESTAR = 1,   /*! Query is/was prestar */
                WPDS_POSTSTAR = 2,  /*! Query is/was poststar */
                WPDS_BOTH = 3       /*! Query might be/was both (used in WPDS) */
            };

        private:    // creator
            Query( query_t q )
                : query(q) {}

        private:    // vars
            query_t query;

            static Query BASE,PRESTAR,POSTSTAR,BOTH;
    };

}

/**************************************************/
/*            VAR DECLS                              */
/**************************************************/

namespace wpds {
    // DO NOT CHANGE WPDS_EPSILON.  This is corresponds to the location
    // of the what represents empty in wpds::Dictionary wpds::_str_dict
    static const wpds_key_t WPDS_EPSILON = 0;
    extern int rulesCount;
    extern int transCount;
    extern int bktsCount;
    extern int mhCount    ;

    // inc
    static inline void incRuleCount() { rulesCount++; }
    static inline void incTransCount() { transCount++; }
    static inline void incBktsCount() { bktsCount++; }
    static inline void incMhCount() { mhCount++; }

    // dec
    static inline void decRuleCount() { rulesCount--; }
    static inline void decTransCount() { transCount--; }
    static inline void decBktsCount() { bktsCount--; }
    static inline void decMhCount() { mhCount--; }

    static inline void dumpCounts()
    {
        printf("Rule Count\t: %d\n",rulesCount);
        printf("Trans Count\t: %d\n",transCount);
        printf("Bucket Count\t: %d\n",bktsCount);
        printf("Mh Count\t: %d\n",mhCount);
    }

}

/**************************************************/
/*             METHOD DECLS                          */
/**************************************************/
#endif    // WPDS_COMMON_H_

/* Yo, Emacs!
   ;;; Local Variables: ***
   ;;; tab-width: 4 ***
   ;;; End: ***
 */
