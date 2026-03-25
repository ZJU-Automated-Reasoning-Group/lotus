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
 * $Id: confluence_functions.h,v 1.2 2005-09-06 16:11:35 radu Exp $
 */
#ifndef CONFLUENCE_FUNCTIONS_H_
#define CONFLUENCE_FUNCTIONS_H_
#include "../ref_ptr.h"
#include <stdio.h>


namespace wpds {
  template <typename SemiringData> class BaseCF {

  public:
    typedef ref_ptr<SemiringData> sem_elem_t;

    BaseCF(SemiringData *sr = NULL) : count(0), sr_data(sr) { }

    BaseCF(const BaseCF &that) : count(0), sr_data(that.sr_data.get_ptr()) { }

    virtual SemiringData *apply_f(SemiringData *w1, SemiringData *w2) const {
      assert(w1 != NULL && w2 != NULL);

      if(sr_data.get_ptr() == NULL) return w1->extend(w2);
      return w1->extend(sr_data->extend(w2));
    }

    virtual BaseCF *parse_element(const char *s, SemiringData *sem) {
      return new BaseCF(sem);
    }

    virtual std::ostream &print(std::ostream &o) {
      o << "BaseCF[";
      if(sr_data.get_ptr() == NULL) {
    o <<"ONE";
      } else {
    sr_data->print(o);
      }
      o<< "]";
      return o;
    }

    virtual ~BaseCF() { }

    unsigned int count;
  private:
    sem_elem_t sr_data;

  };


  // Requires ConfluenceData::apply_f : SemiringData x SemiringData -> SemiringData
  template <typename SemiringData, typename ConfluenceFnData> class ConfluenceFn : public BaseCF<SemiringData> {

  public:
    typedef ref_ptr<ConfluenceFnData> conf_fn_t;
    typedef ref_ptr<SemiringData> sem_elem_t;

    // The semiring element is for implementing a default action
    ConfluenceFn(SemiringData *sr, ConfluenceFnData *cf = NULL) : cf_data(cf), sr_data(sr) { }

    SemiringData *apply_f(SemiringData *w1,SemiringData *w2) const {
      // Both should not be intended to be NULL
      assert(cf_data.get_ptr() != NULL || sr_data.get_ptr() != NULL);
      assert(w1 != NULL && w2 != NULL);

      if(cf_data.get_ptr() != NULL) {
    return cf_data->apply_f(w1,w2);
      } else if(cf_data.get_ptr() == NULL && sr_data.get_ptr() == NULL) { 
    return w1->extend(w2);
      } else if(cf_data.get_ptr() == NULL) {
    return w1->extend(sr_data->extend(w2)); // default action
      }
    }

  private:
    conf_fn_t cf_data;
    sem_elem_t sr_data;
  };
}

#endif
