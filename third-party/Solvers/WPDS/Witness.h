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

#ifndef WITNESS_H
#define WITNESS_H


#include "common.h"
#include "inst_counter.h"
#include "ref_ptr.h"
#include "semiring.h"
#include "UTIL.h"
#include "key_source.h"
#include "HashMap.h"
#include "myallocator.h"
#include <set>
#include <list>
#include <utility>                          /* pair<T1,T2> */
#include <iostream>
#include <stack>

namespace wpds
{

    /* Class pre defs */
    template< typename T > class CA;
    template< typename T > class CATransition;
    template< typename T > class Rule;
    template< typename T > class CAPathOfWitness;
    template< typename T > class Witness;

    /* Generic hashing function for voidpairs */
    typedef std::pair<void*,void*> voidpair_key_t;
    struct hasher_voidpair_key {
        typedef voidpair_key_t argument_type;
        typedef size_t result_type;
        size_t operator() (const voidpair_key_t& k) const { return (size_t)k.first ^ (size_t)k.second; }
    };  
    struct equal_voidpair_key {
        typedef voidpair_key_t first_argument_type;
        typedef voidpair_key_t second_argument_type;
        typedef bool result_type;
        bool operator()(const voidpair_key_t &k1, const voidpair_key_t &k2) const
        { return k1 == k2; }
    };

    /* CAPathOfWitness is sequence of witness nodes w_1, w_2, ... w_n, that
       are witnesses for [weights on] transitions t_1, t_2, ... t_n such
       that t_1 ... t_n form a path in a Configuration Automaton. */
    template< typename T > class CAPathOfWitness PI_STATS_INST_COUNT(CAPathOfWitness< T >) {
    public:
        // typedefs
        GEN_WPDS_TYPEDEFS(T);
        typedef GPP_IMP_TYPENAME__ Witness<T>::witness_path_list_ptby_t witness_path_list_ptby_t;
        typedef witness_path_list_ptby_t* witness_path_list_t;
        typedef HashMap<voidpair_key_t, witness_path_t, 
            hasher_voidpair_key, equal_voidpair_key>
            witness_path_hash_t;

        // friends
        friend class ref_ptr<CAPathOfWitness>;
        friend class Witness<T>;

        // constructors
        CAPathOfWitness(const witness_t & w, const witness_path_t &r) 
            : _first(w), _rest(r), count(0) { }
        CAPathOfWitness(const CAPathOfWitness & w) 
            : _first(w._first), _rest(w._rest), count(0) 
            { }
        ~CAPathOfWitness() { }

        // accessors
        const witness_path_t & rest() { return _rest; }
        const witness_t & first() { return _first; }

        // hash-consing of CAPathOfWitness nodes; we should be
        // careful to occasionally empty mapped_paths
        voidpair_key_t key() const { 
            return voidpair_key_t(_first.get_ptr(), _rest.get_ptr()); 
        }

        void addToHash(witness_path_hash_t& mapped_paths) {
            witness_path_t path_curr(this);
            while( path_curr.get_ptr() ) {
                mapped_paths[path_curr->key()] = path_curr;
                path_curr = path_curr->rest();
            }
        }

        static const witness_path_t &
        WPDS_CALL hashedCreate(
                const GPP_IMP_TYPENAME__ CAPathOfWitness<T>::witness_t& f, 
                const GPP_IMP_TYPENAME__ CAPathOfWitness<T>::witness_path_t& r,
                witness_path_hash_t& mapped_paths)
        {
#ifdef GRAMMATECH_TRACEBACK          
          typename CAPathOfWitness<T>::witness_t fprime;
          if( dynamic_cast<WitnessAppl<T> *>(f.get_ptr()) && r.get_ptr()) {

            fprime = (static_cast<WitnessAppl<T> *>(f.get_ptr()))->correct_witness(r->first());
            voidpair_key_t k(fprime.get_ptr(),r.get_ptr());
            
            if( mapped_paths.find(k) == mapped_paths.end() ) {
              witness_path_t wit_path_new = new CAPathOfWitness(fprime,r);
              mapped_paths.insert(std::pair<voidpair_key_t,witness_path_t>(k,wit_path_new));
            }
            
            return mapped_paths[k];
          }
#endif          
          voidpair_key_t k(f.get_ptr(),r.get_ptr());
          
          if( mapped_paths.find(k) == mapped_paths.end() ) {
            witness_path_t wit_path_new = new CAPathOfWitness(f,r);
            mapped_paths.insert(std::pair<voidpair_key_t,witness_path_t>(k,wit_path_new));
          }
          
          return mapped_paths[k];
        }

        // Sometimes it is easier to build the path is reverse, and then
        // flip it.
        witness_path_t reverse() {
            witness_path_t curr(this), new_path(0);
            while( curr.get_ptr() ) {
                new_path = new CAPathOfWitness<T>(curr->first(),new_path);
                curr = curr->rest();
            }
            return new_path;
        }

    private:

        witness_t _first;
        witness_path_t _rest;
        RefCounter count;
    };

