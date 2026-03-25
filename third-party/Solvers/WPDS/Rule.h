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

#ifndef wpds_RULE_H_
#define wpds_RULE_H_ 1

#include <iostream>
#include "semiring.h"
#include "ref_ptr.h"
#include "common.h"
#include "UTIL.h"
#include "inst_counter.h"
#include "Traits.h"

namespace wpds
{

    template< typename T > class WPDS;

    /*! @class Rule
     *
     * @brief a Rule represents a transition in configuration space
     * 
     * a Rule represents a valid transition in configuration space.  Each
     * rule has a from state and top of stack and a to state.  If there is no
     * stack symbols on the right hand side (RHS) then it is a pop rule.  One 
     * stack symbol on the RHS is a "replace" or pop and push rule, and 2 on
     * the RHS pops and pushes two stack symbols.
     *
     * For doing dataflow analysis, 0 stack symbols is a function return, 1
     * is a transition in a CFG, and 2 is a function call.
     *
     * @see Traits::sem_elem_t
     * @see wpds::wpds_key_t
     *
     */
    template< typename T > class Rule PI_STATS_INST_COUNT(Rule< T >)
    {

        friend class WPDS<T>;
        friend class ref_ptr<Rule>;

        friend std::ostream& operator<<(std::ostream &o, const Rule& r)
        {
            return r.print( o );
        }

        private:
            GEN_WPDS_TYPEDEFS(T);

        public:
            /*! @var an enum specifying the type a rule is */
            enum type {
                RULE0=0, //!< Rule of type 0 (no rhs stack symbols)
                RULE1=1, //!< Rule of type 1 (1 rhs stack symbol)
                RULE2=2, //!< Rule of type 2 (2 rhs stack symbols)
                MAX=3 };

        public:
            /* Constructor/Destructor */
            Rule(
                    T *t,
                    const wpds_key_t l_from_state = WPDS_EPSILON,
                    const wpds_key_t l_from_stack = WPDS_EPSILON,
                    const wpds_key_t l_to_state   = WPDS_EPSILON,
                    const wpds_key_t l_to_stack1  = WPDS_EPSILON,
                    const wpds_key_t l_to_stack2  = WPDS_EPSILON
                )
                : p(l_from_state)
                , y(l_from_stack)
                , q(l_to_state)
                , yp( l_to_stack1 )
                , ypp( l_to_stack2 )
                , se( t )
                { incRuleCount(); }

            Rule( 
                    const sem_elem_t& se_,
                    const wpds_key_t _from_state = WPDS_EPSILON,
                    const wpds_key_t _from_stack = WPDS_EPSILON,
                    const wpds_key_t _to_state   = WPDS_EPSILON, 
                    const wpds_key_t _to_stack1  = WPDS_EPSILON, 
                    const wpds_key_t _to_stack2  = WPDS_EPSILON
                )
                : p( _from_state )
                , y( _from_stack )
                , q( _to_state )
                , yp( _to_stack1 )
                , ypp( _to_stack2 )
                , se(se_)
                { incRuleCount(); }

            virtual ~Rule() { decRuleCount(); }

            /*******************/
            /* Getters         */
            /*******************/
            /*! @return the rule's from state */
            const wpds_key_t from_state() const { return p; }

            /*! @return the rule's from stack */
            const wpds_key_t from_stack() const { return y; }

            /*! @return the rule's to state */
            const wpds_key_t to_state()   const { return q; }

            /*! @return the rule's 1st rhs stack symbol */
            const wpds_key_t to_stack1() const  { return yp; }

            /*! @return the rule's 2nd rhs stack symbol */
            const wpds_key_t to_stack2() const  { return ypp; }

            /*! @return A const reference to the rule's weight */
            const sem_elem_t& weight() const    { return se; }

            /*! @return A const reference to the rule's weight */
            const sem_elem_t& semiring_element() const { return se; }


            /*******************/
            /* Others          */
            /*******************/
            // Hash func
            wpds_size_t hash_code(bool pre=true) const
            {
                if(pre) {
                    // combine_wpds_keys is defined in common.h
                    // because the code here MUST MATCH that that is
                    // used in rule.h
                    return combine_wpds_keys( q,yp );
                }
                else {
                    return combine_wpds_keys( p,y );
                }
            }

            wpds_size_t hash_codeb() const
            {
                return ypp;
            }

            /*! Equality comparison two rules
             * @return true if they are equal
             */
            bool equal( const Rule *r ) const
            {
                return (
                        (this == r) ||
                        (
                         (r != 0) &&
                         (p == r->p) &&
                         (y == r->y) &&
                         (q == r->q) &&
                         (yp == r->yp) &&
                         (ypp == r->ypp)
                         ));
            }

            /*! print the Rule to the ostream provided
             *
             * @return the ostream reference passed in
             * @see std::ostream
             */
            std::ostream& print( std::ostream& o,bool printW=true ) const
            {
                o << "<";
                printkey(from_state(),o) <<", ";
                printkey(from_stack(),o) << "> -> <";
                printkey(to_state(),o) << ", ";
                if( WPDS_EPSILON != to_stack1() ) {
                    printkey(to_stack1(),o);
                }
                if( WPDS_EPSILON != to_stack2() ) {
                    o << " ";
                    printkey(to_stack2(),o);
                }
                o << ">\t";
                if( printW )
                    se->print(o);
                return o;
            }

            //virtual Rule* clone( T *t = 0 ) const = 0;
        protected:
            wpds_key_t p;
            wpds_key_t y;
            wpds_key_t q;
            wpds_key_t yp;  //!< for rules of type 1,2
            wpds_key_t ypp; //!< for rules of type 2
            sem_elem_t se;
    };

} // namespace

#endif  // wpds_RULE_H_
/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
