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

#ifndef WPDS_TRAITS_GUARD
#define WPDS_TRAITS_GUARD 1

#include <list>
#include <set>
#include "ref_ptr.h"
#include "myallocator.h"
#include "common.h"

/*! @struct wpds::Traits
 *
 * @brief Traits is a common place to put typedefs
 *
 * This struct defines typedefs used by WPDS++.
 * The typedefs RuleSet and TransSet are deprecated
 * and will be removed if future version of WPDS++
 *
 * @see Rule
 * @see CATransition
 * @see Semiring
 * @see ref_ptr
 */

namespace wpds
{
    template< typename T > class CATransition;
    template< typename T > class Rule;

    template< typename T > struct Traits
    {  
        /*! the W of WPDS */
        typedef ref_ptr<T>                      sem_elem_t;
        /*! the W of WPDS */
        typedef ref_ptr<T>                      weight_t;
        /*! reference counted transition */
        typedef ref_ptr< CATransition<T> >      catrans_t;
        /*! reference counted transition */
        typedef ref_ptr< CATransition<T> >      trans_t;
        /*! a rule */
        typedef Rule<T> *                       rule_t;   
        /*! a const rule */
        typedef const Rule<T> *                 const_rule_t;

        /*! set of keys */
        typedef STDSET( wpds_key_t )            KeySet;
        /*! iterator over KeySet */
        typedef GPP_IMP_TYPENAME__ KeySet::iterator KeySetIter;

        /*! a List of rules */
        typedef STDLIST( rule_t )               RuleList;
        typedef GPP_IMP_TYPENAME__ RuleList::iterator RuleListIter;
        typedef std::pair< RuleListIter,RuleListIter > RuleListIterPair;
        typedef GPP_IMP_TYPENAME__ RuleList::const_iterator RuleListConstIter;
        typedef std::pair< RuleListConstIter,RuleListConstIter > RuleListConstIterPair;

        
        /*! typedefs for const rule lists */
        typedef STDLIST( const_rule_t )         ConstRuleList;
        typedef GPP_IMP_TYPENAME__ ConstRuleList::iterator ConstRuleListIter;
        typedef std::pair< ConstRuleListIter,ConstRuleListIter > ConstRuleListIterPair;
        typedef GPP_IMP_TYPENAME__ ConstRuleList::const_iterator ConstRuleListConstIter;
        typedef std::pair< ConstRuleListConstIter,ConstRuleListConstIter > ConstRuleListConstIterPair;

        typedef STDLIST( trans_t )              trans_list_t;
        /*! a list of trans_t @see trans_t */
        typedef STDLIST( trans_t )              TransList;
        typedef GPP_IMP_TYPENAME__ TransList::iterator TransListIter;
        typedef GPP_IMP_TYPENAME__ TransList::const_iterator TransListConstIter;
        typedef std::pair< TransListIter,TransListIter > TransListIterPair;
        typedef std::pair< TransListConstIter,TransListConstIter > TransListConstIterPair;

        /*! 
         * TransSet is shortname for a list of trans_t.These are deprecated.
         * Use TransList
         *
         * @see trans_t
         * @see TransList
         */
        typedef STDLIST( trans_t )              TransSet;
        typedef GPP_IMP_TYPENAME__ TransSet::iterator TransSetIter;
        typedef GPP_IMP_TYPENAME__ TransSet::const_iterator TransSetConstIter;
        typedef std::pair< TransSetIter,TransSetIter > TransSetIterPair;
        typedef std::pair< TransSetConstIter,TransSetConstIter > TransSetConstIterPair;

    };

    /*********************************************************************
     * Begin Witness Traits                                              *
     *********************************************************************/

#if defined(GENERIC_WITNESSES)
    template< typename T,typename U,typename V > class TWitness;
    template< typename T,typename U,typename V > class TWitnessApply;
    template< typename T,typename U,typename V > class TWitnessCombine;
    template< typename T,typename U,typename V > class TWitnessEps;

