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
 * $Id: HashMap.h,v 1.24 2007-03-30 22:27:43 chi-hua Exp $
 */
#ifndef _HASH_MAP_H_
#define _HASH_MAP_H_
#include <limits.h> // ULONG_MAX
#include <utility>  // std::pair
#include <functional>
#include "hm_hash.h"
#include "inst_counter.h"
#include "myallocator.h"
#define HASHMAP_GROWTH_FRACTION 0.75
#define HASHMAP_SHRINK_FRACTION 0.25

/*
 * TODO??:  make erase shrink the number of buckets
 * TODO??:  make GROWTH|HASHMAP_SHRINK_FRACTION member vars vs. static
 */

namespace wpds {

#if PI_STATS_DETAIL
    // totalHashMapnumBuckets of all HashMaps
    extern long totalHashMapnumBuckets;
#endif /* PI_STATS_DETAIL */

    // Pre decls
    template< typename Key,
    typename Data,
    typename HashFunc,
    typename EqualFunc > class HashMap;

    template< typename Key,
    typename Data,
    typename HashFunc,
    typename EqualFunc > class HashMapIterator;

    template< typename Key,
    typename Data,
    typename HashFunc,
    typename EqualFunc > struct HashMapConstIterator;

    template< typename Value > struct Bucket PI_STATS_INST_COUNT(Bucket< Value >)
    {
        Bucket( const Value& v, Bucket *n=0 )
            : value(v),next(n) {}
        Value value;
        Bucket *next;
    };

    /*
     * You should always use:
     *      HashMap< a,b,c,d >::iterator
     *              or
     *      HashMap< a,b,c,d >::const_iterator!!
     *
     * This keeps with abstraction.
     */
    template< typename Key,typename Data,typename HashFunc,typename EqualFunc >
    class HashMapIterator
    {
    public:
        typedef HashMapIterator< Key,Data,HashFunc,EqualFunc > iterator;
        typedef HashMap< Key,Data,HashFunc,EqualFunc > hashmap_type;
        typedef GPP_IMP_TYPENAME__ std::pair< Key,Data > pair_type;
        typedef pair_type value_type;
        typedef Bucket< value_type > bucket_type;
        typedef wpds_size_t size_type;

        HashMapIterator() : bucket(0),hashMap(0) {}
        HashMapIterator( bucket_type *bkt,hashmap_type *hmap )
            : bucket( bkt ),hashMap(hmap) {}

        inline value_type *operator->()
        {
            return &(bucket->value);
        }

        inline value_type& operator*()
        {
            return bucket->value;
        }

        inline bool operator==( const iterator& right )
        {
            return right.bucket == bucket;
        }

        inline bool operator!=( const iterator& right )
        {
            return right.bucket != bucket;
        }

        iterator operator++( int );
        iterator operator++( );

        bucket_type  *bucket;
        hashmap_type *hashMap;

    };

    template< typename Key,typename Data,typename HashFunc,typename EqualFunc >
    HashMapIterator< Key,Data,HashFunc,EqualFunc >
    HashMapIterator< Key,Data,HashFunc,EqualFunc >::operator++()
    {
        return this->operator++(3);
    }

    template< typename Key,typename Data,typename HashFunc,typename EqualFunc >
    HashMapIterator< Key,Data,HashFunc,EqualFunc >
    HashMapIterator< Key,Data,HashFunc,EqualFunc >::operator++(int)
    {
        bucket_type *old = bucket;
        bucket = bucket->next;
        if( !bucket ) {
            size_type bktNum = hashMap->bucketFromValue( old->value );
            while( !bucket && ++bktNum < hashMap->numBuckets ) {
                bucket = hashMap->buckets[bktNum];
            }
        }
        return *this;
    }

    template< typename Key,typename Data,typename HashFunc,typename EqualFunc >
    struct HashMapConstIterator
    {
        typedef HashMapIterator< Key,Data,HashFunc,EqualFunc > iterator;
        typedef HashMapConstIterator< Key,Data,HashFunc,EqualFunc > const_iterator;
        typedef HashMap< Key,Data,HashFunc,EqualFunc > hashmap_type;
        typedef std::pair< Key,Data > pair_type;
        typedef pair_type value_type;
        typedef Bucket< value_type > bucket_type;
        typedef wpds_size_t size_type;

