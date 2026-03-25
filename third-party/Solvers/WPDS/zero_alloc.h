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
 * $Id: zero_alloc.h,v 1.4 2005-09-06 16:11:34 radu Exp $
 */
#ifndef RANCH_ALLOC_LOCK__
#define RANCH_ALLOC_LOCK__ 1

#include <stl_alloc.h>

// Allocator adaptor to check size arguments for debugging.
// Reports errors using assert.  Checking can be disabled with
// NDEBUG, but it's far better to just use the underlying allocator
// instead when no checking is desired.
// There is some evidence that this can confuse Purify.
template <class _Alloc>
class zero_alloc {

private:

  enum {_S_extra = 8};  // Size of space used to store size.  Note
                        // that this must be large enough to preserve
                        // alignment.

public:

  static void* WPDS_CALL allocate(size_t __n)
  {
    char* __result = (char*)_Alloc::allocate(__n + _S_extra);
    *(size_t*)__result = __n;
    for(size_t i = _S_extra; i < __n + _S_extra; ++i)
        ((char*)__result)[i] = 0;
    return __result + _S_extra;
  }

  static void WPDS_CALL deallocate(void* __p, size_t __n)
  {
    char* __real_p = (char*)__p - _S_extra;
    assert(*(size_t*)__real_p == __n);
    for(size_t i = 0; i < __n; ++i)
        ((char*)__p)[i] = 0;
    _Alloc::deallocate(__real_p, __n + _S_extra);
  }

  static void* WPDS_CALL reallocate(void* __p, size_t __old_sz, size_t __new_sz)
  {
    char* __real_p = (char*)__p - _S_extra;
    assert(*(size_t*)__real_p == __old_sz);
    for(size_t i = 0; i < __old_sz; ++i)
        ((char*)__p)[i] = 0;
    char* __result = (char*)
      _Alloc::reallocate(__real_p, __old_sz + _S_extra, __new_sz + _S_extra);
    *(size_t*)__result = __new_sz;
    for(size_t i = _S_extra; i < __new_sz + _S_extra; ++i)
        ((char*)__result)[i] = 0;
    return __result + _S_extra;
  }

};

#ifdef __STL_USE_STD_ALLOCATORS

template <class _Alloc>
inline bool operator==(const zero_alloc<_Alloc>&,
                       const zero_alloc<_Alloc>&) {
  return true;
}

#ifdef __STL_FUNCTION_TMPL_PARTIAL_ORDER
template <class _Alloc>
inline bool operator!=(const zero_alloc<_Alloc>&,
                       const zero_alloc<_Alloc>&) {
  return false;
}
#endif /* __STL_FUNCTION_TMPL_PARTIAL_ORDER */


template <class _Tp, class _Alloc>
struct _Alloc_traits<_Tp, zero_alloc<_Alloc> >
{
  static const bool _S_instanceless = true;
  typedef simple_alloc<_Tp, zero_alloc<_Alloc> > _Alloc_type;
  typedef __allocator<_Tp, zero_alloc<_Alloc> > allocator_type;
};

template <class _Tp, class _Tp1, class _Alloc>
struct _Alloc_traits<_Tp, __allocator<_Tp1, zero_alloc<_Alloc> > >
{
  static const bool _S_instanceless = true;
  typedef simple_alloc<_Tp, zero_alloc<_Alloc> > _Alloc_type;
  typedef __allocator<_Tp, zero_alloc<_Alloc> > allocator_type;
};
#endif

typedef zero_alloc<malloc_alloc> zero_malloc_alloc;


#endif  /* RANCH_ALLOC_LOCK__ */

/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
