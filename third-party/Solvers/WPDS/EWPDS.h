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
 * $Id: EWPDS.h,v 1.5 2007-06-24 22:16:30 akash Exp $
 */
#ifndef wpds_EWPDS_
#define wpds_EWPDS_
#include "../WPDS.h"
#include "../util/CA.intersect.h"
#include "ERule.h"
#include <fstream>
#include <iostream>

/* TODO:
 * 1. Witness preservation during pre * and post*
 * 2. Ease semiring and conf_func requirements for parse_ewpds
 */


/* The Extended weighted pushdown system class. Servers the same interface as WPDS except
 * that it has add_erule() functions for adding rules. Internally rules are stored in
 * WPDS<SemiringElemPair<T,T> > i.e. a rule r with weight w is stored as having weight
 * (w,1) if r is not a push rule and (1,w) if r is a push rule. 
 * The confluence functions have to be derieved from BaseCF class which is itself templated
 * on the semiring class. This avoids templatizing EWPDS on the confluence function class.
 */

namespace wpds {
  template<typename T> class ESaturationProcess;
  template< typename T > class ERuleExtender;

  template <typename T> class EWPDS : public WPDS<SemiringElemPair<T,T> >
  {
    friend class ESaturationProcess<T>;

  private:
    bool parse_erule( const char *buf,const char *sem_elem_start, BaseCF<T> *cf );

  public:     // methods
    typedef ref_ptr<SemiringElemPair<T,T> > sem_pair_t;
    typedef ref_ptr<BaseCF<T> > conf_func_t;
    typedef Rule<SemiringElemPair<T,T> > * rule_t;
    typedef std::list<rule_t >               RuleList;
    typedef HashMap< KeyTriple, RuleList, HashKeyTriple, EqualKeyTriple > Rule2SetHashCF;
    typedef GPP_IMP_TYPENAME__ Rule2SetHashCF::iterator Rule2SetHashIter;
    typedef GPP_IMP_TYPENAME__ Rule2SetHashCF::const_iterator ConstRule2SetHashIter;
    typedef GPP_IMP_TYPENAME__ RuleList::const_iterator RuleListConstIter;
    typedef std::pair< RuleListConstIter,RuleListConstIter > RuleListConstIterPair;
    typedef BaseCF<T> * (*CONF_FUNC_INIT)(void);

    /* Constructor / Destructor : Initializes SemiringElemPair<T,T> */
    EWPDS( Semiring<T>& s_,
       Query q = Query::both())
      : s_orig(s_), extender(NULL)
      {
        SemiringElemPair<T,T>::init(s_.get_data(),s_.get_data());
        sem_pair_t s_orig_pair = new SemiringElemPair<T,T>(s_.get_data(),s_.get_data());
        Semiring<SemiringElemPair<T, T> > s_elem(s_orig_pair.get_ptr());
        WPDS<SemiringElemPair<T,T> >(s_elem,q);
        //owpds = new WPDS<SemiringElemPair<T,T> >(*s_elem.get_ptr(),q);
      }
    
    // The constructor takes away ownership of extender__
    EWPDS( Semiring<T>& s_,
       ERuleExtender<T> *extender__,
       Query q = Query::both())
      : s_orig(s_), extender(extender__)
      {
        SemiringElemPair<T,T>::init(s_.get_data(),s_.get_data());
        sem_pair_t s_orig_pair = new SemiringElemPair<T,T>(s_.get_data(),s_.get_data());
        Semiring<SemiringElemPair<T, T> > s_elem(s_orig_pair.get_ptr());
        WPDS<SemiringElemPair<T,T> >(s_elem,q);
        //owpds = new WPDS<SemiringElemPair<T,T> >(*s_elem.get_ptr(),extender__,q);
      }
    
    virtual ~EWPDS() { 
      if(extender) {
        delete extender;
      }
    }

    void add_erule(
          const wpds_key_t p,
          const wpds_key_t y,
          const wpds_key_t q,
          T *t)
      {
        sem_pair_t s_pair = new SemiringElemPair<T,T>(t,s_orig.one());
        rule_t r = new ERule<SemiringElemPair<T,T>, T >(s_pair,p,y,q);
        p_insert_rule( r,Rule<SemiringElemPair<T,T> >::RULE0 );
      }
    
    void add_erule(
          const wpds_key_t p, const wpds_key_t y,
          const wpds_key_t q, const wpds_key_t g1,
          T *t)
      {
        if( g1 != WPDS_EPSILON ) {
          sem_pair_t s_pair = new SemiringElemPair<T,T>(t,s_orig.one());
          rule_t r = new ERule<SemiringElemPair<T,T>, T>(s_pair,p,y,q,g1);
          p_insert_rule( r,Rule<SemiringElemPair<T,T> >::RULE1 );
        } /* someone did not follow the calling convention...*/
        else
          add_erule(p,y,q,t);
      }
    
    void add_erule(
          const wpds_key_t p, const wpds_key_t y,
          const wpds_key_t q, const wpds_key_t g1,
          const wpds_key_t g2, T *t, BaseCF<T> *f = NULL)
    {
      sem_pair_t s_pair;
      if( g2 != WPDS_EPSILON ) {
        s_pair = new SemiringElemPair<T,T>(s_orig.one(),t);
                  conf_func_t mf = f;
        if(f==NULL) {
          mf = new BaseCF<T>(t); 
        }
        rule_t r = new ERule<SemiringElemPair<T,T>, T >(s_pair,p,y,q,g1,g2,mf.get_ptr());
        int retval = p_insert_rule( r,Rule<SemiringElemPair<T,T> >::RULE2 );
                  if(retval != 3) {
                     // p_insert_rule takes ownership of the rule. If retval !=3 then r was a duplicate rule
                          // and it must have been deleted, after taking combine on the weights. For EWPDS, we still need
                          // this rule because the marge function might be different, and there is no way of taking combine
                          // on merge functions
                          // For now, recreate the rule. A better solution would be to change p_insert_rule
           r = new ERule<SemiringElemPair<T,T>, T >(s_pair,p,y,q,g1,g2,mf.get_ptr());
                  }

        // Insert r into hash table
        KeyTriple kt(q,g1,g2);
        Rule2SetHashIter r2shi = rules2Hash.find(kt);
        if(r2shi == rules2Hash.end()) {
          RuleList rs;
          rs.push_back(r);
          rules2Hash.insert(kt,rs);
        } else {
          r2shi->second.push_back(r);
        }
      } /* someone did not follow the calling convention...*/
      else {
        assert(f == NULL); // don't pass f with a non-call rule
        add_erule(p,y,q,g1,t);
      }
    }

    // Matches push rules according to their tail
    RuleListConstIterPair match_rule(const KeyTriple &kt) const {
      RuleListConstIterPair p;
      ConstRule2SetHashIter r2shi = rules2Hash.find(kt);
      if(r2shi != rules2Hash.end()) {
        p = RuleListConstIterPair(r2shi->second.begin(), r2shi->second.end());
      } else {
        p = RuleListConstIterPair(EMPTY_RULE_SET.begin(),EMPTY_RULE_SET.end());
      }
      return p;
    }

#ifdef EWPDS_PARSE_FILE
    bool eparse_string( char *buf, BaseCF<T> *cf);
    static EWPDS<T>* WPDS_CALL parse_ewpds(char *filename, CONF_FUNC_INIT init, int max_line_len );
#endif

  private:
    Semiring<T> &s_orig;
    ERuleExtender<T> *extender;
    Rule2SetHashCF rules2Hash;
  };

#ifdef EWPDS_PARSE_FILE