        HashMapConstIterator() : bucket(0),hashMap(0) {}
        HashMapConstIterator( const bucket_type *bkt,const hashmap_type *hmap )
            : bucket( bkt ),hashMap(hmap) {}
        HashMapConstIterator( const iterator& it )
            : bucket( it.bucket ),hashMap( it.hashMap ) {}

        inline const value_type *operator->()
        {
            return &(bucket->value);
        }

        inline const value_type& operator*()
        {
            return bucket->value;
        }

        inline bool operator==( const const_iterator& right )
        {
            return right.bucket == bucket;
        }

        inline bool operator!=( const const_iterator& right )
        {
            return right.bucket != bucket;
        }

        const_iterator operator++( int );
        const_iterator operator++( );

        const bucket_type  *bucket;
        const hashmap_type *hashMap;
    };

    template< typename Key,typename Data,typename HashFunc,typename EqualFunc >
    HashMapConstIterator< Key,Data,HashFunc,EqualFunc >
    HashMapConstIterator< Key,Data,HashFunc,EqualFunc >::operator++()
    {
        return this->operator++(3);
    }

    template< typename Key,typename Data,typename HashFunc,typename EqualFunc >
    HashMapConstIterator< Key,Data,HashFunc,EqualFunc >
    HashMapConstIterator< Key,Data,HashFunc,EqualFunc >::operator++(int)
    {
        const bucket_type *old = bucket;
        bucket = bucket->next;
        if( !bucket ) {
            size_type bktNum = hashMap->bucketFromValue( old->value );
            while( !bucket && ++bktNum < hashMap->numBuckets ) {
                bucket = hashMap->buckets[bktNum];
            }
        }
        return *this;
    }

