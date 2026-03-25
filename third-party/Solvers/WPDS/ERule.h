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
 * $Id: ERule.h,v 1.2 2005-09-06 16:11:35 radu Exp $
 */
#ifndef wpds_ERULE_H_
#define wpds_ERULE_H_
#include "../ref_ptr.h"
#include "../inst_counter.h"
#include "../semiring.h"
#include "../Traits.h"
#include "../Rule.h"
#include "confluence_functions.h"
#include <iostream>


/* The Extended-Rule class. It just stores an extra confluence function object
 * of type BaseCF<CT>
 */

namespace wpds {
  template<typename T> class EWPDS;

  template<typename T, typename CT> class ERule : public Rule<T> {

    friend class EWPDS<T>;
    friend class ref_ptr<ERule>;

    friend std::ostream& operator <<(std::ostream &o, const ERule &r) {
      return r.eprint(o);
    }
  private:
    typedef ref_ptr<BaseCF<CT> > conf_fn_t;
    typedef ref_ptr<T > sem_elem_t;
    
  public:
    /* Constructor/Destructor */
    ERule(
     T *t,
     const wpds_key_t from_state = WPDS_EPSILON,
     const wpds_key_t from_stack = WPDS_EPSILON,
     const wpds_key_t to_state   = WPDS_EPSILON,
     const wpds_key_t to_stack1  = WPDS_EPSILON,
     const wpds_key_t to_stack2  = WPDS_EPSILON,
     BaseCF<CT> *f = NULL
     )
      : Rule<T>(t,from_state,from_stack,to_state,to_stack1,to_stack2)
      { cf = f;
      //incRuleCount(); 
      }
    
    ERule( 
     const sem_elem_t& se_,
     const wpds_key_t from_state = WPDS_EPSILON,
     const wpds_key_t from_stack = WPDS_EPSILON,
     const wpds_key_t to_state   = WPDS_EPSILON, 
     const wpds_key_t to_stack1  = WPDS_EPSILON, 
     const wpds_key_t to_stack2  = WPDS_EPSILON,
     BaseCF<CT> *f = NULL
     )
      : Rule<T>(se_, from_state, from_stack, to_state, to_stack1, to_stack2)
      { cf = f;
      //incRuleCount(); 
      }
    
    ~ERule() { 
      //decRuleCount(); 
    }

    /*******************/
    /* Getters         */
    /*******************/  
    const conf_fn_t& confluence_fn() const { return cf; }
    
    /*******************/
    /* Others          */
    /*******************/
    std::ostream& eprint( std::ostream& o ) const {
      Rule<T>::print(o);
      if(cf.get_ptr() == NULL) return o;
      return cf->print(o);
    }

  private:
    conf_fn_t cf;
  };
}

#endif
