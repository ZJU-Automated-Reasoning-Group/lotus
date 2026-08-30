#pragma once

#include "CFL/UnaryInterleavedDyck/Adaptive.h"
#include "CFL/UnaryInterleavedDyck/FixedCounter.h"

namespace lotus::cfl::unary_interleaved_dyck {

/// Exact algorithms available for bidirected unary D1-interleaved-D1 graphs.
enum class Algorithm {
  Adaptive,
  FixedCounter,
};

} // namespace lotus::cfl::unary_interleaved_dyck