    /*
     * class HashMap
     *
     * Notes:
     *      equal_to is part of the STL.
     */
#define COMMA_in_macro_invocation ,
    template< 
        typename Key,
        typename Data,
        typename HashFunc = hm_hash< Key >,
        typename EqualFunc = hm_equal< Key > >
        class HashMap PI_STATS_INST_COUNT(HashMap<Key COMMA_in_macro_invocation Data COMMA_in_macro_invocation HashFunc COMMA_in_macro_invocation EqualFunc >)
#undef COMMA_in_macro_invocation
    {
        // friend iterator
        friend class HashMapIterator<Key,Data,HashFunc,EqualFunc>;
        friend struct HashMapConstIterator<Key,Data,HashFunc,EqualFunc>;

        public:     // typedef
            typedef std::pair< Key,Data >   pair_type;
            typedef pair_type               value_type;
            typedef Bucket< value_type >    bucket_type;
            // WARNING: This must also be changed in the iterators
            typedef wpds_size_t size_type;
            enum enum_size_type_max { SIZE_TYPE_MAX = ULONG_MAX };
            // static const size_type SIZE_TYPE_MAX = ULONG_MAX;
            typedef HashMapIterator< Key,Data,HashFunc,EqualFunc> iterator;
            typedef HashMapConstIterator< Key,Data,HashFunc,EqualFunc> const_iterator;

        public:     // con/destructor
            HashMap( size_type _size=47 )
                : numValues(0),numBuckets(_size)
                , growthFactor( _size * HASHMAP_GROWTH_FRACTION )
                , shrinkFactor( _size * HASHMAP_SHRINK_FRACTION )
                { initBuckets(); }

            HashMap& operator=( const HashMap& hm )
            {
                clear();
                releaseBuckets();
                numBuckets = hm.capacity();
                initBuckets();
                for( const_iterator it = hm.begin() ; it != hm.end() ; it++ ) {
                    insert(it->first,it->second);
                }
                return *this;
            }

            ~HashMap() {
                clear();
                releaseBuckets();
            }

        public:     // inline methods
            inline size_type size() const
            {
                return numValues;
            }

            inline size_type capacity() const
            {
                return numBuckets;
            }

            inline std::pair<iterator,bool> insert( const Key& k, const Data& d )
            {
                return insert( pair_type(k,d) );
            }

            void erase( const Key& key )
            {
                iterator it = find( key );
                if( it != end() )
                    erase( it );
            }

            void clear()
            {
                for(size_type i=0;i<numBuckets;i++) {
                    while( buckets[i] ) {
                        bucket_type *tmp = buckets[i];
                        buckets[i] = tmp->next;
                        releaseBucket( tmp );
                    }
                }
            }

            iterator begin()
            {
                for( size_type n = 0 ; n < numBuckets ; n++ )
                    if( buckets[n] )
                        return iterator(buckets[n],this);
                return end();
            }

            inline iterator end()
            {
                return iterator( 0,this );
            }

            const_iterator begin() const
            {
                for( size_type n = 0 ; n < numBuckets ; n++ )
                    if( buckets[n] )
                        return const_iterator(buckets[n],this);
                return end();
            }

            inline const_iterator end() const
            {
                return const_iterator(0,this);
            }

            void print_stats( std::ostream& o = std::cout ) const 
            {
                size_type i= 0;
                int activebuckets= 0;
                int bucket_count=0;
                int max_bucket_count=0;
                int min_bucket_count = 10000000;
                bucket_type *bkt;
                for( ; i< numBuckets; i++ ) {
                    bkt = buckets[i];
                    if( bkt ) {
                        activebuckets++;
                        bucket_count = 0;
                        while( bkt ) {
                            bucket_count++;
                            bkt = bkt->next;
                        }
                        if( bucket_count > max_bucket_count )
                            max_bucket_count = bucket_count;
                        if( bucket_count < min_bucket_count )
                            min_bucket_count = bucket_count;
                    }
                }
                o << "Stats:\n";
                o << "\tNumber of Values   : " << numValues << std::endl;
                o << "\tNumber of Buckets  : " << numBuckets << std::endl;
                o << "\tActive buckets     : " << activebuckets << std::endl;
                o << "\tAverage bucket size: " << (numValues / activebuckets)  << std::endl;
                o << "\tMax bucket count   : " << max_bucket_count << std::endl;
                o << "\tMin bucket count   : " << min_bucket_count << std::endl;
            }

        public:     // methods
            std::pair<iterator,bool> insert( const value_type& );
            iterator find( const Key& );
            const_iterator find( const Key& ) const;
            void erase( iterator it );
            Data & operator[](const Key & k) {
              return (*((insert(value_type(k, Data()))).first)).second;
            }

        public:     // vars

        private:    // inline methods
            /*************  Get Bucket location from various objects **********/
            inline size_type _bucketFromHash( size_type hash, size_type _size ) const
            {
                return hash % _size;
            }
            inline size_type _bucketFromKey( const Key& key,size_type _size ) const
            {
                return _bucketFromHash( hashFunc(key),_size );
            }
            inline size_type _bucketFromValue( const value_type& v,size_type s ) const
            {
                return _bucketFromKey( v.first,s );
            }
            inline size_type bucketFromHash( size_type hash ) const
            {
                return _bucketFromHash( hash,numBuckets );
            }
            inline size_type bucketFromKey( const Key& key ) const
            {
                return bucketFromHash( hashFunc(key) );
            }
            inline size_type bucketFromValue( const value_type& value ) const
            {
                return bucketFromKey( value.first );
            }

            /************ Initialize and release buckets *************************/
            inline void initBuckets()
            {
                buckets = _makeBuckets( numBuckets );
            }

            inline bucket_type **  _makeBuckets( size_type _size )
            {
#if PI_STATS_DETAIL
                totalHashMapnumBuckets += _size;
#endif /* PI_STATS_DETAIL */
                bucket_type **bktptr = new bucket_type*[_size];
                memset(bktptr,0,sizeof(bucket_type*)*_size);
                //for(size_type s = 0;s<_size;s++)
                //bktptr[s] = 0;
                return bktptr;
            }

            inline void releaseBuckets()
            {
#if PI_STATS_DETAIL
                totalHashMapnumBuckets -= numBuckets;
#endif /* PI_STATS_DETAIL */
                delete[] buckets;
            }

            inline void releaseBucket( bucket_type *bkt )
            {
                delete bkt;
            }

        private:    // methods
            void resize( size_type size );

        private:    // variables
            bucket_type **buckets;
            size_type numValues;
            size_type numBuckets;
            double growthFactor;
            double shrinkFactor;
            HashFunc hashFunc;
            EqualFunc equalFunc;
    };

    template< typename Key,typename Data,typename HashFunc,typename EqualFunc >
    HashMapIterator< Key,Data,HashFunc,EqualFunc >
    HashMap< Key,Data,HashFunc,EqualFunc>::find( const Key& key )
    {
        size_type bktNum = bucketFromKey( key );
        for( bucket_type *bkt = buckets[bktNum]; bkt; bkt = bkt->next )
            if( equalFunc( key,bkt->value.first) )
                return iterator( bkt,this );
        return end();
    }
    
