//===-- Verification/Sifa/Caches/StarDagCache.cpp
//--------------------------===//
//
// Explicit instantiation for StarDagCache<Transition>.
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Caches/StarDagCache.h"

#include "Verification/Sifa/Cfg/Transition.h"

template class lotus::sifa::StarDagCache<lotus::sifa::Transition>;
