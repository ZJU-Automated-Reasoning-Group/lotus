#pragma once

#include "Alias/TPA/PointerAnalysis/Support/PtsMap.h"

namespace tpa {

class Pointer;

// Top-level points-to environment.
//
// Env tracks pointer variables (SSA values, parameters, globals, temporaries)
// and maps each pointer to the set of memory objects it may reference. This is
// the "register-like" layer of the analysis.
//
// Memory dereference effects are modeled separately in Store, while transfer
// evaluation may update Env from both top-level and store-carrying nodes.
using Env = PtsMap<const Pointer *>;

} // namespace tpa
