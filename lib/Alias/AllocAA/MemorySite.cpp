
/**
 * @file MemorySite.cpp
 * @brief Implementation of the MemorySiteInfo alias query.
 *
 * See MemorySite.h for the full design description and current stub status.
 *
 * Currently only `doesAlias` is implemented.  The site graph (allocCallSites,
 * allocaSites, referenceSites) is never populated, so `doesAlias` always
 * returns `AllocAAResult::May` in practice.
 */

#include "Alias/AllocAA/MemorySite.h"

AllocAAResult MemorySiteInfo::doesAlias(Value *V1, Value *V2) {

  // -------------------------------------------------------------------------
  // Case 1: Unknown value(s).
  //
  // If either value is not tracked in `referenceSites`, the analysis has no
  // information about it.  We conservatively return May.
  //
  // NOTE: Because the site graph is never built, this branch is always taken
  // in the current implementation, making the method always return May.
  // -------------------------------------------------------------------------
  auto ref1 = referenceSites.find(V1);
  if (ref1 == referenceSites.end())
    return AllocAAResult::May;
  auto ref2 = referenceSites.find(V2);
  if (ref2 == referenceSites.end())
    return AllocAAResult::May;

  auto *site1 = ref1->second;
  auto *site2 = ref2->second;

  // -------------------------------------------------------------------------
  // Case 2: Same site → Must alias.
  //
  // Both values map to the same MemorySite, meaning they are derived from the
  // same allocation.  They must alias (they refer to the same object, though
  // possibly at different offsets — offset disambiguation is not yet
  // implemented here).
  // -------------------------------------------------------------------------
  if (site1 == site2)
    return AllocAAResult::Must;

  // -------------------------------------------------------------------------
  // Case 3: Both sites have escaping values → May alias.
  //
  // If both sites have values that escaped to unknown contexts, we cannot
  // rule out that the escaped pointers were used to create an alias between
  // the two sites (e.g., by storing one site's pointer into the other).
  // Conservatively return May.
  // -------------------------------------------------------------------------
  if (site1->escapingValues.size() > 0 && site2->escapingValues.size() > 0)
    return AllocAAResult::May;

  // -------------------------------------------------------------------------
  // Case 4: Different sites, at least one fully understood → No alias.
  //
  // The two values map to different MemorySite objects.  At least one of the
  // sites has no escaping values, meaning all accesses to it are accounted
  // for.  Since the sites are distinct and at least one is fully understood,
  // the two values cannot alias.
  // -------------------------------------------------------------------------
  return AllocAAResult::No;
}
