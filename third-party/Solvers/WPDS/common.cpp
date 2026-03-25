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
#include "common.h"
#include "dictionary.h"

namespace wpds {
    int rulesCount  = 0;
    int transCount  = 0;
    int bktsCount   = 0;
    int mhCount     = 0;

    std::ostream&
    Query::print( std::ostream& out ) const
    {
        return (out << to_string());
    }

    std::string
    Query::to_string() const
    {
        switch( query )
        {
            case WPDS_PRESTAR:      return "prestar";
            case WPDS_POSTSTAR:     return "poststar";
            case WPDS_BOTH:         return "both";
            case WPDS_BASE:default: return "user";
        }
    }

    Query Query::BASE(WPDS_BASE);
    Query Query::PRESTAR(WPDS_PRESTAR);
    Query Query::POSTSTAR(WPDS_POSTSTAR);
    Query Query::BOTH(WPDS_BOTH);

}

#ifdef DBGWPDS
int b_wpds_debug = 1;
int b_wpds_parse_debug = 0;
int b_dwpds_debug = 1;
int b_readout = 1;
int b_wpds_stat = 1;
#else
int b_wpds_debug = 0;
int b_wpds_parse_debug = 0;
int b_dwpds_debug = 0;
int b_readout = 0;
int b_wpds_stat = 0;
#endif

std::ostream * pWpdsErr = & std::cout;

/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
