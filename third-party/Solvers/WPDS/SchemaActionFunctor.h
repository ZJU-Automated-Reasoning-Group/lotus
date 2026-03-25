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
 * $Id: SchemaActionFunctor.h,v 1.4 2005-09-06 16:11:35 radu Exp $
 */
#ifndef WPDS_UTIL_SCHEMA_ACTION_FUNCTOR_H_
#define WPDS_UTIL_SCHEMA_ACTION_FUNCTOR_H_

#include <iostream>

namespace wpds {

    template< typename T > class Schema;

    namespace util {

        template< typename T > struct SchemaActionFunctor
        {
            SchemaActionFunctor() {}

            virtual ~SchemaActionFunctor() {}

            virtual void operator()( const Schema<T> *s )  = 0;

        };

        template< typename T >
        struct WriteSchemaActionFunctor : SchemaActionFunctor<T>
        {
            std::ostream& out;
            int i;

            WriteSchemaActionFunctor( std::ostream& o ) : out(o),i(0) {}

            virtual ~WriteSchemaActionFunctor() {}

            virtual void operator()( const Schema<T> *s )
            {
                s->print( out << "\t" << i++ <<")\t" ) << std::endl;
            }

        };

        template< typename T >
        struct DeleteSchemaActionFunctor : SchemaActionFunctor<T>
        {
            DeleteSchemaActionFunctor( ) {}

            virtual ~DeleteSchemaActionFunctor() {}

            virtual void operator()( const Schema<T> *s )
            {
                //s->print(std::cerr << "Deleting : ") << std::endl;
                delete s;
            }

        };


    }// namespace util

}    // namespace wpds

#endif //WPDS_UTIL_SCHEMA_ACTION_FUNCTOR_H_
