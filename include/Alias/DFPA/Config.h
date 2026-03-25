#pragma once

#include <cstddef>

namespace llvm {
class Function;
} // namespace llvm

namespace dfpa {

struct DFPAConfig {
  unsigned indirect_ctx_k = 1;
  bool refine_ambiguous_only = true;
  std::size_t max_demand_states = 50000;
  unsigned max_offset_depth = 8;
  bool enable_signature_filter = true;
};

} // namespace dfpa
