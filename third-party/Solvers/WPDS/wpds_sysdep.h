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
#ifndef __WPDS_SYSDEF_GUARD
#define __WPDS_SYSDEF_GUARD
// Handle g++/msvc++ differences
#ifdef __GNUG__ 
#   define GPP_IMP_TYPENAME__ typename
#   if __GNUG__ < 3
#       define SETW(w) ""
#       define UNUSED_VAR
#   else
#       define UNUSED_VAR __attribute__((unused))
#   endif /* __GNUG__ < 3 */
#   // Define this to 'nothing' for GNU (presumably on Unix)
#   define WPDS_CALL 
#elif defined _WIN32
#   pragma once
#   define UNUSED_VAR
#   if _MSC_VER > 1000
#       pragma warning(disable: 4786)
#   endif
#   if _MSC_VER > 1200
#       define GPP_IMP_TYPENAME__ typename
#   else
#       define GPP_IMP_TYPENAME__
#   endif
#   //RTTI needed by WPDS++
#   ifndef _CPPRTTI
#       error RTTI is required by WPDS++
#   endif
#   // Calling convention for static class members
#   define WPDS_CALL __cdecl
#else
#   define WPDS_CALL
#endif

#ifndef SETW
# define SETW(w) std::setw(8)
#endif /* SETW */
#endif /* __WPDS_SYSDEF_GUARD */
