#include "sign_constant.hh"
#include <clam/CrabDomain.hh>
#include <clam/RegisterAnalysis.hh>
#include <clam/config.h>

namespace clam {
#ifdef INCLUDE_ALL_DOMAINS
REGISTER_DOMAIN(clam::CrabDomain::SIGN_CONSTANTS, sign_constant_domain)
#else
UNREGISTER_DOMAIN(sign_constant_domain)
#endif
} // end namespace clam

