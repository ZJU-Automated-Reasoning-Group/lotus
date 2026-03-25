#include "Verification/SymAbsAI/Core/RepresentedValue.h"

#include "Verification/SymAbsAI/Core/repr.h"

#include <iostream>

namespace symabs_ai {
std::ostream &operator<<(std::ostream &out, const RepresentedValue &value) {
  return out << repr(value);
}
} // namespace symabs_ai