    /* Base class for witness nodes.  Instances of the base class serve
       as witness for values that "came out of nowhere" (i.e., were
       provided by the user in the original configuration automaton). 
    */
    template< typename T > class Witness PI_STATS_INST_COUNT(Witness< T >)
        {

            friend class ref_ptr<Witness>;
            friend std::ostream& 
                operator<<( std::ostream &o, const Witness<T> &W )
                { return W.print(o); }

        public:
            GEN_WPDS_TYPEDEFS(T);
            typedef GPP_IMP_TYPENAME__ STDLIST(witness_path_t) witness_path_list_ptby_t;
            typedef witness_path_list_ptby_t* witness_path_list_t;
            typedef HashMap<voidpair_key_t, witness_path_t, 
                hasher_voidpair_key, equal_voidpair_key>
                witness_path_hash_t;

#ifdef GRAMMATECH_TRACEBACK
            typedef wpds_key_t (*PropagateBackAction)(T *, wpds_key_t, T *, wpds_key_t);
            typedef std::pair<wpds_key_t, wpds_key_t> trans_key_pair_t;
#endif

            Witness(sem_elem_t se_,
                    Query _tag = Query::user()) 
                : trans(0), se(se_), orig_tag(_tag),count(0) { }

            Witness(const Witness &w)
                : trans(w.trans), se(w.se), orig_tag(w.orig_tag),count(0)
                {}

            virtual ~Witness() { }

            virtual witness_path_list_t path_succs(const witness_path_t &,
                                                   witness_path_hash_t &) {
                return new witness_path_list_ptby_t;
            }

            virtual witness_path_list_t path_preds(const witness_path_t &,
                                                   witness_path_hash_t &) {
                return new witness_path_list_ptby_t;
            }

            void semiring_element(sem_elem_t s) { se = s;   }
            sem_elem_t semiring_element() const { return se; }

            /*! object sets the catrans_t object for this Witness
             *
             * Added by Kidd for general Witness compatility,
             * does same as transition(catrans_t) method
             *
             * @see catrans_t
             * @see Witness<T>::transition
             */
            void object(const catrans_t &t) { trans = t; }
            void transition(const catrans_t &t) { trans = t; }
            catrans_t transition() const { return trans; }
            Query tag() { return orig_tag; }

            virtual std::ostream & print(std::ostream &o) const {
                trans->print(o,false);
                o << ' ';
                se->print(o);
                return o;
            }

#ifdef GRAMMATECH_TRACEBACK
            virtual wpds_key_t push_seq() {
                return WPDS_EPSILON;
            }

            virtual bool is_signal_entry() {
                return false;
            }

            virtual const const_rule_t get_rule() const {
                return 0;
            }

            virtual void trace_back_bug_trans(PropagateBackAction act,
                                              CA<T> *fa_answer, 
                                              std::pair<wpds_key_t,wpds_key_t> &curr, 
                                              std::list<trans_key_pair_t> &rest_stack) {
              return;
            }
#endif

        private:
            /* We are a witness to the creation of transition 't' with
               semiring element 'se' */
            catrans_t trans;  
            sem_elem_t se;
            Query orig_tag; //! Type of query that created this Witness
            unsigned count; //! for reference counting

        };

    // Class predecl
    template< typename T > class WitnessCombine;