    template< typename T > struct WitnessTraits
    {
        typedef GPP_IMP_TYPENAME__ Traits<T>::catrans_t                 catrans_t; 
        typedef GPP_IMP_TYPENAME__ Traits<T>::sem_elem_t                sem_elem_t; 
        typedef GPP_IMP_TYPENAME__ Traits<T>::const_rule_t              const_rule_t; 
        typedef TWitness< catrans_t,sem_elem_t,const_rule_t >           TransWitness;
        typedef TWitnessApply< catrans_t,sem_elem_t,const_rule_t >      TransWitnessApply;
        typedef TWitnessCombine< catrans_t,sem_elem_t,const_rule_t >    TransWitnessCombine;
        typedef TWitnessEps< catrans_t,sem_elem_t,const_rule_t >        TransWitnessEps;
        typedef ref_ptr< TransWitness >                                 witness_t;
        typedef STDLIST( witness_t )                                    witness_path_t;
    };

#else
    template< typename T > class Witness;
    template< typename T > class WitnessEpsContraction;
    template< typename T > class WitnessAppl;
    template< typename T > class WitnessCombine;
    template< typename T > class CAPathOfWitness;

    template< typename T > struct WitnessTraits
    {
        typedef Witness<T>                      TransWitness;
        typedef WitnessEpsContraction<T>        TransWitnessEps;
        typedef WitnessAppl<T>                  TransWitnessApply;
        typedef WitnessCombine<T>               TransWitnessCombine;
        typedef ref_ptr< TransWitness >         witness_t;
        typedef ref_ptr< CAPathOfWitness<T> >   witness_path_t;
    };

#endif

} // namespace wpds

    /***********************************************************************
     * Begin Macro Traits                                                  *
     ***********************************************************************/

    /*!
     * WPDS_TYPEDEFS is a macro that when used will locally declare
     * typedefs used by WPDS++. 
     */
#define WPDS_TYPEDEFS(TYPE) \
    __WPDS_BASE_TDS__(TYPE); \
    __WPDS_RULE_LIST_TDS__(TYPE); \
    __WPDS_TRANS_LIST_TDS__(TYPE);

    /*!
     * GEN_WPDS_TYPEDEFS is a macro that when used will locally declare
     * typedefs used by WPDS++.  This macro makes sure to insert
     * the "typename" keyword if you need the typedefs inside of templated
     * (generic) class or function. 
     */
#define GEN_WPDS_TYPEDEFS(TYPE) \
    __G_WPDS_BASE_TDS__(TYPE); \
    __G_WPDS_RULE_LIST_TDS__(TYPE); \
    __G_WPDS_TRANS_LIST_TDS__(TYPE);

#define __WPDS_BASE_TDS__(TYPE) \
    typedef ::wpds::Traits<TYPE>::sem_elem_t sem_elem_t; \
    typedef ::wpds::Traits<TYPE>::weight_t weight_t; \
    typedef ::wpds::Traits<TYPE>::catrans_t catrans_t; \
    typedef ::wpds::Traits<TYPE>::trans_t trans_t; \
    typedef ::wpds::Traits<TYPE>::rule_t rule_t; \
    typedef ::wpds::Traits<TYPE>::const_rule_t const_rule_t; \
    typedef ::wpds::WitnessTraits<TYPE>::TransWitness TransWitness; \
    typedef ::wpds::WitnessTraits<TYPE>::TransWitnessApply TransWitnessApply; \
    typedef ::wpds::WitnessTraits<TYPE>::TransWitnessCombine TransWitnessCombine; \
    typedef ::wpds::WitnessTraits<TYPE>::TransWitnessEps TransWitnessEps; \
    typedef ::wpds::WitnessTraits<TYPE>::witness_t witness_t; \
    typedef ::wpds::WitnessTraits<TYPE>::witness_path_t witness_path_t;

