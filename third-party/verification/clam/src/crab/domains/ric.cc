#include "ric.hh"
#include <clam/CrabDomain.hh>
#include <clam/RegisterAnalysis.hh>
#include <clam/config.h>

namespace clam {
#ifdef INCLUDE_ALL_DOMAINS
REGISTER_DOMAIN(clam::CrabDomain::INTERVALS_CONGRUENCES, ric_domain)
#else
UNREGISTER_DOMAIN(ric_domain)
#endif
} // end namespace clam