    /*
     * This method combines wit_new into wit.  It is side effecting
     * of the parameter wit.  This is currently used in CATransition::witness
     * but will be migrated to SatPrcs.
     */
    template< typename T >
        void combine_witnesses(ref_ptr< Witness<T> >& wit,
                               const ref_ptr< Witness<T> >& wit_new )
        {
            GPP_IMP_TYPENAME__ Witness<T>::sem_elem_t se_new =
                wit->transition()->semiring_element();

            DBGS({
                GPP_IMP_TYPENAME__ Witness<T>::sem_elem_t se_tmp;
                se_tmp=wit->semiring_element();
                se_tmp=se_tmp->combine(wit_new->semiring_element().get_ptr());
                // assert( se_tmp == se_new );
            });

            // *NK-|-REMEMBER* This looks strange but the weight on wit->transition
            // has already been updated.  wit->semiring_element is the
            // transition's old weight!!!
            if( se_new->equal(wit->semiring_element().get_ptr()) )
                // The old witness approximates the new. KEEP THE OLD
                // WITNESS. This is very important as otherwise we can
                // create a cycle in a witess chain, leading to leaked
                // memory (by failure of reference counting) and
                // infinite loops in the traceback.
                return;

            if( se_new->equal(wit_new->semiring_element().get_ptr()) ) {
                // The new witness approximates the old
                wit = wit_new;
                return;
            }

            // The old and new witnesses are incomprable.
            // Combine the new witness with the existing witness
            WitnessCombine<T> * pWitComb;
            pWitComb = 
                dynamic_cast<WitnessCombine<T> *>(wit.get_ptr());
            if( ! pWitComb ) {
                // The existing witness is not a combine; create a combine witness.
                pWitComb = new WitnessCombine<T>(
                        se_new,wit_new->tag(),wit,wit_new);
                pWitComb->transition(wit->transition());
            } else {
                // create a copy of the combine witness (since others
                // may be reference the old witness with the old
                // value) and add the new witness to the copy.
                pWitComb = new WitnessCombine<T>(*pWitComb);
                pWitComb->add_operand(se_new,wit_new);
            }
            // install the combined witness
            wit = ref_ptr< Witness<T> >(pWitComb);
        }

