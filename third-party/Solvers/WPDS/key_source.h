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

#ifndef KEY_SOURCE_DEFINED
#define KEY_SOURCE_DEFINED 1

#include <iostream>
#include <string>
#include "common.h"
#include "inst_counter.h"
#include "hm_hash.h"

/* FIXME: because dynamic cast is not working */
class pdg_vertex_src;
class ccfg_vertex_src;
class scm_src;

namespace wpds
{
    /*  Class key_source is an abstract class for representing items that can
        be turned into wpds_keys.
     */

    class string_src;       /* FIXME: dynamic_cast work around */
    class int_src;
    class key_pair_src;

    class key_source PI_STATS_INST_COUNT(key_source) {
    public:
      // Need this virtual destructor because str in string_src needs
      // to be freed.
      virtual ~key_source(){}
      /* FIXME: because dynamic cast is not working */
      virtual const pdg_vertex_src * cast_pdg_vertex_src() const { return 0; }
      virtual const ccfg_vertex_src * cast_ccfg_vertex_src() const { return 0; }
      virtual const scm_src * cast_scm_src() const { return 0; }
      virtual const string_src * cast_string_src() const { return 0; }
      virtual const int_src * cast_int_src() const { return 0; }
      virtual const key_pair_src * cast_key_pair_src() const { return 0; }

        virtual wpds_size_t hash(void) const = 0;
        //virtual int compare(const key_source *) const = 0;
        virtual int equal(const key_source *o) const = 0;
        virtual void show(std::string &s) { s = "?"; }
        virtual std::ostream& print(std::ostream& out) { return out << "?"; }; 
    };

    /* Class string_src is a subclass of the abstract class key_source.
       This allows strings to be turned into wpds_keys.
     */
    class string_src : public key_source PI_STATS_INST_COUNTm(string_src, key_source) {
    public:
        char *str;
        string_src(const char *s)
        {
            str = (char*)malloc(strlen(s) + 1);
            memcpy(str, s, strlen(s) + 1);
        }
        virtual const string_src * cast_string_src() const { return this; }
        virtual ~string_src() { if (str) free(str); }
        inline wpds_size_t hash(void) const
        {
            static hm_hash<char *> hh;
            return hh(str);
        }
        int equal(const key_source *item) const
        {
            const string_src *ss = item->cast_string_src();
            //dynamic_cast<const string_src *>(item);
            if (ss)
                return 0 == strcmp(str, ss->str);
            else
                return 0;
        }
        std::ostream& print(std::ostream& out)
        {
            return out << str;
        }
        void show(std::string &s)
        {
            s = str;
        }
                
    };

    class int_src : public key_source PI_STATS_INST_COUNTm(int_src, key_source)
    {
    public:
        int value;
        int_src(int x)
        {
            value = x;
        }
        virtual const int_src * cast_int_src() const { return this; }
        wpds_size_t hash(void) const
        {
            static hm_hash<int> hasher;
            return hasher(value);
        }
        int equal(const key_source *item) const
        {
            const int_src *is = item->cast_int_src();
            //dynamic_cast<const int_src *>(item);
            if (is)
                return value == is->value;
            else
                return 0;
        }
        std::ostream& print(std::ostream& out)
        {
            return out << value;
        }
        void show(std::string& s)
        {
            char x[20];
            snprintf(x, sizeof(x), "%d", value);
            s = x;
        }
    };

    class key_pair_src : public key_source PI_STATS_INST_COUNTm(key_pair_src, key_source)
    {
    public:
        unsigned long k1, k2;
        key_pair_src(unsigned long _k1, unsigned long _k2) 
            : k1(_k1), k2(_k2) 
            { }
        virtual const key_pair_src * cast_key_pair_src() const { return this; }
        wpds_size_t hash(void)const
        {
            static hm_hash<int> hasher;
            return (hasher(k2) << 3) ^ hasher(k1);
        }
        int equal(const key_source *item) const
        {
            const key_pair_src *kps = item->cast_key_pair_src();
            if( kps )
                return k1 == kps->k1  &&  k2 == kps->k2;
            else
                return 0;
        }

        std::ostream & print( std::ostream & out)
        {
            std::string s;
            show(s);
            out << s;
            return out;
        }
        void show(std::string & s);
    };

    template <> struct hm_hash<key_source *>
    {
        wpds_size_t operator()(key_source *ks) const
        {
            return ks->hash();
        }
    };

    template <> struct hm_hash<const key_source *>
    {
        wpds_size_t operator()(const key_source *ks) const
        {
            return ks->hash();
        }
    };

    template <> struct hm_equal<key_source *>
    {
        bool operator()(key_source *lhs, key_source *rhs) const
        {
            return lhs->equal(rhs) != 0;
        }
    };

    template <> struct hm_equal<const key_source *>
    {
        bool operator()(const key_source *lhs, const key_source *rhs) const
        {
            return lhs->equal(rhs) != 0;
        }
    };


};

#endif /* KEY_SOURCE_DEFINED */

/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
