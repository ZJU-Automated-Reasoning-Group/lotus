#pragma once

#include "Alias/TPA/PointerAnalysis/Support/PtsMap.h"

namespace tpa {

class MemoryObject;

// Memory-level points-to store.
//
// Store models memory cell contents: each abstract memory object is mapped to
// the set of memory objects that may be loaded from that cell. This is the
// "heap/memory" layer of the analysis and is propagated through mem-level CFG
// edges by the worklist engine.
using Store = PtsMap<const MemoryObject *>;

} // namespace tpa
