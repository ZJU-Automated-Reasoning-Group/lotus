#include "pk_pplite.hh"
#include <clam/CrabDomain.hh>
#include <clam/RegisterAnalysis.hh>
#include <clam/config.h>
#include <crab/config.h>

namespace clam {
#ifdef INCLUDE_ALL_DOMAINS
#ifdef HAVE_PPLITE
REGISTER_DOMAIN(clam::CrabDomain::PK_PPLITE, pk_pplite_domain)
#else
UNREGISTER_DOMAIN(pk_pplite_domain)
#endif
#else
UNREGISTER_DOMAIN(pk_pplite_domain)
#endif
} // end namespace clam
  
