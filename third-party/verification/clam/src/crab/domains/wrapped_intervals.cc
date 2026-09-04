#include "wrapped_intervals.hh"
#include <clam/CrabDomain.hh>
#include <clam/RegisterAnalysis.hh>
#include <clam/config.h>

namespace clam {
#ifdef INCLUDE_ALL_DOMAINS
REGISTER_DOMAIN(clam::CrabDomain::WRAPPED_INTERVALS, wrapped_interval_domain)
#else
UNREGISTER_DOMAIN(wrapped_interval_domain)
#endif
} // end namespace clam