    /* Witness for a semiring value created using combine */
    template< typename T > class WitnessCombine 
        :   public Witness<T> PI_STATS_INST_COUNTm(WitnessCombine< T >, Witness<T>) {
        public:

        GEN_WPDS_TYPEDEFS(T);
        /* It really irritates me to repeat these typenames here, but it
           eliminates a lot of compiler warnings, and I don't see the
           correct way to do it. */
        typedef GPP_IMP_TYPENAME__ Witness<T>::witness_path_hash_t witness_path_hash_t;
        typedef GPP_IMP_TYPENAME__ Witness<T>::witness_path_list_ptby_t witness_path_list_ptby_t;
        typedef witness_path_list_ptby_t* witness_path_list_t;

        WitnessCombine(const sem_elem_t & se, Query tag) 
            : Witness<T>(se,tag) { }

        WitnessCombine(
                const sem_elem_t & l_se,
                Query l_tag,
                const witness_t & w1,
                const witness_t & w2)
            : Witness<T>(l_se,l_tag) { 
#ifdef GRAMMATECH
            if(w1->semiring_element()->totalOrderLessThan(w2->semiring_element().get_ptr()))
                operands.push_front(w1);
            else 
                operands.push_front(w2);
#else
            operands.push_front(w1);
            operands.push_front(w2);
#endif
        }

        virtual std::ostream & print(std::ostream &o) const {
            Witness<T>::print(o);
            return o << " COMBINE(" << operands.size() << ')';
        }

        virtual witness_path_list_t path_succs(const witness_path_t & path,
                                               witness_path_hash_t & mapped_paths) {
            witness_path_list_t succs = new witness_path_list_ptby_t;
            typename STDLIST(witness_t)::iterator i;

            for(i = operands.begin(); i != operands.end(); ++i) {
                witness_path_t curr_path = 
                    CAPathOfWitness<T>::hashedCreate((*i), path->rest(), mapped_paths);
                succs->push_front(curr_path);
                /*                  witness_path_list_t curr_succ =  */
                /*                      curr_path->first()->path_succs(curr_path,mapped_paths); */
                /*                  succs->splice(succs->begin(), *curr_succ); */
                /*                  delete curr_succ; */
            }

            return succs;
        }

        virtual witness_path_list_t path_preds(const witness_path_t & path,
                                               witness_path_hash_t & mapped_paths) {
            witness_path_list_t preds = new witness_path_list_ptby_t;
            typename STDLIST(witness_t)::iterator i;

            for(i = operands.begin(); i != operands.end(); ++i) {
                witness_path_t curr_path = 
                    CAPathOfWitness<T>::hashedCreate((*i), path->rest(), mapped_paths);
                preds->push_front(curr_path);
                /*                  witness_path_list_t curr_pred =  */
                /*                      curr_path->first()->path_preds(curr_path,mapped_paths); */
                /*                  preds->splice(preds->begin(), *curr_pred); */
                /*                  delete curr_pred; */
            }

            return preds;
        }

        void add_operand(sem_elem_t se_new, witness_t w) {
            /* Set the new value */
            semiring_element(se_new);

#ifdef GRAMMATECH
            if( w->semiring_element()->totalOrderLessThan(se_new.get_ptr()) ) {
                operands.clear();
                operands.push_front(w);
            } else {
                return;
            }
#else
            /* Put w on the front of the operands list; go through the rest
               of the list, removing elements that are approximated by w in
               the meet semilattice */
            typename STDLIST(witness_t)::iterator i_prev, i;
            sem_elem_t se_newWit = w->semiring_element();
            operands.push_front(w);
            i = operands.begin();
            ++i;
            while( i != operands.end() ) {
                sem_elem_t se_cmb, se_curr;

                se_curr = (*i)->semiring_element();
                se_cmb = se_newWit->combine(se_curr.get_ptr());

                i_prev = i;
                ++i;

                if( se_newWit->equal(se_cmb.get_ptr()) ) 
                    operands.erase(i_prev);
            }
#endif
        }

        private:
        STDLIST(witness_t) operands;
    };

