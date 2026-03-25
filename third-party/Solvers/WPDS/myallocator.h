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
 * $Id: myallocator.h,v 1.6 2005-06-22 22:05:11 kidd Exp $
 */
#ifndef WPDS_MYALLOCATOR_H_
#define WPDS_MYALLOCATOR_H_
// class my_alloc
// 
// Overwrites allocate and deallocate of std::allocator to
// - collect STL memory usage, and
// - optionally malloc and free memory ourselves. (Just uncomment
//   the code in allocate and deallocate.)
//
// The reason for using malloc and free is that some std::allocator
// implementations have their own memory pools for small size
// allocation, for effeciency. Memory in pools are not freed until
// program terminates. Using our own allocate and deallocate let us
// see what is the real memory requirement.
//
#if PI_STATS_DETAIL
#if !__GNUG__
#error -- my_alloc has only been tested with g++ 3.3 on spitfire. The code may compile under other compiler/STL but the result may not be correct. Check before you use it.
#endif /* __GNUG__ */
#include <memory>
#include <iostream>
#include <cstdlib>
#include <typeinfo>
#include <map>
#include "common.h"
template <typename T> class my_alloc;
template<> class my_alloc<void> : public std::allocator<void> {
public:
    template <class U> struct rebind { typedef my_alloc<U> other; };
};
#if PI_STATS_DETAIL
extern long total_my_allocator_bytes;
typedef std::map<const std::type_info*, long> my_allocator_bytes_t;
extern my_allocator_bytes_t my_allocator_bytes;
inline void my_allocator_bytes_record(const std::type_info& ti,
                                      long size) {
    my_allocator_bytes_t::iterator i;
    i = my_allocator_bytes.find(&ti);
    if (i == my_allocator_bytes.end()) {
        my_allocator_bytes[&ti] = size;
    } else {
        my_allocator_bytes[&ti] += size;
    }
    total_my_allocator_bytes += size;
}
#define RECORD_ALLOC(T, count) my_allocator_bytes_record(typeid(T), sizeof(T)*n)
#define RECORD_DEALLOC(T, count) my_allocator_bytes_record(typeid(T), -(sizeof(T)*n))
#else /* PI_STATS_DETAIL */
#define RECORD_ALLOC(T, bytes)
#define RECORD_DEALLOC(T, bytes)
#endif /* PI_STATS_DETAIL */
template <typename T> class my_alloc : public std::allocator<T> {
  public:
    
    typedef GPP_IMP_TYPENAME__ std::allocator<T>::pointer pointer;
    typedef GPP_IMP_TYPENAME__ std::allocator<T>::size_type size_type;

    template <class U> struct rebind { typedef my_alloc<U> other; };

#ifndef _WIN32
    my_alloc() throw() {}
    my_alloc(const my_alloc&) {}
    // g++ 3.3
    template <typename U>
        my_alloc(const my_alloc<U>&) throw() {}
#endif /* _WIN32 */
        

    pointer allocate(size_type n,
                     std::allocator<void>::const_pointer hint=0) {
        RECORD_ALLOC(T, n);
        return std::allocator<T>::allocate(n, hint);
        //return (pointer)malloc(sizeof(T) * n);
    } // allocate

#ifdef _WIN32
    // The standard says the first arg is pointer, MSVC uses void*
    void deallocate(void* p, size_type n) {
#else  // _WIN32
    void deallocate(pointer p, size_type n) {
#endif // _WIN32
        RECORD_DEALLOC(T, n);
        static_cast<std::allocator<T>*>(this)->deallocate(p, n);
        //free(p);
    } // deallocate

#ifdef _WIN32
#error -- my_alloc::_Charalloc not tested
    // FIXME: I did my orignal experiment on a Windows machine with MSVC
    // 6.0. It seems to me that its STL doesn't always use allocate
    // to get memory. The STL sometimes just call _Charalloc -- which
    // is not in the standard at all -- directly.
    // At that time I was just trying to figure out whether allocate
    // is called and I didn't pay attention to when who is called.
    char _FARQ *_Charalloc(size_type _N) {
        return static_cast<std::allocator<T>*>(this)->_Charalloc(_N);
    } // _Charalloc
#endif // _WIN32
};
#endif /* PI_STATS_DETAIL */

// Define some macros to be used in declaring objects of STL containers.
// Using of these macros are not mandatory but would make life easier
// if we ever need to trace memory usage of STL.
#if PI_STATS_DETAIL
#define MYALLOC my_alloc
#define STDLIST(T) std::list<T, MYALLOC<T > >
#define STDMAP(T1, T2) std::map<T1, T2, std::less<T1 >, MYALLOC<T2 > >
#define STDMULTIMAP(T1, T2) std::multimap<T1, T2, std::less<T1 >, MYALLOC<T2 > >
// Compare is given
#define STDMULTIMAP2(T1, T2, less) std::multimap<T1, T2, less, MYALLOC<T2 > >
#define STDSET(T) std::set<T, std::less<T >, MYALLOC<T > >
// Compare is given
#define STDSET2(T, less) std::set<T, less, MYALLOC<T > >
#else /* PI_STATS_DETAIL */
#define MYALLOC std::allocator
#define STDLIST(T) std::list<T >
#define STDMAP(T1, T2) std::map<T1, T2 >
#define STDMULTIMAP(T1, T2) std::multimap<T1, T2 >
#define STDMULTIMAP2(T1, T2, less) std::multimap<T1, T2, less >
#define STDSET(T) std::set<T >
#define STDSET2(T, less) std::set<T, less >
#endif /* PI_STATS_DETAIL */

#endif // WPDS_MYALLOCATOR_H_
