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
 * $Id: inst_counter.h,v 1.7 2005-09-06 16:11:34 radu Exp $
 */
#ifndef WPDS_INST_COUNTER_H_
#define WPDS_INST_COUNTER_H_

#include <iomanip>
#include <iostream>
#include <typeinfo>
#if PI_STATS_DETAIL
#include <set>
#endif /* PI_STATS_DETAIL */
#include "Utils/LLVM/Demangle.h"
#include "myallocator.h"

struct InstCounter {
    InstCounter(const char *s) : count(0), sz(s), ti(NULL), size(0) { 
    }
    InstCounter(const std::type_info& ti_, size_t size_) : 
        count(0), sz(NULL), ti(&ti_), size(size_)  { 
#if PI_STATS_DETAIL
        counters.insert(this);
#endif /* PI_STATS_DETAIL */
    }
    ~InstCounter() {
        if( count > 0 ) {
            std::cout << "Warning: there were " << count
                      << " instances of " << name() 
                      << " that were not deleted."
                      << '\n';
        }
    }
    
    void operator++() { 
        ++count; 
    }
    void operator--() { --count; }
#if PI_STATS_DETAIL
    void show(const char* prefix = "") {
        std::cout << prefix
                  << std::setw(8) << size
                  << std::setw(8) << count 
                  << std::setw(16) << (count >= 0 ? size*count : 0)
                  << " " << name() << std::endl;
    } /* show */
    static void WPDS_CALL show_counters(const char* prefix = "") {
        std::cout << prefix
                  << std::setw(8) << "size"
                  << std::setw(8) << "count"
                  << std::setw(16) << "bytes"
                  << std::endl;
        for(STDSET( InstCounter* )::iterator iter = counters.begin();
            iter != counters.end(); iter++ ) {
            (*iter)->show(prefix);
        }
    } /* show_counters */
    static void WPDS_CALL show_total(const char* prefix = "") {
        unsigned long total = 0;
        for(STDSET( InstCounter* )::iterator iter = counters.begin();
            iter != counters.end(); iter++ ) {
            if ((*iter)->count > 0) {
                total += (*iter)->count * (*iter)->size;
            }
        }
        std::cout << prefix << " instances total memory usage: "
                  << total << std::endl;
    } /* show_counters */
#endif /* PI_STATS_DETAIL */

    private:

    long count;                 // use signed so we can catch errors
    const char *sz;
    const std::type_info* ti;
    const size_t size;
    std::string name(void) const {
        if (sz) {
            return sz;
        } else {
            return DemangleUtils::demangle(ti->name());
        } /* if */
    } /* name */

#if PI_STATS_DETAIL
    static std::set<InstCounter*> counters;
#endif /* PI_STATS_DETAIL */
};

#if PI_STATS_DETAIL && PI_STATS_DEFINE_VARIABLES
std::set<InstCounter*> InstCounter::counters
#if __GNUG__
__attribute__ ((init_priority (201)));
#else /* __GNUG__ */
#error -- You need to guarantee that InstCounter::counters is initialized before any InstCounterBase<T, R>::counter is created
#endif /* __GNUG__ */
;
#endif /* PI_STATS_DETAIL && PI_STATS_DEFINE_VARIABLES */

#if PI_STATS_DETAIL

// InstCounterBase is for counting instances of classes.
// To count the instances of a class, make InstCounterBase
// its base class. If the class has another baseclass that
// is being counted, use InstCounterBaseRev to counter
// the effect unless you want both the subobject (the baseclass)
// and the complete object counted.

struct InstCounterBaseRevNop 
{
    static void WPDS_CALL dec() {}
    static void WPDS_CALL inc() {}
};

// NOTE: Keep this an empty class

// NOTE: T doesn't have to be the class being counted.  Classes you
// want to be counted together can use the same T. However, using a T
// different than the class being counted could cause the "instances
// total memory usage:" being wrong.
template< typename T, class R = InstCounterBaseRevNop >
struct InstCounterBase {
    InstCounterBase() {
        ++counter;
        R::dec();
    } // InstCounterBase
    InstCounterBase(const InstCounterBase<T, R> &) {
        ++counter;
        R::dec();
    } // InstCounterBase
    ~InstCounterBase() {
        --counter;
        R::inc();
    } // InstCounterBase
    static InstCounter counter;
};

template< typename T >
struct InstCounterBaseRev 
{
    static void WPDS_CALL dec() {
        --InstCounterBase<T>::counter;
    }        
    static void WPDS_CALL inc() {
        ++InstCounterBase<T>::counter;
    }
};

// FIXME: This one seems to get instantiated/initialized only after
// InstCounterBase finished executing if it is executed for a global
// variable. So we may miss counts of global variables unless 
// explict instantiation is done
template< typename T, class R > InstCounter InstCounterBase<T, R>::counter(typeid(T), sizeof(T))
#if __GNUG__
__attribute__ ((init_priority (211))); /* after counters */
#else /* __GNUG__ */
#error -- You need to guarantee that InstCounterBase<T, R>::counter is initialized before any classed counted by it is created
#endif /* __GNUG__ */
;
// Use this when the counter is the only base class of the class that is
// counted
#define PI_STATS_INST_COUNT(T) : private InstCounterBase< T >
// Use this when there are other base classes but none of them has
// InstCounterBase as its (direct or indirect) base class. Therefore
// we don't need to decrease their counts.
#define PI_STATS_INST_COUNTm0(T) , private InstCounterBase< T >
// Use this when there are other base classes and one of them has
// InstCounterBase as its (direct or indirect) base class and we
// need to decrease its count. 
#define PI_STATS_INST_COUNTm(T, R) , private InstCounterBase< T, InstCounterBaseRev<R> >
#else /* PI_STATS_DETAIL */
#define PI_STATS_INST_COUNT(T)
#define PI_STATS_INST_COUNTm0(T)
#define PI_STATS_INST_COUNTm(T, R)
#endif /* PI_STATS_DETAIL */
#endif // WPDS_INST_COUNTER_H_
/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
