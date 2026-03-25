#pragma once

#include <cstdint>

namespace dynamic {

enum AllocType : std::uint8_t { Global = 0, Stack, Heap };

} // namespace dynamic