    /* Witness for a semiring value created using rule application and
       extend */
    template< typename T > class WitnessAppl
        : public Witness<T> PI_STATS_INST_COUNTm(WitnessAppl<T>, Witness<T>) {
        public:

        GEN_WPDS_TYPEDEFS(T);
        typedef GPP_IMP_TYPENAME__ Witness<T>::witness_path_hash_t witness_path_hash_t;
        typedef GPP_IMP_TYPENAME__ Witness<T>::witness_path_list_ptby_t witness_path_list_ptby_t;
        typedef witness_path_list_ptby_t* witness_path_list_t;
#ifdef GRAMMATECH_TRACEBACK
        typedef GPP_IMP_TYPENAME__ Witness<T>::PropagateBackAction PropagateBackAction;
        typedef GPP_IMP_TYPENAME__ Witness<T>::trans_key_pair_t trans_key_pair_t;
#endif /* GRAMMATECH_TRACEBACK */

        WitnessAppl(
                const sem_elem_t & s, 
                Query l_tag,
                const_rule_t r_, 
                const witness_t & w1_,
                const witness_t & w2_)
            : Witness<T>(s,l_tag), rule(r_), w1(w1_), w2(w2_)
            {}

        virtual std::ostream & print(std::ostream &o) const {
            Witness<T>::print(o);
            o << ' ';
            rule->print(o);
            return o;
        }

        virtual witness_path_list_t path_succs(const witness_path_t & path,
                                               witness_path_hash_t & mapped_paths) {
            witness_path_list_t succs = new witness_path_list_ptby_t;
            witness_path_t new_path;

            // There are 3 possibile ways for the transition to be created:
            //   1: using p<g> -> p'<>; both w1 and w2 are 0, the succ is just
            //      path->rest() 
            //   2: using p<g> -> p'<g'>; w2 is 0, the succ is w1 concatenated on
            //      path->rest()
            //   3: using p<g> -> p'<g'g">; the succ is w1, w2, path->rest()

            new_path = path->rest();
            if( w2.get_ptr() )
                new_path =
                    CAPathOfWitness<T>::hashedCreate(w2,new_path,mapped_paths);
            if( w1.get_ptr() )
                new_path =
                    CAPathOfWitness<T>::hashedCreate(w1,new_path,mapped_paths);

            succs->push_front(new_path);
            return succs;
        }

        virtual witness_path_list_t path_preds(const witness_path_t & path,
                                               witness_path_hash_t & mapped_paths) {
            if( rule->to_stack2() != WPDS_EPSILON ) {
                /* A rule p<y> -> p'<y'y"> and a trans (p,y,q) was used to
                   create transitions:
                   (p',y',(p'y'))  --- first on 'path'
                   ((p'y',y",q)    --- should be second on 'path'

                   The first transition may have been created by other rules of
                   the form "p<y> -> ...", so its witness is not reliable.
                   Instead we use the witness on the second transition.
                   The first thing we need to do is check wether we are the
                   first or second transition.
                */
                key_source *pFromKeyInfo = 
                    retrieve_item(this->transition()->from_state());
                if( !pFromKeyInfo->cast_key_pair_src() ) {
                    // We are the first transition.
                    if(path->rest().get_ptr()) {
                                // There is a second transition; use it to get the
                                // predecessors.  (Note the second transition must be
                                // from a state that was created as a key_pair.)
                        return path->rest()->first()->path_preds(path->rest(),
                                                                 mapped_paths);
                    } else {
                                // There is no second transition, this path does not
                                // make sense; we'll simply return an empty list.
                        return new witness_path_list_ptby_t;
                    }
                }
            }

            // We are the second transition, or the rule did not create two
            // transitions. 
            witness_path_list_t preds = new witness_path_list_ptby_t;
            preds->push_front(CAPathOfWitness<T>::hashedCreate(w1,
                                                               path->rest(),
                                                               mapped_paths));
            return preds;
        }

#ifdef GRAMMATECH_TRACEBACK
        WitnessAppl* correct_witness(const witness_t &r) {

          if(rule->to_stack2() != WPDS_EPSILON ) {
            key_source *pFromKeyInfo = 
              retrieve_item(this->transition()->from_state());
            if( !pFromKeyInfo->cast_key_pair_src() ) {
              // We are the first transition.
              if(r == w1) {
                // avoid creating a duplicate
                return this;
              }
              WitnessAppl<T> *corrected_wit = new WitnessAppl<T>(
                  this->semiring_element(),
                  this->tag(),
                      rule,
                      r.get_ptr(), // store the correct wit
                      w2);
              corrected_wit->transition(this->transition());
              return corrected_wit;
            }
          }
          return this;
        }

        wpds_key_t push_seq() {
          if(rule->to_stack2() != WPDS_EPSILON) {
            key_source *pFromKeyInfo = 
              retrieve_item(this->transition()->from_state());
            if( !pFromKeyInfo->cast_key_pair_src() ) {
              // We're a witness for the first transition. So we represent w1.
              return w1->push_seq(); 
            }
          }
          return rule->to_stack1();
        }

        // Currently, a call to a signal handler uses a rule
        // looks like:
        //   q <v> --> q <e v>
        // This is the ONLY rule that has the 2nd stack symbol the
        // same as the source symbol.  We can use this to check if we
        // are a witness for an entry to a signal handler.
        bool is_signal_entry() {
          if(rule->to_stack2() != WPDS_EPSILON) {
            key_source *pFromKeyInfo = 
              retrieve_item(this->transition()->from_state());
            if( !pFromKeyInfo->cast_key_pair_src() ) {
                return rule->to_stack2() == rule->from_stack();
            }
          }
          return false;
        }

        const const_rule_t get_rule() const {
            return rule;
        }

        /* FIXME: Only works for poststar */
        virtual void trace_back_bug_trans(PropagateBackAction act,
                                          CA<T> *fa_answer, 
                                          std::pair<wpds_key_t,wpds_key_t> &curr, 
                                          std::list<trans_key_pair_t> &rest_stack) {

          if( rule->to_stack2() != WPDS_EPSILON ) {
            /* A rule p<y> -> p'<y'y"> and a trans (p,y,q) was used to
               create transitions:
               (p',y',(p'y'))  --- first on 'path'
               ((p'y',y",q)    --- should be second on 'path'
               
               The first transition may have been created by other rules of
               the form "p<y> -> ...", so its witness is not reliable.
               Instead we use the witness on the second transition.
               The first thing we need to do is check wether we are the
               first or second transition.
            */
            key_source *pFromKeyInfo = 
              retrieve_item(this->transition()->from_state());
            if( !pFromKeyInfo->cast_key_pair_src() ) {
              // We are the first transition.
              // We're done with curr
              assert(rest_stack.size() != 0);
              //assert(curr.first == curr.second);
              curr = rest_stack.front();
              rest_stack.pop_front();

              if(rest_stack.size() == 0) { // construct on the fly
                sem_elem_t ltfr = w1->semiring_element();
                trans_key_pair_t rest;
                wpds_key_t mid;

                mid = act(fa_answer->state_weight(w1->transition()->to_state()).get_ptr(),
                          curr.first,
                          ltfr.get_ptr(),
                          curr.second);

                rest = trans_key_pair_t(curr.first,mid);
                curr.first = mid;
                rest_stack.push_front(rest);
              }

              w1->trace_back_bug_trans(act,fa_answer,curr,rest_stack);
              return;
            }
          }
          T *se_first = w1.get_ptr()->semiring_element().get_ptr();
          T *se_second = rule->semiring_element().get_ptr();
          curr.second = act(se_first,curr.first,se_second,curr.second);
          return;
        }
#endif

        private:
        const_rule_t rule;
        witness_t w1, w2;
    };

