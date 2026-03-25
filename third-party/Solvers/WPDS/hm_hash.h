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
 * $Id: hm_hash.h,v 1.10 2005-09-06 16:11:34 radu Exp $
 */
#ifndef HASH_MAP_DEFAULTS_H_
#define HASH_MAP_DEFAULTS_H_
#include <cstring> // for hm_equal< char * >
#include "common.h" // for wpds_size_t


/* These were taken from Thomas Wang
 * http://www.concentric.net/~Ttwang/tech/inthash.htm
 *
 * On machines where long is 64 bits, the below should
 * probably be used.
 *
 
 long longhash1(long key)
 {
    key += ~(key << 32);
    key ^= (key >>> 22);
    key += ~(key << 13);
    key ^= (key >>> 8);
    key += (key << 3);
    key ^= (key >>> 15);
    key += ~(key << 27);
    key ^= (key >>> 31);
    return key;
 }

 */

#undef jenkins_mix
#define jenkins_mix(a,b,c) \
{ \
    a=a-b;  a=a-c;  a=a^(c>>13); \
    b=b-c;  b=b-a;  b=b^(a<<8);  \
    c=c-a;  c=c-b;  c=c^(b>>13); \
    a=a-b;  a=a-c;  a=a^(c>>12); \
    b=b-c;  b=b-a;  b=b^(a<<16); \
    c=c-a;  c=c-b;  c=c^(b>>5);  \
    a=a-b;  a=a-c;  a=a^(c>>3);  \
    b=b-c;  b=b-a;  b=b^(a<<10); \
    c=c-a;  c=c-b;  c=c^(b>>15); \
}

#undef jenkins_wrapper
#define jenkins_wrapper( c )            \
{                                       \
    wpds_size_t a = 0x9e3779b9;     \
    wpds_size_t b = 0x9e3779b9;     \
    jenkins_mix(a,b,c);             \
}

#undef wpds_primitive_hash_type
#define wpds_primitive_type_hash( key ) \
{                                       \
    key += (key << 12);                 \
    key ^= (key >> 22);                 \
    key += (key << 4);                  \
    key ^= (key >> 9);                  \
    key += (key << 10);                 \
    key ^= (key >> 2);                  \
    key += (key << 7);                  \
    key ^= (key >> 12);                 \
}

namespace wpds {

    /*
     * hm_hash is a templated functor to provide defaults
     * for hashing the c++ primitive types.  It is defined
     * because std::hash<> is currently an SGI extension
     * to the STL.
     */

    template< typename T > struct hm_hash {};
    template< typename T > struct hm_equal {};

    /*
     * Primitives hm_hash is defined for (includes const prim):
     *      int
     *      unsigned int
     *      char
     *      unsigned char
     *      char *
     */


    /********************/
    /* char             */
    /********************/
    template<> struct hm_hash< char >
    {
        wpds_size_t operator()( char key ) const
        {
            wpds_size_t sz = key;
            wpds_primitive_type_hash( sz );
            return sz;
        }
    };
    template<> struct hm_hash< const char >
    {
        wpds_size_t operator()( const char key ) const
        {
            wpds_size_t sz = key;
            wpds_primitive_type_hash( sz );
            return sz;
        }
    };
    template<> struct hm_equal< char >
    {
        bool operator()( char lhs,char rhs ) const
        {
            return lhs == rhs;
        }
    };
    template<> struct hm_equal< const char >
    {
        bool operator()( const char lhs,const char rhs ) const
        {
            return lhs == rhs;
        }
    };

    /********************/
    /* unsigned char    */
    /********************/
    template<> struct hm_hash< unsigned char >
    {
        wpds_size_t operator()( unsigned char key ) const
        {
            wpds_size_t sz = key;
            wpds_primitive_type_hash( sz );
            return sz;
        }
    };
    template<> struct hm_hash< const unsigned char >
    {
        wpds_size_t operator()( const unsigned char key ) const
        {
            wpds_size_t sz = key;
            wpds_primitive_type_hash( sz );
            return sz;
        }
    };
    template<> struct hm_equal< unsigned char >
    {
        bool operator()( unsigned char lhs,unsigned char rhs ) const
        {
            return lhs == rhs;
        }
    };
    template<> struct hm_equal< const unsigned char >
    {
        bool operator()( const unsigned char lhs,const unsigned char rhs ) const
        {
            return lhs == rhs;
        }
    };