    template< typename Key,typename Data,typename HashFunc,typename EqualFunc >
    HashMapConstIterator< Key,Data,HashFunc,EqualFunc >
    HashMap< Key,Data,HashFunc,EqualFunc>::find( const Key& key ) const
    {
        size_type bktNum = bucketFromKey( key );
        for( bucket_type *bkt = buckets[bktNum]; bkt; bkt = bkt->next )
            if( equalFunc( key,bkt->value.first) )
                return const_iterator( bkt,this );
        return end();
    }

    template< typename Key,typename Data,typename HashFunc,typename EqualFunc >
    void HashMap<Key,Data,HashFunc,EqualFunc>::erase(
            typename HashMap<Key,Data,HashFunc,EqualFunc>::iterator it )
    {
        /* We can't just erase the bucket in the iterator b/c we
         * need to fix the "pointers" in the bucket list.
         *
         * ie   prev -> it.bkt -> next  ==> prev -> next
         *
         * This would be simple if we were using doubly linked list..
         */
        bucket_type *bkt = it.bucket;
        if( bkt ) {
            // get the first bucket in chain
            size_type bktNum = bucketFromValue( it.bucket->value );
                bucket_type *cur = buckets[bktNum];
                if( cur == bkt ) {
                    buckets[bktNum] = cur->next;
                    releaseBucket( cur );
                    numValues--;
                }
                else {
                    bucket_type *next = cur->next;
                    while( next ) {
                        if( next == bkt ) {
                            cur->next = next->next;
                            releaseBucket( next );
                            numValues--;
                            break;
                        }
                        else {
                            cur = next;
                            next = cur->next;
                        }
                    }
                }
        }
    }

    template< typename Key,typename Data,typename HashFunc,typename EqualFunc >
    std::pair< HashMapIterator< Key,Data,HashFunc,EqualFunc >,bool >
    HashMap<Key,Data,HashFunc,EqualFunc>::insert( const value_type& value )
    {
        typedef std::pair< iterator,bool > RPair;
        resize( numValues+1 );
        size_type bktNum = bucketFromValue( value );
        bucket_type *bktptr = buckets[bktNum];
        for( bucket_type *tmp = bktptr; tmp ; tmp = tmp->next ) {
            if( equalFunc( value.first,tmp->value.first ) ) {
                return RPair( iterator(tmp,this),false );
            }
        }
        bktptr = new bucket_type( value,bktptr );
        buckets[bktNum] = bktptr;
        numValues++;
        return RPair( iterator(bktptr,this),true );
    }

    template< typename Key,typename Data,typename HashFunc,typename EqualFunc >
    void HashMap<Key,Data,HashFunc,EqualFunc>::resize( size_type needed )
    {
        if( needed < growthFactor )
            return;
        size_type new_size = numBuckets * 2;
        if( new_size >= SIZE_TYPE_MAX )
            return;

#ifdef DBGHASHMAP
        printf("DBG HashMap : Resizing to %lu buckets\n",new_size);
        print_stats(std::cout);
#endif

        // alloc new array of buckets
        bucket_type **tmp = _makeBuckets( new_size );

        // copy from old array to new
        size_type newBktNum;
        for(size_type i=0;i<numBuckets;i++) {
            /*
            // is this ok?
            if( buckets[i] ) {
            bucket_type *bktptr = buckets[i];
            newBktNum = _bucketFromValue( bktptr->value,new_size );
            tmp[newBktNum] = bktptr;
            buckets[i] = 0;
            }
             */
            while( buckets[i] ) {
                bucket_type *old = buckets[i];
                // get new bucket num from hashing
                newBktNum = _bucketFromValue( old->value,new_size );
                // store old's next ptr
                buckets[i] = old->next;
                // set up old's new bktNum
                old->next = tmp[newBktNum];
                // put buck in new array
                tmp[newBktNum] = old;
            }
        }

        // release old array
        releaseBuckets();

        // set buckets to new array
        buckets = tmp;
        numBuckets = new_size;
        // set up new growth|shrink factors
        growthFactor = numBuckets * HASHMAP_GROWTH_FRACTION;
        shrinkFactor = numBuckets * HASHMAP_SHRINK_FRACTION;
    }

} // namespace wpds
#endif  // _HASH_MAP_H_
/* Yo, Emacs!
;;; Local Variables: ***
;;; tab-width: 4 ***
;;; End: ***
*/