    // Witness of an epsilon contraction (always the result of a "post*
    // operation".
    template< typename T > class WitnessEpsContraction
        : public Witness<T> PI_STATS_INST_COUNTm(WitnessEpsContraction<T>, Witness<T>) {
        public:

            GEN_WPDS_TYPEDEFS(T);
            typedef GPP_IMP_TYPENAME__ Witness<T>::witness_path_hash_t witness_path_hash_t;
            typedef GPP_IMP_TYPENAME__ Witness<T>::witness_path_list_ptby_t witness_path_list_ptby_t;
            typedef witness_path_list_ptby_t* witness_path_list_t;
#ifdef GRAMMATECH_TRACEBACK
            typedef GPP_IMP_TYPENAME__ Witness<T>::trans_key_pair_t trans_key_pair_t;
            typedef GPP_IMP_TYPENAME__ Witness<T>::PropagateBackAction PropagateBackAction;
#endif /* GRAMMATECH_TRACEBACK */

            WitnessEpsContraction(
                    sem_elem_t s,
                    witness_t ew,
                    witness_t w1)
                : Witness<T>(s,Query::poststar()), eps_wit(ew), wit1(w1)
                { }


            virtual std::ostream & print(std::ostream &o) const {
                Witness<T>::print(o);
                o << " ";
                eps_wit->transition()->print(o);
                o << " EPS w/ ";
                wit1->transition()->print(o);
                return o;
            }

            virtual witness_path_list_t path_succs(const witness_path_t & path,
                    witness_path_hash_t &
                    mapped_paths) {
                // FIXME: we should give some kind of warning error here.
                return new witness_path_list_ptby_t;
            }

            virtual witness_path_list_t path_preds(const witness_path_t & path,
                    witness_path_hash_t & mapped_paths) {
                witness_path_list_t preds = new witness_path_list_ptby_t;
                witness_path_t rest;

                rest = new CAPathOfWitness<T>(wit1,path->rest());
                preds->push_front(CAPathOfWitness<T>::hashedCreate(eps_wit,rest,mapped_paths));
                return preds;
            }

#ifdef GRAMMATECH_TRACEBACK
            wpds_key_t push_seq() {
                return this->transition()->transition();
            }

            /* Only works for poststar */
            virtual void trace_back_bug_trans(
                    PropagateBackAction act,
                    CA<T> *fa_answer, 
                    std::pair<wpds_key_t,wpds_key_t> &curr, 
                    std::list<trans_key_pair_t> &rest_stack) {

                T *se_first = wit1.get_ptr()->semiring_element().get_ptr();
                T *se_second = eps_wit.get_ptr()->semiring_element().get_ptr();
                wpds_key_t mid = act(se_first,curr.first,se_second,curr.second);

                trans_key_pair_t rest(curr.first,mid);
                rest_stack.push_front(rest);
                curr.first = mid;
                return;
            }
#endif

        private:
            witness_t eps_wit, wit1;
        };


