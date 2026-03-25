//===-- Verification/Sifa/Caches/TopsortCache.cpp ------------------------===//
//
// Explicit instantiation for TopsortCache<Transition>.
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Caches/TopsortCache.h"

#include "Verification/Sifa/Cfg/Transition.h"

template class lotus::sifa::TopsortCache<lotus::sifa::Transition>;
