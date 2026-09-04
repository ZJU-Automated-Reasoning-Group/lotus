#include "terms_intervals.hh"
#include <clam/CrabDomain.hh>
#include <clam/RegisterAnalysis.hh>
#include <clam/config.h>

namespace clam {
#ifdef INCLUDE_ALL_DOMAINS
REGISTER_DOMAIN(clam::CrabDomain::TERMS_INTERVALS, term_int_domain)
#else
UNREGISTER_DOMAIN(term_int_domain)
#endif
} // end namespace clam

