#pragma once

#include "CFL/InterleavedDyck/Unary/Adaptive.h"
#include "CFL/InterleavedDyck/Unary/FixedCounter.h"

namespace lotus::cfl::interleaved_dyck::unary {

/// Exact algorithms available for bidirected unary D1-interleaved-D1 graphs.
enum class Algorithm {
  Adaptive,
  FixedCounter,
};

} // namespace lotus::cfl::interleaved_dyck::unary
