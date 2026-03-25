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
 * $Id: ESaturationProcess.h,v 1.3 2005-09-06 16:11:35 radu Exp $
 */
#ifndef wpds_ESATURATION_PROCESS_H_
#define wpds_ESATURATION_PROCESS_H_
#include "../SaturationProcess.h"
#include "../util/CA.intersect.h"

/* The saturation processes for Extended-WPDS. 
 */
#define SEM_ELEM_PAIR_COLLAPSE(se) (se->get_first()->extend(se->get_second()))

namespace wpds {
  template< typename T > class EWPDS;
  
  template< typename T> class ESaturationProcess : public SaturationProcess<SemiringElemPair<T,T> > {

  public:       // Constructor/De
    
    ESaturationProcess(
               EWPDS<T>& ewpds_,
               CA<SemiringElemPair<T,T> >& ca_,
               Semiring<T>& s_,
               Query q)
      : SaturationProcess<SemiringElemPair<T,T> >( *((WPDS<SemiringElemPair<T,T> > *) (&ewpds_)),
                           ca_,
                           *(new Semiring<SemiringElemPair<T,T> >(new SemiringElemPair<T,T>(s_.get_data(),s_.get_data()))),
                           q)
      , ewpds(ewpds_)
      , s(s_)
      , s_elem(new SemiringElemPair<T,T>(s_.get_data(), s_.get_data()))
      { }
    
    virtual ~ESaturationProcess() {}
    typedef SaturationProcess<SemiringElemPair<T,T > > sat_base_class_t;
    typedef SemiringElemPair<T,T> sem_pair_t;
    GEN_WPDS_TYPEDEFS(sem_pair_t);
    typedef HashMap< wpds_key_t, ConstRuleList > sat_rule_hash_t;
    typedef ref_ptr<BaseCF<T> > conf_fn_t;
    
    
    
  private:    
    void apply_post2(catrans_t t,
             const sem_elem_t& delta );
    void apply_pre2b(const WPDS<SemiringElemPair<T,T> >& wpds,
             catrans_t t,
             const sem_elem_t& delta,
             sat_rule_hash_t& rule2b_map );
    void apply_pre2a(const WPDS<SemiringElemPair<T,T> >& wpds,
             catrans_t t,
             const sem_elem_t& delta );

  public:
    void prestar();
    void poststar();

  private:
    EWPDS<T> &ewpds;
    Semiring<T> &s;
    Semiring<SemiringElemPair<T,T> > s_elem;
  };
  
  /* ***************************************************
   * POSTSTAR
   * **************************************************/
  