    /* A Directed Acyclic Graph of Witness Nodes that explains a (Witness) Path
       in a Configuration Automaton.  Each path in the DAG corresponds to a
       sequence of WPDS rule applications (if you ignore the WitnessCombine and
       WitnessEpsContraction nodes).  Taking the COMBINE over all those
       sequences justifies the path used to create the DAG*/
    template<typename T> class DAGWitnessForPath PI_STATS_INST_COUNT(DAGWitnessForPath< T >) {
        public:
            GEN_WPDS_TYPEDEFS(T);
            friend class ref_ptr<DAGWitnessForPath>;
            typedef ref_ptr<DAGWitnessForPath> witness_dag_t;
            typedef GPP_IMP_TYPENAME__ CAPathOfWitness<T>::witness_path_hash_t witness_path_hash_t;
            typedef GPP_IMP_TYPENAME__ Witness<T>::witness_path_list_ptby_t witness_path_list_ptby_t;
            typedef witness_path_list_ptby_t* witness_path_list_t;
            typedef std::pair<void *, void*> voidpair_key_t;
            typedef std::pair<DAGWitnessForPath<T>*, witness_path_t> workitem_t;

            DAGWitnessForPath(const DAGWitnessForPath &w)
                : wit(w.wit), succs(w.succs), count(0) { }
            ~DAGWitnessForPath() { 
                // Deleting a single DAGWitnessForPath node can lead to a huge
                // recursive delete, overflowing the stack.
                //
                // To flatten the recursion, we use a holding list, "list_hold".
                // The idea is to temporarily create an extra reference to the
                // children that have only us as a referee, then allow us to be
                // deleted.  Then we pick one of the children and repeat the
                // process with its children ...
                STDLIST(witness_dag_t) list_hold;

                // First, populate list_hold with our children. (Note: you cannot
                // simply put this on list_hold, b/c it would create an reference
                // to this already "dead" node and we'd try to delete it again
                // later.)

                while( ! succs.empty() ) {
                    if( 1 == succs.front()->count )
                        list_hold.push_back(succs.front());
                    succs.pop_front();
                }

                while( ! list_hold.empty() ) {
                    const witness_dag_t & dag_curr = list_hold.front();
                    GPP_IMP_TYPENAME__ STDLIST(witness_dag_t)::const_iterator iter;
                    for( iter  = dag_curr->succs.begin();
                            iter != dag_curr->succs.end();
                            ++iter ) {
                        if( 1 == (*iter)->count )
                            /* dag_curr is the last reference to **iter; create an
                               extra reference. */
                            list_hold.push_back(*iter);
                    }

                    /* Now, we pop dag_curr from the front of the list. This should
                       cause dag_curr to be deleted, since list_hold should be the
                       last reference to it.  However, the deletition will not
                       recurse, b/c we've already created extra references to the
                       singly referenced children. */
                    list_hold.pop_front();
                }
            }

            friend std::ostream &operator<<(std::ostream &o, const DAGWitnessForPath<T> &wg) {
                return wg.print(o);
            }

            struct hasher_void_key {
                typedef void* argument_type;
                typedef size_t result_type;
                size_t operator() (const void* k) const { return (size_t)k; }
            };  
            struct equal_void_key {
                typedef void* first_argument_type;
                typedef void* second_argument_type;
                typedef bool result_type;
                bool operator() (const void* k1, const void* k2) const 
                { return k1 == k2; }
            };  

            static witness_dag_t WPDS_CALL createFromCAPathOfWitness(
                    witness_path_t path_curr,
                    const Query tag)
            {
                /* argument "path_curr" is resued; should not be a const ref */
                HashMap <void*, witness_dag_t, hasher_void_key, equal_void_key> mapped_dags;
                witness_path_hash_t mapped_paths;
                STDLIST(workitem_t) worklist;
                witness_dag_t wit_dag_orig, wit_dag_curr;
                witness_path_list_t path_list;

                // Add this path (and suffixes) to the mapped_paths
                path_curr->addToHash(mapped_paths);

                // Create the first node of the DAG
                wit_dag_orig = wit_dag_curr = new DAGWitnessForPath(path_curr->first());
                mapped_dags[path_curr.get_ptr()] = wit_dag_curr;

                // Sanity check: we should only be using witnesses that are tagged
                // according to the request.
                if(wit_dag_curr->wit->tag() != tag)
                    return wit_dag_orig;

                // initialize the worklist
                if( tag.is_poststar() )
                    path_list = wit_dag_curr->wit->path_preds(path_curr, mapped_paths);
                else
                    path_list = wit_dag_curr->wit->path_succs(path_curr, mapped_paths);

                while( !path_list->empty() ) {
                    worklist.push_front(workitem_t(wit_dag_curr.get_ptr(), path_list->front()));
                    path_list->pop_front();
                }
                delete path_list;

                // process the worklist
                while( !worklist.empty() ) {
                    wit_dag_curr = worklist.front().first;
                    path_curr = worklist.front().second;
                    worklist.pop_front();

                    // Check if we already created a node for this path:
                    if(mapped_dags.find(path_curr.get_ptr()) != mapped_dags.end()) {
                        // There is an existing node; install it as a successor of this
                        // node. 
                        wit_dag_curr->succs.push_front(mapped_dags[path_curr.get_ptr()]);
                    } else {
                        // Create a new dag node for this path; add it as a successor to the
                        // current dag
                        witness_dag_t wit_dag_new = new DAGWitnessForPath(path_curr->first());
                        mapped_dags[path_curr.get_ptr()] = wit_dag_new;
                        wit_dag_curr->succs.push_front(wit_dag_new);

                        // Add predecessor paths to the worklist (if this is a
                        // witness of the proper kind)
                        if(wit_dag_curr->wit->tag() == tag) {
                            if( tag.is_poststar() )
                                path_list = wit_dag_new->wit->path_preds(path_curr, mapped_paths);
                            else
                                path_list = wit_dag_new->wit->path_succs(path_curr, mapped_paths);

                            while( !path_list->empty() ) {
                                worklist.push_front(workitem_t(wit_dag_new.get_ptr(), path_list->front()));
                                path_list->pop_front();
                            }
                            delete path_list;
                        }
                    }
                }

                mapped_dags.clear();
                mapped_paths.clear();
                return wit_dag_orig;
            }

            std::ostream & print(std::ostream & o) const {
                STDSET(const void*) visited;
                STDLIST(const DAGWitnessForPath<T> *) worklist;
                const DAGWitnessForPath<T> * dag_curr;

                o << "digraph post_witness_dag {" << std::endl;

                visited.insert(this);
                worklist.push_front(this);
                while( ! worklist.empty() ) {
                    dag_curr = worklist.front();
                    worklist.pop_front();

                    o << "  n" << ((int)dag_curr) << "[label = \"";
                    dag_curr->wit->print(o);
                    o << "\"];" << std::endl;

                    typename STDLIST(witness_dag_t)::const_iterator i;
                    for( i  = dag_curr->succs.begin();
                            i != dag_curr->succs.end();
                            ++i ) {
                        DAGWitnessForPath * dag_succ = (*i).get_ptr();
                        o << "  n" << (int)dag_curr << " -> "
                            << 'n' << (int)dag_succ << ';' << std::endl;
                        if( visited.find(dag_succ) == visited.end() ) {
                            visited.insert(dag_succ);
                            worklist.push_front(dag_succ);
                        }
                    }
                }

                o << "};" << std::endl;
                return o;
            }

            // accessor functions:
            const witness_t &witness() const { return wit; }
            const STDLIST(witness_dag_t) &successors() const { return succs; }
            bool has_succ() const { return ! succs.empty(); }

        private:
            DAGWitnessForPath(witness_t w_) : wit(w_), count(0) { }

            witness_t wit;
            STDLIST(witness_dag_t) succs;
            RefCounter count;
            // unsigned count;
    };

}   /* namespace */

#endif

/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