  template< typename T>
  EWPDS<T> *EWPDS<T>::parse_ewpds(char *filename, CONF_FUNC_INIT init, int max_line_len = 1024) {
    EWPDS<T> *ewpds = NULL;
    conf_func_t cf(NULL);
    std::fstream file(filename);
    char *line;
    line = new char[max_line_len];
    while(true) {
      file.getline(line,max_line_len);
      if(file.eof()) break;
      if(line[0]==0) continue; // empty line
      if(line[0]=='*') continue; // comment
      if(line[0]=='_') { // Semiring directive
    T::init_directive(line);
    continue;
      }
      // now we expect a EWPDS rule
      // and we expect the semiring to have initialized by now
      if(ewpds == NULL) {
    Semiring<T> s(T::getOne());
    cf = init();
    ewpds = new EWPDS<T>(s, Query::poststar());
      }
      int b = ewpds->eparse_string(line,cf.get_ptr());
      if(b==false) { // an error occurred
    T::init_directive(NULL); // tell semiring to forget earlier directives
    file.close();
    delete [] line;
    return NULL;
      }
    }
    delete [] line;
    file.close();
    return ewpds;
  }

  template< typename T >
    bool EWPDS<T>::eparse_string( char *buf, BaseCF<T> *cf = NULL )
    {
      char *tmp = 0;
      char *tmp2 = 0;
      char hold_arrow_start;
      int i=0;
      tmp = strstr( buf, "->" );
      if( !tmp ) {
        WPDS<SemiringElemPair<T,T> >::p_bad_parse_error_msg( buf );
        return false;
      }
      hold_arrow_start = *tmp;
      *tmp = 0;
      tmp2 = strchr( buf, '[' );
      if(tmp2) return false; // No schemas allowed for EWPDS
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
          WPDS<SemiringElemPair<T,T> >::p_bad_parse_error_msg( buf );
          return false;
        }
      }
      //printf("tmp --> %s\n",tmp);
      return parse_erule( buf,tmp,cf );
    } 

  /*
   * p_parse_erule 
   *
   * Parses a rule out of the buf according to the 
   * defined parse rules (see WPDS.h).
   *
   * sem_elem_start points to the first character after the rule/schema
   * so that we may call s.parse_element( sem_elem_start ).  This should
   * return a semiring element (a nice way of saying T* )
   */

  template< typename T >
    bool EWPDS<T>::parse_erule( const char *buf,const char *sem_elem_start, BaseCF<T> *cf = NULL)
    {
      int rulenum;
      char fstate[1024],fstack[1024];
      char tstate[1024],tstack1[1024],tstack2[1024];
      int rs;
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
        //rule_type = Rule<T>::RULE2;
        tstk2 = str2key(tstack2);
        tstk1 = str2key(tstack1);
        break;
      case 5:
        //rule_type = Rule<T>::RULE1;
        tstk1 = str2key(tstack1);
        break;
      case 4:
        //rule_type = Rule<T>::RULE0;
        break;
      default:
        WPDS<SemiringElemPair<T,T> >::p_bad_parse_error_msg( buf );
        return false;
      }
      fstt = str2key(fstate);
      fstk = str2key(fstack);
      tstt = str2key(tstate);
      if(rs == 6 && cf != NULL) { // TYPE2 rule. So we need to parse a cf
        char *cf_elem_start;
        Semiring<T> s(s_orig.parse_element(sem_elem_start, cf_elem_start).get_ptr());
        BaseCF<T> *the_cf = cf->parse_element(cf_elem_start, s.get_data());
        add_erule(fstt,fstk,tstt,tstk1,tstk2,s.get_data(),the_cf);
      } else {
        Semiring<T> s(s_orig.parse_element(sem_elem_start).get_ptr());
        add_erule(fstt,fstk,tstt,tstk1,tstk2,s.get_data());
      }
      return true;
    }
#endif // EWPDS_PARSE_FILE



}

#include "ERuleExtender.h"
#include "ESaturationProcess.h"
#endif