    /********************/
    /* int              */
    /********************/
    template<> struct hm_hash< int >
    {
        wpds_size_t operator()( int key ) const
        {
            wpds_size_t sz = key;
            wpds_primitive_type_hash( sz );
            return sz;
        }
    };
    template<> struct hm_hash< const int >
    {
        wpds_size_t operator()( const int key ) const
        {
            wpds_size_t sz = key;
            wpds_primitive_type_hash( sz );
            return sz;
        }
    };
    template<> struct hm_equal< int >
    {
        bool operator()( int lhs,int rhs ) const
        {
            return lhs == rhs;
        }
    };
    template<> struct hm_equal< const int >
    {
        bool operator()( const int lhs,const int rhs ) const
        {
            return lhs == rhs;
        }
    };

    /********************/
    /* unsigned int */
    /********************/
    template<> struct hm_hash< unsigned int >
    {
        wpds_size_t operator()( int key ) const
        {
            wpds_size_t sz = key;
            wpds_primitive_type_hash( sz );
            return sz;
        }
    };
    template<> struct hm_hash< const unsigned int >
    {
        wpds_size_t operator()( const unsigned int key ) const
        {
            wpds_size_t sz = key;
            wpds_primitive_type_hash( sz );
            return sz;
        }
    };
    template<> struct hm_equal< unsigned int >
    {
        bool operator()( unsigned int lhs,unsigned int rhs ) const
        {
            return lhs == rhs;
        }
    };
    template<> struct hm_equal< const unsigned int >
    {
        bool operator()( const unsigned int lhs,const unsigned int rhs ) const
        {
            return lhs == rhs;
        }
    };

    /********************/
    /* long              */
    /********************/
    template<> struct hm_hash< long >
    {
        wpds_size_t operator()( long key ) const
        {
            wpds_size_t sz = key;
            wpds_primitive_type_hash( sz );
            return sz;
        }
    };
    template<> struct hm_hash< const long >
    {
        wpds_size_t operator()( const long key ) const
        {
            wpds_size_t sz = key;
            wpds_primitive_type_hash( sz );
            return sz;
        }
    };
    template<> struct hm_equal< long >
    {
        bool operator()( long lhs,long rhs ) const
        {
            return lhs == rhs;
        }
    };
    template<> struct hm_equal< const long >
    {
        bool operator()( const long lhs,const long rhs ) const
        {
            return lhs == rhs;
        }
    };

    /********************/
    /* unsigned long    */
    /********************/
    template<> struct hm_hash< unsigned long >
    {
        wpds_size_t operator()( long key ) const
        {
            wpds_size_t sz = key;
            wpds_primitive_type_hash( sz );
            return sz;
        }
    };
    template<> struct hm_hash< const unsigned long >
    {
        wpds_size_t operator()( const unsigned long key ) const
        {
            wpds_size_t sz = key;
            wpds_primitive_type_hash( sz );
            return sz;
        }
    };
    template<> struct hm_equal< unsigned long >
    {
        bool operator()( unsigned long lhs,unsigned long rhs ) const
        {
            return lhs == rhs;
        }
    };
    template<> struct hm_equal< const unsigned long >
    {
        bool operator()( const unsigned long lhs,const unsigned long rhs ) const
        {
            return lhs == rhs;
        }
    };

    /********************/
    /* char*            */
    /********************/
    /* The hash function used was found at:
     *      http://www.cs.yorku.ca/~oz/hash.html
     *
     * The algorithm (djb2) is attributed to Dan Bernstein
     * on the web page.  It claims to have been posted on the
     * newsgroup comp.lang.c so I am assuming it is in the public
     * domain.
     */
    static unsigned long WPDS_CALL strhashfn( const char* str )
    {
        unsigned long hash = 5381;
        int c;

        while (0 != (c = *str++))
            hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

        return hash;
    }

    template<> struct hm_hash< char * >
    {
        wpds_size_t operator()( char * str ) const
        {
            return strhashfn( str );
        }
    };
    template<> struct hm_hash< const char * >
    {
        wpds_size_t operator()( const char * str ) const
        {
            return strhashfn( str );
        }
    };
    template<> struct hm_equal< char * >
    {
        bool operator()( char * lhs,char * rhs ) const
        {
            return (0 == strcmp(lhs,rhs));
        }
    };
    template<> struct hm_equal< const char * >
    {
        bool operator()( const char * lhs,const char * rhs ) const
        {
            return (0 == strcmp(lhs,rhs));
        }
    };

} // namespace wpds

#endif  // HASH_MAP_DEFAULTS_H_
/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
