#include "terms_zones.hh"
#include <clam/CrabDomain.hh>
#include <clam/RegisterAnalysis.hh>
#include <clam/config.h>

namespace clam {
#ifdef INCLUDE_ALL_DOMAINS
REGISTER_DOMAIN(clam::CrabDomain::TERMS_ZONES, num_domain)
#else
UNREGISTER_DOMAIN(num_domain)
#endif
} // end namespace clam