#define __WPDS_RULE_LIST_TDS__(TYPE) \
    typedef ::wpds::Traits<TYPE>::RuleList RuleList; \
    typedef ::wpds::Traits<TYPE>::RuleListIter RuleListIter; \
    typedef ::wpds::Traits<TYPE>::RuleListIterPair RuleListIterPair; \
    typedef ::wpds::Traits<TYPE>::RuleListConstIter RuleListConstIter; \
    typedef ::wpds::Traits<TYPE>::RuleListConstIterPair RuleListConstIterPair; \
    typedef ::wpds::Traits<TYPE>::ConstRuleList ConstRuleList; \
    typedef ::wpds::Traits<TYPE>::ConstRuleListIter ConstRuleListIter; \
    typedef ::wpds::Traits<TYPE>::ConstRuleListIterPair ConstRuleListIterPair; \
    typedef ::wpds::Traits<TYPE>::ConstRuleListConstIter ConstRuleListConstIter; \
    typedef ::wpds::Traits<TYPE>::ConstRuleListConstIterPair ConstRuleListConstIterPair;

#define __WPDS_TRANS_LIST_TDS__(TYPE) \
    typedef ::wpds::Traits<TYPE>::trans_list_t trans_list_t; \
    typedef ::wpds::Traits<TYPE>::TransList TransList; \
    typedef ::wpds::Traits<TYPE>::TransListIter TransListIter; \
    typedef ::wpds::Traits<TYPE>::TransListConstIter TransListConstIter; \
    typedef ::wpds::Traits<TYPE>::TransListIterPair TransListIterPair; \
    typedef ::wpds::Traits<TYPE>::TransListConstIterPair TransListConstIterPair; \
    typedef ::wpds::Traits<TYPE>::TransSet TransSet; \
    typedef ::wpds::Traits<TYPE>::TransSetIter TransSetIter; \
    typedef ::wpds::Traits<TYPE>::TransSetConstIter TransSetConstIter; \
    typedef ::wpds::Traits<TYPE>::TransSetIterPair TransSetIterPair; \
    typedef ::wpds::Traits<TYPE>::TransSetConstIterPair TransSetConstIterPair;

#define __G_WPDS_BASE_TDS__(TYPE) \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::sem_elem_t sem_elem_t; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::weight_t weight_t; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::catrans_t catrans_t; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::trans_t trans_t; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::rule_t rule_t; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::const_rule_t const_rule_t; \
    typedef GPP_IMP_TYPENAME__ ::wpds::WitnessTraits<TYPE>::TransWitness TransWitness; \
    typedef GPP_IMP_TYPENAME__ ::wpds::WitnessTraits<TYPE>::TransWitnessApply TransWitnessApply; \
    typedef GPP_IMP_TYPENAME__ ::wpds::WitnessTraits<TYPE>::TransWitnessCombine TransWitnessCombine; \
    typedef GPP_IMP_TYPENAME__ ::wpds::WitnessTraits<TYPE>::TransWitnessEps TransWitnessEps; \
    typedef GPP_IMP_TYPENAME__ ::wpds::WitnessTraits<TYPE>::witness_t witness_t; \
    typedef GPP_IMP_TYPENAME__ ::wpds::WitnessTraits<TYPE>::witness_path_t witness_path_t;

#define __G_WPDS_RULE_LIST_TDS__(TYPE) \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::RuleList RuleList; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::RuleListIter RuleListIter; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::RuleListIterPair RuleListIterPair; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::RuleListConstIter RuleListConstIter; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::RuleListConstIterPair RuleListConstIterPair; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::ConstRuleList ConstRuleList; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::ConstRuleListIter ConstRuleListIter; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::ConstRuleListIterPair ConstRuleListIterPair; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::ConstRuleListConstIter ConstRuleListConstIter; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::ConstRuleListConstIterPair ConstRuleListConstIterPair;

#define __G_WPDS_TRANS_LIST_TDS__(TYPE) \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::trans_list_t trans_list_t; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::TransList TransList; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::TransListIter TransListIter; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::TransListConstIter TransListConstIter; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::TransListIterPair TransListIterPair; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::TransListConstIterPair TransListConstIterPair; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::TransSet TransSet; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::TransSetIter TransSetIter; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::TransSetConstIter TransSetConstIter; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::TransSetIterPair TransSetIterPair; \
    typedef GPP_IMP_TYPENAME__ ::wpds::Traits<TYPE>::TransSetConstIterPair TransSetConstIterPair;

#endif  // WPDS_TRAITS_GUARD
