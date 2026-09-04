#include "pk.hh"
#include <clam/CrabDomain.hh>
#include <clam/RegisterAnalysis.hh>
#include <clam/config.h>
#include <crab/config.h>

namespace clam {
#ifdef INCLUDE_ALL_DOMAINS
#if defined(HAVE_APRON) || defined(HAVE_ELINA)
REGISTER_DOMAIN(clam::CrabDomain::PK, pk_domain)
#else
UNREGISTER_DOMAIN(pk_domain)
#endif
#else
UNREGISTER_DOMAIN(pk_domain)
#endif
} // end namespace clam
  