  /************************************************************/
  // Rule2s: <p,g>-><p',g'g''> -->
  //                (p',g',q_p'g') && (q_p'g',g'',q)
  // This encodes eps-contractions as well
  /************************************************************/
  template<typename T>
  void ESaturationProcess<T>::apply_post2(
                      catrans_t t,
                      const sem_elem_t& delta ) {
    
    sem_elem_t se;
    sem_elem_t quasi;
    witness_t witness;
    catrans_t tprime;

    RuleListConstIterPair p
      = ewpds.match_rule_post2( KeyPair(t->from_state(),t->stack()) );
    
    RuleListConstIter iter;
    
    for( iter = p.first ; iter != p.second ; iter++ ) {
      
      const_rule_t r = *iter;
      
      DBGS({
    std::cout << "[poststar] Rule2 match found:\t";
    r->print(std::cout) << std::endl;
      });
      wpds_key_t new_state = gen_state( r );
      
      // We must add the midstate in case it does not exist
      sat_base_class_t::ca.make_state( new_state );
      
      se = s_elem.extend( delta,F(r) );
      
      // For quasione, we must combine se with the quasione
      // of the state if it already existed.  If new_state was
      // truly new, its quasione starts at zero so the combining
      // will result in "se" created above
      quasi = sat_base_class_t::ca.state_quasione( new_state );
      sem_elem_t sec = new SemiringElemPair<T,T>(SEM_ELEM_PAIR_COLLAPSE(se), s_elem.one()->get_first());
      quasi = s_elem.combine( quasi,sec );
      sat_base_class_t::ca.state_quasione( new_state, quasi );
      
      // (p',g',q_p'g')
      UPDATE( r->to_state(),
          r->to_stack1(),
          new_state,
          s_elem.quasiOne( quasi ),
          NEW_WITNESS_APPL(s_elem.one(),Query::poststar(),
                   r,t->witness(), witness_t(0)) );
      
      // (q_p'g',g'',q)
      tprime =
    UPDATE_PRIME( new_state,
              r->to_stack2(),
              t->to_state(), se,
              NEW_WITNESS_APPL(se,Query::poststar(),
                       r,t->witness(), witness_t(0)) );
      
      if( T_MOD_CONTINUE( tprime, s_elem ) )
    {
          
      TransListIterPair tIterPair;
      TransListIter titer;
          
      tIterPair = sat_base_class_t::ca.match_eps_to(tprime->from_state());
      for( titer  = tIterPair.first;
           titer != tIterPair.second; ++titer )
        {
          
          catrans_t teps(*titer);
          sem_elem_t seExtend;
          key_source *ks = retrieve_item(tprime->from_state());
          key_pair_src *kps;
          if(kps = dynamic_cast<key_pair_src *>(ks)) {
        RuleListConstIterPair rp = ewpds.match_rule(KeyTriple(kps->k1,kps->k2,tprime->transition()));
        assert(rp.first != rp.second); // FIXME: can take default action if this is the case
        RuleListConstIter rit;
        seExtend = s_elem.zero();
        for(rit = rp.first; rit != rp.second; rit++) {
          //assert(dynamic_cast<ERule<SemiringElemPair<T,T>, T > *>(*rit));
          conf_fn_t cf = ((ERule<SemiringElemPair<T,T>, T > *)(*rit))->confluence_fn();
          DBGS(cf->print(std::cout) << "\n");
          sem_elem_t se2 = new SemiringElemPair<T,T>(cf->apply_f(POST2_SELECT(se,tprime)->get_first(),L(teps)->get_first()),s.one());
          seExtend = seExtend->combine(se2.get_ptr()); 
        }
          } else {
        seExtend = s_elem.extend( POST2_SELECT(se,tprime), L(teps) );
          }       
          
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

  template <typename T>
  void ESaturationProcess<T>::poststar() {
      
      
    DBGS({
      WPDS_ERR << "-----\n\tESaturationProcess::poststar()\n-----\n";
      //WPDS_ERR << ca << std::endl;
    });
    sat_base_class_t::populate_workset();

    // Local vars for chaotic iteration
    catrans_t t,tprime;
    sem_elem_t se,se2;
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
    while( !sat_base_class_t::workset_is_empty() ) {

      // (p,g,q)
      t = sat_base_class_t::get_from_workset();
      delta = DELTA(t);

      DBGS(t->print(WPDS_ERR << "Popped ") << " from workset\n");

      if (ewpds.extender)
    {
      // Ask if the pair (from-state,stack-sym has been
      // visited before.  If not, then mark it as so
      // and call the rule-extension method.
      sym_stack_pair.first = t->from_state();
      sym_stack_pair.second = t->transition();
      found = visited.find(sym_stack_pair);
      if (found == visited.end())
        {
          ewpds.extender->add_rules(ewpds,
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
    //              (p',g',q_p'g') && (q_p'g',g'',q)

    APPLY_POST2( t,delta );

      }
      else {

    // for all t' = (q,g',q')
    tIterPair = sat_base_class_t::ca.match( t->to_state() );
    for( titer = tIterPair.first;
         titer != tIterPair.second; titer++ ) {

      tprime = *titer;
      DBGS({
        std::cout << "[poststar] eps trans, think ";
        tprime->print(std::cout) << "matches...\n";
      });

      key_source *ks = retrieve_item(t->to_state());
      key_pair_src *kps;
      if(kps = dynamic_cast<key_pair_src *>(ks)) {
        RuleListConstIterPair rp = ewpds.match_rule(KeyTriple(kps->k1,kps->k2,tprime->transition()));
        assert(rp.first != rp.second); // FIXME: can take default action if this is the case
        RuleListConstIter rit;
        se = s_elem.zero();
        for(rit = rp.first; rit != rp.second; rit++) {
          //assert(dynamic_cast<ERule<SemiringElemPair<T,T>, T > *>(*rit));
          conf_fn_t cf = ((ERule<SemiringElemPair<T,T>, T > *)(*rit))->confluence_fn();
          DBGS(cf->print(std::cout) << "\n");
          sem_elem_t se2 = new SemiringElemPair<T,T>(cf->apply_f(L(tprime)->get_first(),delta->get_first()),s.one());
          se = se->combine(se2.get_ptr()); 
        }
      } else {
        se = s_elem.extend( L(tprime),delta );
      }
      // (p,g',q')
      UPDATE( t->from_state(),
          tprime->stack(),
          tprime->to_state(),
          se,
          NEW_WITNESS_EPS(se, t->witness(), tprime->witness()) );

    }
      }
    }

    DBGS( std::cerr << "numpops is " << sat_base_class_t::numpops << std::endl );
  }

  /* ***************************************************
   * PRESTAR
   * **************************************************/

  /************************************************/
  /* for all rule2s <p1,g1>-><q,g g2> (Rule2a)    */
  /*        + (q',g2,q'') --> (p1,g1,q'')     */
  /************************************************/
  // First argument is not needed. It is there because this rule overrides the one from SaturationProcess<T,T>
  // and in there the argument is required for schemas. Schemas are not supported in EWPDS yet.
  template< typename T >
    void ESaturationProcess<T>::apply_pre2a(
                        const WPDS<SemiringElemPair<T,T> >& wpds,
                        catrans_t t,
                        const sem_elem_t& delta ) {
    sem_elem_t se, fr;
    
    RuleListConstIterPair ruleIterPair =
      ewpds.match_rule_pre( KeyPair(t->from_state(), t->transition()) ,Rule<SemiringElemPair<T,T> >::RULE2 );
    
    RuleListConstIter riter;
    
    TransListIterPair tIterPair;
    TransListIter titer;
    
    for( riter = ruleIterPair.first;
     riter != ruleIterPair.second; riter++ ) {
      const_rule_t r = *riter;
      
      DBGS({
    std::cout << "\t[prestar] Rule2a match found:\t";
    r->print(std::cout) << std::endl;
      });
      
      tIterPair = sat_base_class_t::ca.match( t->to_state(),r->to_stack2() );

      if(ewpds.is_element_of_P(t->to_state())) { // Apply combination functions
    for( titer = tIterPair.first;
         titer != tIterPair.second; titer++ ) {
      // (q',g2,g'')
      catrans_t& tprime = *titer;
      //assert(dynamic_cast<ERule<SemiringElemPair<T,T>, T > *>(r));
      conf_fn_t cf = ((ERule<SemiringElemPair<T,T>, T > *)(r))->confluence_fn();
      se = new SemiringElemPair<T,T>(cf->apply_f(s.one().get_ptr(), delta->get_first()),s.one().get_ptr()); 
      se = s_elem.extend( se, L(tprime) );
      
      // (p1,g1,q'')
      UPDATE( r->from_state(),
          r->from_stack(),
          tprime->to_state(),se,
          NEW_WITNESS_APPL(se,Query::prestar(),
                   r,t->witness(), tprime->witness()) );
    }
      } else { // Do not apply combination function
    for( titer = tIterPair.first;
         titer != tIterPair.second; titer++ ) {
      // (q',g2,g'')
      catrans_t& tprime = *titer;
      // Correct weight on rule r
      fr = new SemiringElemPair<T,T>(F(r)->get_second(), s.one().get_ptr());      
      se = s_elem.extend( delta, L(tprime) );
      se = s_elem.extend( fr,se );
      
      // (p1,g1,q'')
      UPDATE( r->from_state(),
          r->from_stack(),
          tprime->to_state(),se,
          NEW_WITNESS_APPL(se,Query::prestar(),
                   r,t->witness(), tprime->witness()) );
    }
      }
    }
  }
  
  /************************************************/
  /* for all rule2s <p1,y1>-><p',g2 g> (Rule2b)   */
  /*        + (p',g2,q)  -->  (p1,g1,q')      */
  /************************************************/
  // First argument is not needed. It is there because this rule overrides the one from SaturationProcess<T,T>
  // and in there the argument is required for schemas. Schemas are not supported in EWPDS yet.
  template< typename T >
    void ESaturationProcess<T>::apply_pre2b(
                        const WPDS<SemiringElemPair<T,T> >& wpds,
                        catrans_t t,
                        const sem_elem_t& delta,
                        sat_rule_hash_t& rule2b_map ) {

    catrans_t tprime;
    sem_elem_t se, fr;
    
    GPP_IMP_TYPENAME__ sat_rule_hash_t::iterator iter = rule2b_map.find( t->stack() );
    if( iter == rule2b_map.end() )
      return;
    
    ConstRuleList& ls = iter->second;
    ConstRuleListIter lsiter = ls.begin();
    for( ; lsiter != ls.end(); lsiter++ ) {
    
      const_rule_t r = *lsiter;
      
      DBGS({
    std::cout << "\t[prestar] Rule2b match found:\t";
    r->print(std::cout) << std::endl;
      });
      // (p',g2,q)
      if( sat_base_class_t::ca.find(r->to_state(),r->to_stack1(),t->from_state(),tprime) ) {

    if(ewpds.is_element_of_P(t->from_state())) { // Apply Combination function

      //assert(dynamic_cast<ERule<SemiringElemPair<T,T>, T > *>(r));
      conf_fn_t cf = ((ERule<SemiringElemPair<T,T>, T > *)(r))->confluence_fn();
      se = new SemiringElemPair<T,T>(cf->apply_f(s.one().get_ptr(), L(tprime)->get_first()),s.one().get_ptr());
      se = s_elem.extend( se, delta );
      
      // (p1,g1,q')
      UPDATE( r->from_state(),
          r->from_stack(),
          t->to_state(),se,
          NEW_WITNESS_APPL(se,Query::prestar(),
                   r,tprime->witness(), t->witness()));
    } else { // Do not apply combination function

      // Correct weight on rule r
      fr = new SemiringElemPair<T,T>(F(r)->get_second(), s.one().get_ptr());
      se = s_elem.extend( L(tprime),delta );
      se = s_elem.extend( fr,se );
      
      // (p1,g1,q')
      UPDATE( r->from_state(),
          r->from_stack(),
          t->to_state(),se,
          NEW_WITNESS_APPL(se,Query::prestar(),
                   r,tprime->witness(), t->witness()));
    }
      }
    }
  }
  
  template< typename T > void ESaturationProcess<T>::prestar() {
    DBGS({
      std::cout << "-----\n\tSaturationProcess::prestar()\n-----\n";
      //std::cout << fa << std::endl;
    });
    // Rule hashes
    sat_rule_hash_t rule2b_map; // hash rules 2 on b hash code
    
    // what do you think it does?
    sat_base_class_t::populate_workset();
    
    // FIXME: Lets get rid of this nasty hash table
    // Hash rules of type 2 ( non default hash )
    // We do not use hash_rules b/c hash_codeb is needed..
    MakeRuleHashBActionFunctor<SemiringElemPair<T,T> > b_hasher( rule2b_map );
    ewpds.for_each_rule2( b_hasher );
    
    // Add transitions for rules of type 0
    UpdateRuleActionFunctor<SemiringElemPair<T,T> > apply_pre0( *this );
    ewpds.for_each_rule0( apply_pre0 );
    
    // vars for the chaotic iteration
    catrans_t t,tprime;
    sem_elem_t se;
    sem_elem_t delta;
    
    // Chaotic iteration
    while( !sat_base_class_t::workset_is_empty() ) {
      // t = (q,g,q')
      
      t = sat_base_class_t::get_from_workset();
      delta = DELTA(t);
      
      DBGS({
    std::cout << "Popped:\n\t ";
    t->print(std::cout) << " from workset.\n";
      });
      
      /************************************************/
      /* for all rule1s <p1,g1>-><q,g> --> (p1,g1,q') */
      /************************************************/
      
      APPLY_PRE1( ewpds,t,delta );
      
      /************************************************/
      /* for all rule2s <p1,g1>-><q,g g2> (Rule2a)    */
      /*        + (q',g2,g'') --> (p1,g1,q'')           */
      /************************************************/
      
      APPLY_PRE2a( ewpds,t,delta );
      
      /************************************************/
      /* for all rule2s <p1,y1>-><p',g2 g> (Rule2b)   */
      /*        + (p',g2,q)  -->  (p1,g1,q')            */
      /************************************************/
      
      APPLY_PRE2b( ewpds,t,delta,rule2b_map );
      
    }
    
#ifdef DBGDWPDS
    sat_base_class_t::ca.print( std::cout ) << std::endl;
#endif // DBGDWPDS
    
  }
  
  
  template< typename T >
    struct InjectTransActionFunctor : TransActionFunctor<T>
    {

      GEN_WPDS_TYPEDEFS(T);

      CA<SemiringElemPair<T,T> > *ca;

      InjectTransActionFunctor( CA<SemiringElemPair<T,T> > *ca_ ) {
    ca = ca_;
      }

      virtual void operator()( const catrans_t& t )
      {
    ref_ptr<CATransition<SemiringElemPair<T,T> > > tnew =
      ca->add( t->from_state(),
           t->stack(),
           t->to_state(),
           new SemiringElemPair<T,T>(t->semiring_element(),t->semiring_element()->one() ));
    //TSET_TRANS_WIT(tnew,t->witness());
    // FIXME: Take care of witnesses
      }

    };

  template< typename T >
    struct ProjectTransActionFunctor : TransActionFunctor<SemiringElemPair<T,T> >
    {

      GEN_WPDS_TYPEDEFS(T);

      CA<T> *ca;

      ProjectTransActionFunctor( CA<T> *ca_ ) {
    ca = ca_;
      }

      virtual void operator()( const ref_ptr<CATransition<SemiringElemPair<T,T> > > & t )
      {
    catrans_t tnew =
      ca->add( t->from_state(),
           t->stack(),
           t->to_state(),
           t->semiring_element()->get_first()->extend(t->semiring_element()->get_second()) );
    //TSET_TRANS_WIT(tnew,t->witness());
    // FIXME: Take care of witnesses
      }

    };

  template< typename T >
    inline void poststar(
             EWPDS<T>& wpds,
             const CA<T>& ca_in,
             Semiring<T>& s,
             CA<T> &ca_out )
    {
      SemiringElemPair<T,T> s_pair(s.get_data(),s.get_data(),1);
      Semiring<SemiringElemPair<T,T> > s_elem(&s_pair);
      ca_out.operator=(ca_in);
      CA<SemiringElemPair<T,T> > ca_actual(s_elem); 
      // Convert CA<T> to CA<T,T>
      InjectTransActionFunctor<T> copier(&ca_actual);
      ca_in.for_each(copier);

      wpds::ESaturationProcess<T> prcs(wpds,ca_actual,s,Query::poststar());
      prcs.poststar();
      // Convert back to CA<T>
      ProjectTransActionFunctor<T > project(&ca_out);
      ca_actual.for_each(project);
      // set ca_out.query to Query::poststar()
      ca_out.set_query (Query::poststar());
    }

  template< typename T >
    inline void prestar(
            EWPDS<T>& wpds,
            const CA<T>& ca_in,
            Semiring<T>& s,
            CA<T> &ca_out )
    {
      SemiringElemPair<T,T> s_pair(s.get_data(),s.get_data(),1);
      Semiring<SemiringElemPair<T,T> > s_elem(&s_pair);
      ca_out.operator=(ca_in);
      CA<SemiringElemPair<T,T> > ca_actual(s_elem); 
      // Convert CA<T> to CA<T,T>
      InjectTransActionFunctor<T> copier(&ca_actual);
      ca_in.for_each(copier);

      wpds::ESaturationProcess<T> prcs(wpds,ca_actual,s,Query::prestar());
      prcs.prestar();
      // Convert back to CA<T>
      ProjectTransActionFunctor<T > project(&ca_out);
      ca_actual.for_each(project);
      // set ca_out.query to Query::prestar()
      ca_out.set_query (Query::prestar());
    }
}
#endif
